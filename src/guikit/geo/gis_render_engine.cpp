#include "gis_render_internal.hpp"
#include "guikit/geo/polygon_triangulator.hpp"
#include "keel/algo.h"
#include "keel/format.h"
#include "keel/math.h"
#include "keel/memory.h"
#include "keel/small_array.h"
#include "keel/time.h"

namespace guikit::geo {

using namespace detail;

void GisRenderEngine::InvalidateTileCache() {
    GetAsyncStreamer().ClearAndBumpEpoch();
    GetTileCache().Clear();
    GetTilePixelPool().Clear();
    GetDeclutterCache().Clear();
    GetCursorHitCache().Clear();
}

void GisRenderEngine::TrimInactiveCaches() noexcept {
    GetAsyncStreamer().TrimIdleMemory();
    GetTileCache().ReleaseMemory();
    GetTilePixelPool().Clear();
    GetDeclutterCache().ReleaseMemory();
    GetCursorHitCache().Clear();

    keel::Arc<IRasterTileProvider> defaultProv;
    keel::Arc<IRasterTileProvider> lastProv;
    {
        keel::LockGuard<keel::Mutex> lk(GetDefaultBasemapMutex());
        defaultProv = GetDefaultBasemapStorage();
        lastProv = keel::move(GetLastActiveBasemapStorage());
    }
    if (defaultProv) {
        defaultProv->ClearCache();
    }
    if (lastProv && lastProv.get() != defaultProv.get()) {
        lastProv->ClearCache();
    }
    keel::memory_trim();
}

bool GisRenderEngine::WaitForBackgroundStreamingIdle(keel::u32 timeoutMs) {
    return GetAsyncStreamer().WaitForIdle(timeoutMs);
}


GisRenderStats GisRenderEngine::RenderLayers(
    guikit::MapViewScope& map,
    keel::ArrayRef<geo::GeoLayer> layers,
    const GisRenderConfig& cfg,
    keel::u64& ioSelectedFeatureUid,
    bool lightMode
) {
    static keel::u64 s_frameCounter = 1;
    ++s_frameCounter;

    GisRenderStats stats{};
    guikit::SceneScope& scene = map.GetScene();
    const guikit::MapViewState& viewState = map.GetViewState();
    const guikit::GeoBounds visBounds = map.GetVisibleGeoBounds();
    const bool isPanning = scene.IsPanning();
    const bool useAsyncStreaming = cfg.asyncBackgroundStreaming && HasBackgroundWorkerThreads();

    const keel::f32 zoom = keel::max(0.0001f, scene.GetZoom());
    const keel::f32 invZ = 1.0f / zoom;
    const keel::f32 minSegWorld = cfg.lodSimplification ? (cfg.lodPixelTolerance * invZ) : 0.0f;
    const keel::f32 minSegWorldSq = minSegWorld * minSegWorld;
    const keel::i32 tileZoom = viewState.tileZoom;
    GetAsyncStreamer().SetActiveTileZoom(tileZoom);

    const keel::u32 configuredTileSizePx = ResolveTileSizePx(cfg.tileSizePx);
    const keel::Arc<geo::IRasterTileProvider> activeBasemapProvider = ResolveActiveBasemapProvider(cfg);
    const keel::StringRef basemapSourceId = activeBasemapProvider
        ? activeBasemapProvider->GetSourceId()
        : keel::StringRef{};
    const keel::u8 basemapOpacityByte = activeBasemapProvider
        ? keel::saturate<keel::u8>(keel::round_i32(keel::clamp(cfg.basemapOpacity, 0.0f, 1.0f) * 255.0f))
        : 0u;
    const bool useBasemapOverlay = (activeBasemapProvider != nullptr && basemapOpacityByte > 0);

    const bool isNightPalette = (cfg.chartPalette == ChartPalette::MarineNight);
    const bool isDuskPalette  = (cfg.chartPalette == ChartPalette::MarineDusk);
    const bool darkChartSurface = isNightPalette || isDuskPalette || (cfg.chartPalette == ChartPalette::ClassicGis && !lightMode);

    const guikit::Color selAccent = darkChartSurface
        ? guikit::Color(250, 204, 21, 255)  // Crisp Gold
        : guikit::Color(217, 119, 6, 255);  // Deep Amber
    const guikit::Color hovAccent = darkChartSurface
        ? guikit::Color(56, 189, 248, 255)
        : guikit::Color(2, 132, 199, 255);
    const guikit::Color labelTextCol = darkChartSurface
        ? guikit::Color(241, 245, 249, 245)
        : guikit::Color(15, 23, 42, 245);
    const guikit::Color handleFill = darkChartSurface
        ? guikit::Color(15, 23, 42, 245)
        : guikit::Color(255, 255, 255, 245);
    const guikit::Color chartBaseBg = isNightPalette
        ? guikit::Color(6, 15, 28, 255)
        : (isDuskPalette
            ? guikit::Color(17, 38, 60, 255)
            : (cfg.chartPalette == ChartPalette::ClassicGis
                ? (lightMode ? guikit::Color(241, 245, 249, 255) : guikit::Color(15, 23, 42, 255))
                : guikit::Color(226, 238, 248, 255)));

    // Compute normalized Web Mercator visible box for fast O(log N) R-Tree queries
    const guikit::Vec2 visNwNorm = guikit::MapGrid::GeoToNormalized(visBounds.nw, guikit::MapCrs::EPSG3857_WebMercator);
    const guikit::Vec2 visSeNorm = guikit::MapGrid::GeoToNormalized(visBounds.se, guikit::MapCrs::EPSG3857_WebMercator);
    const keel::f32 visMinX = keel::min(visNwNorm.x, visSeNorm.x);
    const keel::f32 visMinY = keel::min(visNwNorm.y, visSeNorm.y);
    const keel::f32 visMaxX = keel::max(visNwNorm.x, visSeNorm.x);
    const keel::f32 visMaxY = keel::max(visNwNorm.y, visSeNorm.y);
    const guikit::Rect visNormBox(visMinX, visMinY, visMaxX - visMinX, visMaxY - visMinY);

    // Padded query box (80 px screen margin) so labels near screen edges slide smoothly without popping
    const keel::f32 worldPixelsTotal = keel::max(256.0f, 2048.0f * zoom);
    const keel::f32 labelPadNorm = 80.0f / worldPixelsTotal;
    const guikit::Rect paddedVisNormBox(
        visMinX - labelPadNorm,
        visMinY - labelPadNorm,
        (visMaxX - visMinX) + labelPadNorm * 2.0f,
        (visMaxY - visMinY) + labelPadNorm * 2.0f
    );

    // Compute union of `visBounds` and all visible slippy tiles (128 / 256 / 512 px, including guard band)
    // so polygon/line layers always have 100% spatial coverage for every visible tile cell.
    guikit::GeoBounds tileUnionGeoBounds = visBounds;
    if (cfg.useVectorTileCache && !viewState.visibleTiles.empty()) {
        keel::f64 uMinLon = keel::min(visBounds.nw.lon, visBounds.se.lon);
        keel::f64 uMaxLon = keel::max(visBounds.nw.lon, visBounds.se.lon);
        keel::f64 uMinLat = keel::min(visBounds.nw.lat, visBounds.se.lat);
        keel::f64 uMaxLat = keel::max(visBounds.nw.lat, visBounds.se.lat);
        const keel::f64 kGuardFrac = keel::as<keel::f64>(kTileGuardBandPx) / keel::as<keel::f64>(configuredTileSizePx);
        for (const guikit::OsmTileCell& cell : viewState.visibleTiles) {
            const keel::f64 cMinLon = keel::min(cell.geoBounds.nw.lon, cell.geoBounds.se.lon);
            const keel::f64 cMaxLon = keel::max(cell.geoBounds.nw.lon, cell.geoBounds.se.lon);
            const keel::f64 cMinLat = keel::min(cell.geoBounds.nw.lat, cell.geoBounds.se.lat);
            const keel::f64 cMaxLat = keel::max(cell.geoBounds.nw.lat, cell.geoBounds.se.lat);
            const keel::f64 padLon = (cMaxLon - cMinLon) * kGuardFrac;
            const keel::f64 padLat = (cMaxLat - cMinLat) * kGuardFrac;
            uMinLon = keel::min(uMinLon, cMinLon - padLon);
            uMaxLon = keel::max(uMaxLon, cMaxLon + padLon);
            uMinLat = keel::min(uMinLat, cMinLat - padLat);
            uMaxLat = keel::max(uMaxLat, cMaxLat + padLat);
        }
        tileUnionGeoBounds.nw = {keel::clamp(uMaxLat, -85.05112878, 85.05112878), keel::clamp(uMinLon, -180.0, 180.0)};
        tileUnionGeoBounds.se = {keel::clamp(uMinLat, -85.05112878, 85.05112878), keel::clamp(uMaxLon, -180.0, 180.0)};
    }

    const keel::u64 frameRenderStartUs = keel::monotonic_us();

    // Stream active spatial window chunks for massive Out-of-Core GeoPackage layers.
    const keel::u64 syncStartUs = keel::monotonic_us();
    keel::Array<guikit::Rect> dirtiedBoxes;
    keel::usize totalInvalidatedTilesThisFrame = 0;

    if (useAsyncStreaming) {
        keel::Array<AsyncWindowResult> completedWindows;
        GetAsyncStreamer().DrainCompletedWindows(completedWindows);
        for (AsyncWindowResult& winRes : completedWindows) {
            if (!winRes.snapshot) continue;
            for (const geo::GeoLayer& layer : layers) {
                if (layer.uid == winRes.layerUid) {
                    const keel::Array<guikit::Rect> snapDirtied = winRes.snapshot->dirtiedNormBoxes.clone();
                    const keel::usize newlyPaged = winRes.snapshot->newlyPagedFeatures;
                    if (const_cast<geo::GeoLayer&>(layer).ApplyWindowSnapshot(keel::move(*winRes.snapshot))) {
                        stats.pagedInFeaturesThisFrame += newlyPaged;
                        totalInvalidatedTilesThisFrame += GetTileCache().InvalidateTilesForDirtiedBoxes(
                            layer.uid, snapDirtied, s_frameCounter
                        );
                        GetCursorHitCache().Clear();
                    }
                    break;
                }
            }
        }
    }

    for (const geo::GeoLayer& layer : layers) {
        if (!layer.visible || !layer.isWindowStreamed || !layer.pager) continue;
        const bool hasPolyOrLine = (layer.polygonFeatureCount > 0 || layer.lineFeatureCount > 0 || layer.pointFeatureCount == 0);
        const guikit::GeoBounds& targetWindowBounds = hasPolyOrLine ? tileUnionGeoBounds : visBounds;

        if (!useAsyncStreaming) {
            dirtiedBoxes.clear();
            const keel::usize prevCachedFeats = layer.pager->GetCachedFeatureCount();
            if (layer.pager->SyncSpatialWindow(
                    const_cast<geo::GeoLayer&>(layer),
                    targetWindowBounds,
                    paddedVisNormBox,
                    keel::as<keel::f32>(tileZoom),
                    cfg.scaleFiltering,
                    s_frameCounter,
                    &dirtiedBoxes)) {
                const keel::usize curCachedFeats = layer.pager->GetCachedFeatureCount();
                if (curCachedFeats > prevCachedFeats) {
                    stats.pagedInFeaturesThisFrame += (curCachedFeats - prevCachedFeats);
                }
                if (!dirtiedBoxes.empty()) {
                    totalInvalidatedTilesThisFrame += GetTileCache().InvalidateTilesForDirtiedBoxes(
                        layer.uid, dirtiedBoxes, s_frameCounter
                    );
                }
                GetCursorHitCache().Clear();
            }
        } else if (layer.pager->NeedsSpatialWindowUpdate(
                       layer, targetWindowBounds, keel::as<keel::f32>(tileZoom), cfg.scaleFiltering)) {
            GetAsyncStreamer().RequestSpatialWindow(
                layer,
                targetWindowBounds,
                paddedVisNormBox,
                keel::as<keel::f32>(tileZoom),
                cfg.scaleFiltering,
                s_frameCounter
            );
        }
    }

    if (useAsyncStreaming) {
        stats.cachedTilesPending += GetAsyncStreamer().GetPendingWindowCount();
    }
    const keel::f64 syncMs = keel::as<keel::f64>(keel::monotonic_us() - syncStartUs) * 0.001;

    keel::usize activeWindowFeatures = 0;
    for (const geo::GeoLayer& layer : layers) {
        if (layer.visible) {
            ++stats.visibleLayers;
            stats.totalFeatures += layer.isWindowStreamed
                ? keel::max(layer.estimatedTotalFeatures, layer.features.size())
                : layer.features.size();
            activeWindowFeatures += layer.features.size();
        }
    }

    if (stats.visibleLayers == 0 && !useBasemapOverlay) {
        return stats;
    }
    if (!useBasemapOverlay && stats.totalFeatures == 0 && activeWindowFeatures == 0 && stats.cachedTilesPending == 0) {
        return stats;
    }

    // ========================================================================
    // 1. O(log N) R-Tree Interactive Hit-Testing under Mouse Cursor
    //    (Suppressed while actively dragging/panning the map)
    // ========================================================================
    EmitRenderBreadcrumb("GisRenderEngine::Stage1_CursorHitTest", keel::as<keel::i32>(s_frameCounter), tileZoom);
    HitTestMatch hoveredMatch{};
    if (cfg.enableFeaturePicking && !isPanning && scene.IsHovered() && activeWindowFeatures > 0) {
        const guikit::Vec2 mouseWorld = scene.GetMouseWorldPos();
        const guikit::GeoCoord cursorGeo = map.WorldToGeo(mouseWorld);
        const guikit::Vec2 cursorNorm = guikit::MapGrid::GeoToNormalized(cursorGeo, guikit::MapCrs::EPSG3857_WebMercator);
        const keel::f32 pickRadiusNorm = 9.0f / worldPixelsTotal;

        hoveredMatch = FindFeatureUnderCursor(
            cursorNorm, pickRadiusNorm, tileZoom, layers, cfg, s_frameCounter, &stats.pagedInFeaturesThisFrame
        );

        if (hoveredMatch.feat) {
            stats.hoveredFeatureUid = hoveredMatch.feat->uid;
            const keel::f32 hitR = 12.0f * invZ;
            const guikit::Rect cursorHitRect(mouseWorld.x - hitR, mouseWorld.y - hitR, hitR * 2.0f, hitR * 2.0f);
            char catBuf[160];
            const auto catLabelRef = keel::format_buf(catBuf, "%s [%s]",
                hoveredMatch.layer->name.as_ref(),
                hoveredMatch.feat->featureCode.empty()
                    ? geo::GeometryTypeToString(hoveredMatch.feat->type)
                    : hoveredMatch.feat->featureCode.as_ref());
            auto item = scene.Item({
                .id = hoveredMatch.feat->id.as_ref(),
                .title = hoveredMatch.feat->name.as_ref(),
                .category = catLabelRef,
                .hitRect = cursorHitRect,
                .selectable = true,
                .showSelectionFrame = false
            });
            if (item.Clicked()) {
                stats.clickedFeatureUid = hoveredMatch.feat->uid;
                ioSelectedFeatureUid = hoveredMatch.feat->uid;
            }
        }
    }

    // Locate currently selected feature in O(1) via packed (layerUid << 32 | idx)
    const geo::GeoLayer* selectedLayer = nullptr;
    const geo::GeoFeature* selectedFeat = cfg.enableFeaturePicking
        ? geo::FindFeatureByUid(layers, ioSelectedFeatureUid, &selectedLayer)
        : nullptr;
    if (selectedLayer && !selectedLayer->visible) {
        selectedLayer = nullptr;
        selectedFeat = nullptr;
    }

    // ========================================================================
    // 2. Static Polygons & Lines: Two-Tier Atomic [z/x/y] Vector Tile Compositor
    // ========================================================================
    EmitRenderBreadcrumb("GisRenderEngine::Stage2_VectorTileCompositor", keel::as<keel::i32>(s_frameCounter), keel::as<keel::i32>(viewState.visibleTiles.size()));
    if (!viewState.visibleTiles.empty()) {
        keel::f32 minTileWorldX = viewState.visibleTiles.front().worldRect.x;
        keel::f32 minTileWorldY = viewState.visibleTiles.front().worldRect.y;
        keel::f32 maxTileWorldX = viewState.visibleTiles.front().worldRect.Right();
        keel::f32 maxTileWorldY = viewState.visibleTiles.front().worldRect.Bottom();
        for (const guikit::OsmTileCell& cell : viewState.visibleTiles) {
            minTileWorldX = keel::min(minTileWorldX, cell.worldRect.x);
            minTileWorldY = keel::min(minTileWorldY, cell.worldRect.y);
            maxTileWorldX = keel::max(maxTileWorldX, cell.worldRect.Right());
            maxTileWorldY = keel::max(maxTileWorldY, cell.worldRect.Bottom());
        }
        scene.DrawRect(
            guikit::Rect(minTileWorldX, minTileWorldY, maxTileWorldX - minTileWorldX, maxTileWorldY - minTileWorldY),
            chartBaseBg
        );
    }

    keel::f64 rasterMs = 0.0;
    const keel::usize numVisTiles = viewState.visibleTiles.size();
    const bool useCpuVectorTileCache = cfg.useVectorTileCache && !viewState.visibleTiles.empty() &&
                                       guikit::HasDefaultVectorCanvasFactory();
    if (useCpuVectorTileCache) {
        const keel::u64 rasterPassStartUs = keel::monotonic_us();
        PerLayerTileCacheManager& cache = GetTileCache();
        cache.SyncConfig(cfg, viewState.crs, basemapSourceId);
        cache.GarbageCollectLayers(layers);

        const keel::u32 tileSizePx = cache.GetTileSizePx();
        const keel::f32 tileSizeF = cache.GetTileSizeF();

        struct OrderedLayerEntry {
            keel::usize layerIdx{0};
            const geo::GeoLayer* layer{nullptr};
            PerLayerTileCacheManager::LayerTileStore* store{nullptr};
            keel::u64 styleHash{0};
            keel::u8 opacityByte{255};
            keel::i32 cartoRank{90};
            bool isActiveStyle{false};
        };

        keel::Array<OrderedLayerEntry> tileLayers;
        tileLayers.reserve(layers.size());
        for (keel::usize li = 0; li < layers.size(); ++li) {
            const geo::GeoLayer& lyr = layers[li];
            if (!lyr.visible || (lyr.polygonFeatureCount == 0 && lyr.lineFeatureCount == 0)) {
                continue;
            }
            keel::f32 effectiveOpacity = 1.0f;
            if (lyr.polygonFeatureCount > 0 && lyr.style.renderFill) {
                effectiveOpacity = keel::clamp(lyr.style.fillOpacity, 0.0f, 1.0f);
                if (useBasemapOverlay && lyr.HasChartClass(geo::ChartFeatureClass::DepthArea)) {
                    effectiveOpacity *= keel::clamp(cfg.bathymetryOverBasemapOpacity, 0.05f, 1.0f);
                }
            }
            const keel::u8 layerOpacityByte = keel::saturate<keel::u8>(keel::round_i32(effectiveOpacity * 255.0f));
            if (layerOpacityByte == 0) continue;

            (void)cache.SyncLayerStore(lyr, viewState.crs);
            tileLayers.push_back(OrderedLayerEntry{
                .layerIdx = li,
                .layer = &lyr,
                .store = nullptr,
                .styleHash = ComputeLayerStyleHash(lyr, cfg, viewState.crs),
                .opacityByte = layerOpacityByte,
                .cartoRank = CartographicLayerRank(lyr),
                .isActiveStyle = (keel::as<keel::i32>(li) == cfg.activeStyleLayerIdx)
            });
        }
        keel::Array<PerLayerTileCacheManager::LayerTileStore*> visibleStores;
        visibleStores.reserve(tileLayers.size());
        for (OrderedLayerEntry& entry : tileLayers) {
            entry.store = &cache.SyncLayerStore(*entry.layer, viewState.crs);
            visibleStores.push_back(entry.store);
        }
        // Pin all currently visible Tier-1, Tier-2 & basemap tiles to `s_frameCounter` BEFORE draining async results
        // so `StoreAsyncLayerTile` / `StoreBasemapTile` can NEVER evict visible tiles while sibling layers finish.
        cache.PinVisibleTiles(viewState.visibleTiles, visibleStores, s_frameCounter);

        if (useBasemapOverlay) {
            keel::Array<AsyncBasemapResult> completedBasemapTiles;
            GetAsyncStreamer().DrainCompletedBasemapTiles(completedBasemapTiles);
            for (AsyncBasemapResult& bm : completedBasemapTiles) {
                if (cache.StoreBasemapTile(bm.sourceId.as_ref(), bm.tileKey, keel::move(bm.image), s_frameCounter)) {
                    ++stats.cachedTilesRendered;
                }
            }
        }

        // Drain completed background layer tiles AFTER `SyncLayerStore` & `PinVisibleTiles`
        if (useAsyncStreaming) {
            keel::Array<AsyncLayerTileResult> completedLayerTiles;
            GetAsyncStreamer().DrainCompletedLayerTiles(completedLayerTiles);
            for (AsyncLayerTileResult& ltRes : completedLayerTiles) {
                for (const AsyncLoadedFeaturePayload& lp : ltRes.newlyLoadedPayloads) {
                    for (const geo::GeoLayer& lyr : layers) {
                        if (lyr.uid == lp.layerUid && lp.featureIndex < lyr.features.size()) {
                            const geo::GeoFeature& feat = lyr.features[lp.featureIndex];
                            if (feat.uid == lp.featureUid && !feat.geometryLoaded && lp.payload) {
                                feat.sharedGeometry = lp.payload;
                                feat.ApplyDecodedGeometryPayload(*lp.payload);
                                feat.TouchAccess(s_frameCounter);
                                ++stats.pagedInFeaturesThisFrame;
                            }
                            break;
                        }
                    }
                }
                if (cache.StoreAsyncLayerTile(keel::move(ltRes), s_frameCounter)) {
                    ++stats.cachedTilesRendered;
                }
            }
        }

        // Sort back-to-front by cartographic rank for deterministic compositing
        keel::sort(tileLayers,
            [](const OrderedLayerEntry& a, const OrderedLayerEntry& b) noexcept {
                if (a.cartoRank != b.cartoRank) return a.cartoRank < b.cartoRank;
                return a.layerIdx < b.layerIdx;
            });

        const keel::u32 bgPx = PackColorABGR8888(chartBaseBg);
        keel::u64 compositeSignature = keel::kFnv1aOffsetBasis;
        HashCombineVal(compositeSignature, viewState.crs);
        HashCombineVal(compositeSignature, tileSizePx);
        HashCombineVal(compositeSignature, bgPx);
        HashCombineVal(compositeSignature, useBasemapOverlay);
        if (useBasemapOverlay) {
            HashCombineBytes(compositeSignature, basemapSourceId.data(), basemapSourceId.size());
            HashCombineVal(compositeSignature, basemapOpacityByte);
            HashCombineVal(compositeSignature, cache.GetBasemapGeneration());
        }

        keel::Array<PerLayerTileCacheManager::CompositeLayerRef> compositeRefs;
        compositeRefs.reserve(tileLayers.size());
        for (const OrderedLayerEntry& entry : tileLayers) {
            HashCombineVal(compositeSignature, entry.layer->uid);
            HashCombineVal(compositeSignature, entry.styleHash);
            HashCombineVal(compositeSignature, entry.opacityByte);
            compositeRefs.push_back({
                .store = entry.store,
                .opacityByte = entry.opacityByte
            });
        }

        const guikit::Vec2 viewCenterW = map.GeoToWorld(viewState.center);

        struct PendingCellTask {
            keel::usize cellIdx{0};
            keel::u64 lastUpdatedFrame{0};
            keel::f32 distSq{0.0f};
            bool hasPreview{false};
        };
        keel::Array<PendingCellTask> pendingCells;
        pendingCells.reserve(numVisTiles);

        for (keel::usize ci = 0; ci < numVisTiles; ++ci) {
            const guikit::OsmTileCell& cell = viewState.visibleTiles[ci];
            const TileCoordKey key{cell.id.z, cell.id.x, cell.id.y};

            const keel::f32 cx = cell.worldRect.x + cell.worldRect.w * 0.5f;
            const keel::f32 cy = cell.worldRect.y + cell.worldRect.h * 0.5f;
            const keel::f32 dx = cx - viewCenterW.x;
            const keel::f32 dy = cy - viewCenterW.y;
            const keel::f32 cellDistSq = dx * dx + dy * dy;

            if (useBasemapOverlay && !cache.HasBasemapTile(key)) {
                const geo::SlippyTileKey bmKey{cell.id.z, cell.id.x, cell.id.y, tileSizePx};
                auto cachedBm = activeBasemapProvider->TryGetCachedTile(bmKey);
                if (cachedBm && cachedBm->IsValid()) {
                    if (cache.StoreBasemapTile(basemapSourceId, bmKey, keel::move(cachedBm), s_frameCounter)) {
                        ++stats.cachedTilesRendered;
                    }
                } else {
                    GetAsyncStreamer().EnqueueBasemapTileJob(activeBasemapProvider, bmKey, cellDistSq);
                }
            }

            // Fast-path 1: Tier-2 Composite Tile is already complete and matches current signature
            CompositeTile* comp = cache.FindCompositeTile(key, s_frameCounter);
            if (comp && comp->isComplete && comp->compositeSignature == compositeSignature && !comp->pixels.empty()) {
                ++stats.cachedTilesHit;
                continue;
            }

            // Compute guard-band normalized EPSG:3857 query box for this cell's geographic footprint
            const CellNorm3857Bounds cb = ComputeCellNorm3857Bounds(cell, viewState.crs);
            const keel::f32 marginU = cb.spanU * (kTileGuardBandPx / tileSizeF);
            const keel::f32 marginV = cb.spanV * (kTileGuardBandPx / tileSizeF);
            const guikit::Rect cellQueryBox(
                cb.u0 - marginU,
                cb.v0 - marginV,
                (cb.u1 - cb.u0) + 2.0f * marginU,
                (cb.v1 - cb.v0) + 2.0f * marginV
            );

            bool anyLayerNeedsRaster = false;
            for (const OrderedLayerEntry& entry : tileLayers) {
                const CachedVectorTile* lt = PerLayerTileCacheManager::FindTile(*entry.store, key, s_frameCounter);
                if (lt && lt->styleHash == entry.styleHash) {
                    continue;
                }
                if (!entry.layer->normBounds.Intersects(cellQueryBox)) {
                    cache.MarkTileEmpty(*entry.store, key, cellQueryBox, entry.styleHash, s_frameCounter);
                    continue;
                }
                if (entry.layer->features.empty()) {
                    if (useAsyncStreaming && entry.layer->isWindowStreamed &&
                        GetAsyncStreamer().IsLayerWindowPending(entry.layer->uid)) {
                        anyLayerNeedsRaster = true;
                        continue;
                    }
                    cache.MarkTileEmpty(*entry.store, key, cellQueryBox, entry.styleHash, s_frameCounter);
                    continue;
                }
                anyLayerNeedsRaster = true;
            }

            // Fast-path 2: All Tier-1 layer tiles are already cached (e.g. live fillOpacity change, basemap arrival, or layer toggle)
            if (!anyLayerNeedsRaster) {
                if (!comp || comp->pixels.empty() || comp->compositeSignature != compositeSignature) {
                    (void)cache.BuildCompositeTile(
                        key, cellQueryBox, compositeSignature, bgPx, compositeRefs,
                        s_frameCounter, useBasemapOverlay, basemapOpacityByte
                    );
                }
                ++stats.cachedTilesHit;
                continue;
            }

            // Seed fallback preview from parent [z-1]/[z-2] or child [z+1] composite tiles if this cell has no pixels yet
            if (!comp || comp->pixels.empty()) {
                (void)cache.SeedFallbackFromHierarchy(key, bgPx, s_frameCounter);
                comp = cache.FindCompositeTile(key, s_frameCounter);
            }

            pendingCells.push_back(PendingCellTask{
                .cellIdx = ci,
                .lastUpdatedFrame = comp ? comp->lastUpdatedFrame : 0ULL,
                .distSq = cellDistSq,
                .hasPreview = (comp != nullptr && !comp->pixels.empty())
            });
        }

        if (!pendingCells.empty()) {
            const bool isStyleDrag = (cfg.activeStyleLayerIdx >= 0);
            keel::sort(pendingCells,
                [isStyleDrag](const PendingCellTask& a, const PendingCellTask& b) noexcept {
                    if (isStyleDrag) {
                        if (a.lastUpdatedFrame != b.lastUpdatedFrame) {
                            return a.lastUpdatedFrame < b.lastUpdatedFrame;
                        }
                        if (a.distSq != b.distSq) return a.distSq < b.distSq;
                        return a.cellIdx < b.cellIdx;
                    }
                    if (a.hasPreview != b.hasPreview) {
                        return !a.hasPreview; // Tiles without fallback preview get highest priority
                    }
                    if (a.distSq != b.distSq) return a.distSq < b.distSq; // Center-outward radial completion
                    return a.cellIdx < b.cellIdx;
                });

            const keel::f32 baseBudgetMs = keel::clamp(cfg.tileRenderBudgetMs, 1.0f, 30.0f);
            const keel::f32 effectiveBudgetMs = isPanning ? keel::min(baseBudgetMs, 4.0f) : baseBudgetMs;
            const keel::u64 deadlineUs = frameRenderStartUs + keel::as<keel::u64>(effectiveBudgetMs * 1000.0f);
            const keel::u64 fastPathDeadlineUs = frameRenderStartUs + keel::as<keel::u64>(isPanning ? 1400 : 2500);
            keel::usize completedCells = 0;

            for (keel::usize pi = 0; pi < pendingCells.size(); ++pi) {
                const keel::usize ci = pendingCells[pi].cellIdx;
                if (!useAsyncStreaming &&
                    (completedCells >= 1 || syncMs >= 3.0) &&
                    keel::monotonic_us() >= deadlineUs) {
                    stats.cachedTilesPending += (pendingCells.size() - pi);
                    break;
                }

                const guikit::OsmTileCell& cell = viewState.visibleTiles[ci];
                const TileCoordKey key{cell.id.z, cell.id.x, cell.id.y};

                const CellNorm3857Bounds cb = ComputeCellNorm3857Bounds(cell, viewState.crs);
                const keel::f32 marginU = cb.spanU * (kTileGuardBandPx / tileSizeF);
                const keel::f32 marginV = cb.spanV * (kTileGuardBandPx / tileSizeF);
                const guikit::Rect cellQueryBox(
                    cb.u0 - marginU,
                    cb.v0 - marginV,
                    (cb.u1 - cb.u0) + 2.0f * marginU,
                    (cb.v1 - cb.v0) + 2.0f * marginV
                );

                keel::String layerSummary;
                bool allLayersReady = true;
                for (const OrderedLayerEntry& entry : tileLayers) {
                    const CachedVectorTile* lt = PerLayerTileCacheManager::FindTile(*entry.store, key, s_frameCounter);
                    if (lt && lt->styleHash == entry.styleHash) {
                        if constexpr (kEnableFileDebugLog) {
                            if (lt->hasAnyGeometry) {
                                if (!layerSummary.empty()) layerSummary.append(",");
                                layerSummary.append(entry.layer->tableName.empty() ? entry.layer->name.as_ref() : entry.layer->tableName.as_ref());
                            }
                        }
                        continue;
                    }

                    if (!useAsyncStreaming &&
                        (completedCells >= 1 || stats.pagedInFeaturesThisFrame >= 48) &&
                        keel::monotonic_us() >= deadlineUs) {
                        allLayersReady = false;
                        break;
                    }

                    bool abortedOrQueued = false;
                    const keel::usize minPagedBeforeCheck = (completedCells == 0) ? 48u : 16u;
                    const bool canRunSyncFastPath =
                        !useAsyncStreaming || (keel::monotonic_us() < fastPathDeadlineUs);
                    const CachedVectorTile* rendered = cache.RasterizeOrDispatchLayerTile(
                        *entry.store,
                        key,
                        entry.styleHash,
                        cell,
                        viewState.crs,
                        *entry.layer,
                        cfg,
                        s_frameCounter,
                        pendingCells[pi].distSq,
                        useAsyncStreaming,
                        canRunSyncFastPath,
                        &stats.pagedInFeaturesThisFrame,
                        /*hasDeadline=*/!useAsyncStreaming,
                        deadlineUs,
                        minPagedBeforeCheck,
                        &abortedOrQueued
                    );
                    if (!rendered || abortedOrQueued) {
                        allLayersReady = false;
                        if (!useAsyncStreaming) {
                            break;
                        }
                        continue;
                    }
                    if constexpr (kEnableFileDebugLog) {
                        if (rendered->hasAnyGeometry) {
                            if (!layerSummary.empty()) layerSummary.append(",");
                            layerSummary.append(entry.layer->tableName.empty() ? entry.layer->name.as_ref() : entry.layer->tableName.as_ref());
                        }
                    }
                }

                if (!allLayersReady) {
                    if (!pendingCells[pi].hasPreview && useBasemapOverlay) {
                        CompositeTile& partial = cache.BuildCompositeTile(
                            key, cellQueryBox, compositeSignature, bgPx, compositeRefs,
                            s_frameCounter, useBasemapOverlay, basemapOpacityByte
                        );
                        partial.isComplete = false;
                    }
                    if (!useAsyncStreaming) {
                        stats.cachedTilesPending += (pendingCells.size() - pi);
                        break;
                    }
                    ++stats.cachedTilesPending;
                    continue;
                }

                (void)cache.BuildCompositeTile(
                    key, cellQueryBox, compositeSignature, bgPx, compositeRefs,
                    s_frameCounter, useBasemapOverlay, basemapOpacityByte
                );
                ++completedCells;
                ++stats.cachedTilesRendered;

                if constexpr (kEnableFileDebugLog) {
                    const auto logLine = keel::format(
                        "[RasterTile] frame=%llu [%d/%d/%d] geo=[%.3f..%.3fE, %.3f..%.3fN] drawnLayers=[%s]",
                        s_frameCounter,
                        cell.id.z, cell.id.x, cell.id.y,
                        keel::min(cell.geoBounds.nw.lon, cell.geoBounds.se.lon),
                        keel::max(cell.geoBounds.nw.lon, cell.geoBounds.se.lon),
                        keel::min(cell.geoBounds.nw.lat, cell.geoBounds.se.lat),
                        keel::max(cell.geoBounds.nw.lat, cell.geoBounds.se.lat),
                        layerSummary.empty() ? keel::StringRef("EMPTY") : layerSummary.as_ref()
                    );
                    AppendRenderDebugLog(logLine);
                }
            }
        }

        cache.EnforceCompositeBudget(s_frameCounter);
        cache.EnforceGlobalLayerPixelBudget(s_frameCounter, compositeSignature);

        if (useAsyncStreaming || useBasemapOverlay) {
            stats.cachedTilesPending += GetAsyncStreamer().GetPendingLayerTileCount() +
                                        GetAsyncStreamer().GetPendingBasemapCount();
        }

        // Single-pass blit: 1 opaque CompositeTile DrawPixels call per visible [z/x/y] cell,
        // plus cartographic loading placeholder overlay for cells still streaming/rasterizing in the background.
        for (const guikit::OsmTileCell& cell : viewState.visibleTiles) {
            const TileCoordKey key{cell.id.z, cell.id.x, cell.id.y};
            const CompositeTile* comp = cache.FindCompositeTile(key, s_frameCounter);
            const bool hasPixels = (comp != nullptr && !comp->pixels.empty());
            const bool isComplete = (hasPixels && comp->isComplete);
            if (hasPixels) {
                scene.DrawPixels(
                    {comp->pixels, tileSizePx, tileSizePx},
                    cell.worldRect,
                    0.0f,
                    255
                );
            }
            if (!isComplete && cfg.showLoadingTilePlaceholders) {
                DrawLoadingTilePlaceholder(
                    scene,
                    cell,
                    zoom,
                    invZ,
                    darkChartSurface,
                    /*hasPreview=*/hasPixels,
                    s_frameCounter
                );
            }
        }
        rasterMs = keel::as<keel::f64>(keel::monotonic_us() - rasterPassStartUs) * 0.001;
    } else {
        // Direct immediate-mode vector pass (GPU-native path when no offscreen CPU vector canvas
        // is registered, or when VectorTileCache is explicitly toggled off).
        if (useBasemapOverlay && !viewState.visibleTiles.empty()) {
            PerLayerTileCacheManager& cache = GetTileCache();
            cache.SyncConfig(cfg, viewState.crs, basemapSourceId);
            cache.PinVisibleBasemapTiles(viewState.visibleTiles, s_frameCounter);
            const keel::u32 tileSizePx = cache.GetTileSizePx();

            keel::Array<AsyncBasemapResult> completedBasemapTiles;
            GetAsyncStreamer().DrainCompletedBasemapTiles(completedBasemapTiles);
            for (AsyncBasemapResult& bm : completedBasemapTiles) {
                if (cache.StoreBasemapTile(bm.sourceId.as_ref(), bm.tileKey, keel::move(bm.image), s_frameCounter)) {
                    ++stats.cachedTilesRendered;
                }
            }

            const guikit::Vec2 viewCenterW = map.GeoToWorld(viewState.center);
            for (const guikit::OsmTileCell& cell : viewState.visibleTiles) {
                const TileCoordKey key{cell.id.z, cell.id.x, cell.id.y};
                if (!cache.HasBasemapTile(key)) {
                    const geo::SlippyTileKey bmKey{cell.id.z, cell.id.x, cell.id.y, tileSizePx};
                    auto cachedBm = activeBasemapProvider->TryGetCachedTile(bmKey);
                    if (cachedBm && cachedBm->IsValid()) {
                        if (cache.StoreBasemapTile(basemapSourceId, bmKey, keel::move(cachedBm), s_frameCounter)) {
                            ++stats.cachedTilesRendered;
                        }
                    } else {
                        const keel::f32 dx = (cell.worldRect.x + cell.worldRect.w * 0.5f) - viewCenterW.x;
                        const keel::f32 dy = (cell.worldRect.y + cell.worldRect.h * 0.5f) - viewCenterW.y;
                        GetAsyncStreamer().EnqueueBasemapTileJob(activeBasemapProvider, bmKey, dx * dx + dy * dy);
                    }
                }
                if (const geo::RasterTileImage* bm = cache.FindBasemapTile(key, s_frameCounter)) {
                    scene.DrawPixels(
                        {bm->rgbaPixels, bm->width, bm->height},
                        cell.worldRect,
                        0.0f,
                        basemapOpacityByte
                    );
                    ++stats.cachedTilesHit;
                }
            }
            stats.cachedTilesPending += GetAsyncStreamer().GetPendingBasemapCount();
        }

        struct OrderedDirectLayer {
            const geo::GeoLayer* layer{nullptr};
            keel::usize layerIdx{0};
            keel::i32 rank{90};
        };
        keel::Array<OrderedDirectLayer> orderedLayers;
        orderedLayers.reserve(layers.size());
        for (keel::usize li = 0; li < layers.size(); ++li) {
            const geo::GeoLayer& layer = layers[li];
            if (!layer.visible || layer.features.empty()) continue;
            if (cfg.frustumCulling && !layer.normBounds.Intersects(visNormBox)) continue;
            orderedLayers.push_back({&layer, li, CartographicLayerRank(layer)});
        }
        keel::sort(orderedLayers,
            [](const OrderedDirectLayer& a, const OrderedDirectLayer& b) noexcept {
                if (a.rank != b.rank) return a.rank < b.rank;
                return a.layerIdx < b.layerIdx;
            });

        keel::Array<keel::u32> visIndices;
        for (keel::i32 pass = 0; pass < 2; ++pass) {
            for (const OrderedDirectLayer& entry : orderedLayers) {
                const geo::GeoLayer& layer = *entry.layer;
                if (pass == 0 && layer.polygonFeatureCount == 0) continue;
                if (pass == 1 && layer.lineFeatureCount == 0 && layer.polygonFeatureCount == 0) continue;

                visIndices.clear();
                layer.spatialIndex.query_box(
                    keel::RTreeBox<keel::f32>::from_xywh(visNormBox.x, visNormBox.y, visNormBox.w, visNormBox.h),
                    [&](keel::u32 fi) { visIndices.push_back(fi); }
                );
                if (pass == 0 && layer.HasChartClass(geo::ChartFeatureClass::DepthArea) && visIndices.size() > 1) {
                    keel::sort(visIndices,
                        [&layer](keel::u32 a, keel::u32 b) noexcept {
                            const geo::GeoFeature& fa = layer.features[a];
                            const geo::GeoFeature& fb = layer.features[b];
                            const keel::f32 da = !keel::is_nan(fa.minDepthMeters) ? fa.minDepthMeters
                                           : (!keel::is_nan(fa.depthMeters) ? fa.depthMeters : 10000.0f);
                            const keel::f32 db = !keel::is_nan(fb.minDepthMeters) ? fb.minDepthMeters
                                           : (!keel::is_nan(fb.depthMeters) ? fb.depthMeters : 10000.0f);
                            if (da != db) return da > db;
                            return a < b;
                        });
                }

                for (keel::u32 fIdx : visIndices) {
                    if (fIdx >= layer.features.size()) continue;
                    const geo::GeoFeature& feat = layer.features[fIdx];
                    if (ShouldSkipFeatureAtZoom(feat, tileZoom, cfg)) continue;

                    const ResolvedFeatureStyle st = ResolveMarineFeatureStyle(layer, feat, cfg, /*bakeFillOpacity=*/true);
                    if (pass == 0 && feat.HasPolygons()) {
                        const bool wasLoaded = feat.geometryLoaded;
                        if (!feat.EnsureGeometryLoaded(s_frameCounter)) continue;
                        if (!wasLoaded) ++stats.pagedInFeaturesThisFrame;

                        auto& mutablePolys = const_cast<keel::Array<geo::GeoPolygon>&>(feat.polygons);
                        for (geo::GeoPolygon& poly : mutablePolys) {
                            if (st.renderFill && poly.triangles.empty() && poly.exteriorRing.size() >= 3) {
                                geo::PolygonTriangulator::TriangulatePolygon(poly);
                                feat.RefreshResidentHeapBytes();
                            }
                            if (st.renderFill && !poly.triangles.empty()) {
                                guikit::Color fillCol = st.fillColor;
                                if (useBasemapOverlay && feat.chartClass == geo::ChartFeatureClass::DepthArea) {
                                    const keel::f32 mult = keel::clamp(cfg.bathymetryOverBasemapOpacity, 0.05f, 1.0f);
                                    fillCol.a = keel::saturate<keel::u8>(keel::round_i32(keel::as<keel::f32>(fillCol.a) * mult));
                                }
                                for (const geo::GeoTriangle& tri : poly.triangles) {
                                    scene.DrawTriangle(
                                        map.GeoToWorld(tri.a),
                                        map.GeoToWorld(tri.b),
                                        map.GeoToWorld(tri.c),
                                        fillCol
                                    );
                                    ++stats.renderedTriangles;
                                }
                            }
                            if (st.renderStroke) {
                                DrawWorldRing(map, scene, poly.exteriorRing, st.strokeColor, st.strokeWidthPx * invZ,
                                    minSegWorldSq, true, false, 0.0f, handleFill, stats.renderedVertices);
                                for (const auto& hole : poly.interiorRings) {
                                    DrawWorldRing(map, scene, hole, st.strokeColor, st.strokeWidthPx * invZ,
                                        minSegWorldSq, true, false, 0.0f, handleFill, stats.renderedVertices);
                                }
                            }
                        }
                    } else if (pass == 1 && feat.HasLines() && st.renderStroke) {
                        const bool wasLoaded = feat.geometryLoaded;
                        if (!feat.EnsureGeometryLoaded(s_frameCounter)) continue;
                        if (!wasLoaded) ++stats.pagedInFeaturesThisFrame;

                        for (const auto& line : feat.lines) {
                            DrawWorldRing(map, scene, line, st.strokeColor, st.strokeWidthPx * invZ,
                                minSegWorldSq, false, false, 0.0f, handleFill, stats.renderedVertices);
                        }
                    }
                }
            }
        }
    }

    // ========================================================================
    // 3. Pan-Invariant World-Space Decluttered Overlay Pass
    //    (Pre-Indexed Compact Lists: O(accepted) instead of O(totalFeatures))
    // ========================================================================
    EmitRenderBreadcrumb("GisRenderEngine::Stage3_WorldDeclutterOverlay", keel::as<keel::i32>(s_frameCounter), tileZoom);
    const keel::u64 declutterStartUs = keel::monotonic_us();
    WorldDeclutterCache& declutterCache = GetDeclutterCache();
    declutterCache.Sync(layers, cfg, tileZoom, viewState.crs, paddedVisNormBox);
    stats.declutteredLabels = declutterCache.GetTotalDecluttered();
    const keel::f64 declutterMs = keel::as<keel::f64>(keel::monotonic_us() - declutterStartUs) * 0.001;

    if constexpr (kEnableFileDebugLog) {
        if (totalInvalidatedTilesThisFrame > 0 || stats.cachedTilesRendered > 0 || syncMs >= 1.0 || declutterMs >= 1.0) {
            const keel::f64 totalFrameMs = keel::as<keel::f64>(keel::monotonic_us() - frameRenderStartUs) * 0.001;
            const auto logLine = keel::format(
                "[FrameTiles] frame=%llu z=%d vis=[%.3f..%.3fE, %.3f..%.3fN] visTiles=%zu hit=%zu new=%zu pend=%zu invalidated=%zu activeFeats=%zu pagedIn=%zu ms=%.2f (sync=%.2f raster=%.2f declutter=%.2f)",
                s_frameCounter,
                tileZoom,
                keel::min(visBounds.nw.lon, visBounds.se.lon),
                keel::max(visBounds.nw.lon, visBounds.se.lon),
                keel::min(visBounds.nw.lat, visBounds.se.lat),
                keel::max(visBounds.nw.lat, visBounds.se.lat),
                numVisTiles,
                stats.cachedTilesHit,
                stats.cachedTilesRendered,
                stats.cachedTilesPending,
                totalInvalidatedTilesThisFrame,
                activeWindowFeatures,
                stats.pagedInFeaturesThisFrame,
                totalFrameMs,
                syncMs,
                rasterMs,
                declutterMs
            );
            AppendRenderDebugLog(logLine);
        }
    }

    // Pass 3A: Render Accepted Point Symbols (Buoys/Beacons, Underwater Hazards, Generic Points)
    for (const auto& item : declutterCache.GetAcceptedSymbols()) {
        if (item.layerIdx >= layers.size()) continue;
        const geo::GeoLayer& layer = layers[item.layerIdx];
        if (!layer.visible || item.featIdx >= layer.features.size()) continue;
        const geo::GeoFeature& feat = layer.features[item.featIdx];
        if (feat.points.empty() || feat.normPoints.empty()) continue;
        if (cfg.frustumCulling && !paddedVisNormBox.Contains(feat.normPoints[0])) {
            ++stats.culledFeatures;
            continue;
        }
        ++stats.renderedFeatures;

        const keel::f32 ptScale = keel::clamp(layer.style.pointRadiusPx / 6.5f, 0.35f, 3.5f);
        const bool customCol = layer.style.useCustomColor;
        const bool isSel = (feat.uid == ioSelectedFeatureUid);
        const bool asSeamark = layer.style.renderAsSeamark;

        if (feat.chartClass == geo::ChartFeatureClass::BuoyOrBeacon) {
            const guikit::Color buoyCol = isSel
                ? selAccent
                : (customCol ? layer.style.primaryColor : ResolveIalaBuoyColor(feat.lightOrColor.as_ref(), guikit::Color(232, 121, 249, 255)));
            const bool isLitMark = asSeamark && (feat.hasLight || feat.featureCode == "LIGHTS");
            for (const guikit::GeoCoord& pt : feat.points) {
                if (isLitMark && tileZoom >= 12) {
                    map.DrawGeoCircle(pt, 8.5f * ptScale, WithAlpha(buoyCol, 0.20f), WithAlpha(buoyCol, 0.70f), 1.1f);
                }
                map.DrawGeoCircle(pt, (isSel ? 6.0f : 4.6f) * ptScale, buoyCol, handleFill, 1.5f);
                if (asSeamark) {
                    map.DrawGeoCircle(pt, 1.6f * ptScale, handleFill);
                }
                ++stats.renderedVertices;
            }
        } else if (feat.chartClass == geo::ChartFeatureClass::UnderwaterHazard) {
            if (tileZoom < 11 && !isSel) continue;
            const guikit::Color hazFill = customCol
                ? WithAlpha(layer.style.primaryColor, 0.78f)
                : (isNightPalette
                    ? guikit::Color(14, 116, 144, 190)
                    : guikit::Color(125, 211, 252, 210));
            const guikit::Color hazCross = isSel
                ? selAccent
                : (darkChartSurface ? guikit::Color(248, 113, 113, 245) : guikit::Color(15, 23, 42, 245));
            for (const guikit::GeoCoord& pt : feat.points) {
                map.DrawGeoCircle(pt, 5.2f * ptScale, hazFill, asSeamark ? hazCross : handleFill, 1.1f);
                if (asSeamark) {
                    const guikit::Vec2 wPt = map.GeoToWorld(pt);
                    const keel::f32 armW = 3.2f * ptScale * invZ;
                    scene.DrawLine({wPt.x - armW, wPt.y}, {wPt.x + armW, wPt.y}, hazCross, 1.3f * invZ);
                    scene.DrawLine({wPt.x, wPt.y - armW}, {wPt.x, wPt.y + armW}, hazCross, 1.3f * invZ);
                }
                ++stats.renderedVertices;
            }
        } else if (feat.chartClass == geo::ChartFeatureClass::BuiltUpOrStructure &&
                   !feat.HasPolygons() && !feat.HasLines()) {
            const guikit::Color structCol = isSel
                ? selAccent
                : (customCol
                    ? layer.style.primaryColor
                    : (isNightPalette ? guikit::Color(148, 128, 96, 235) : guikit::Color(92, 73, 46, 235)));
            for (const guikit::GeoCoord& pt : feat.points) {
                map.DrawGeoCircle(pt, (isSel ? 4.8f : 3.2f) * ptScale, structCol, handleFill, 1.2f);
                ++stats.renderedVertices;
            }
        } else if (feat.chartClass == geo::ChartFeatureClass::LandArea &&
                   !feat.HasPolygons() && !feat.HasLines()) {
            const guikit::Color landPtCol = isSel
                ? selAccent
                : (customCol
                    ? layer.style.primaryColor
                    : (isNightPalette ? guikit::Color(168, 142, 102, 225) : guikit::Color(120, 88, 42, 230)));
            for (const guikit::GeoCoord& pt : feat.points) {
                map.DrawGeoCircle(pt, (isSel ? 4.4f : 2.6f) * ptScale, landPtCol, handleFill, 1.1f);
                ++stats.renderedVertices;
            }
        } else if (feat.chartClass == geo::ChartFeatureClass::Generic && !feat.HasPolygons() && !feat.HasLines()) {
            const guikit::Color ptCol = isSel ? selAccent : layer.style.primaryColor;
            for (const guikit::GeoCoord& pt : feat.points) {
                if (asSeamark) {
                    auto smItem = map.DrawSeamark(pt, {
                        .id = feat.id.as_ref(),
                        .title = layer.style.showLabels ? feat.name.as_ref() : keel::StringRef{},
                        .category = layer.name.as_ref(),
                        .radiusPx = layer.style.pointRadiusPx,
                        .showLightSector = true,
                        .color = ptCol,
                        .selectable = true
                    });
                    if (smItem.Clicked()) {
                        stats.clickedFeatureUid = feat.uid;
                        ioSelectedFeatureUid = feat.uid;
                    }
                } else {
                    map.DrawGeoCircle(pt, layer.style.pointRadiusPx, WithAlpha(ptCol, 0.85f), handleFill, 1.5f);
                }
                ++stats.renderedVertices;
            }
        }
    }

    // Pass 3B: Render Accepted Pan-Invariant Text Labels (`labelRu` / `labelEn` / Named Features)
    if (cfg.showFeatureCallouts) {
        keel::String labelScratch;
        for (const auto& item : declutterCache.GetAcceptedLabels()) {
            if (item.layerIdx >= layers.size()) continue;
            const geo::GeoLayer& layer = layers[item.layerIdx];
            if (!layer.visible || !layer.style.showLabels || item.featIdx >= layer.features.size()) continue;
            const geo::GeoFeature& feat = layer.features[item.featIdx];
            if (cfg.frustumCulling && !paddedVisNormBox.Contains(feat.normCentroid)) {
                continue;
            }

            keel::usize charCount = 0;
            const keel::StringRef labelText = FormatMapCalloutLabel(feat, cfg, labelScratch, &charCount);
            if (labelText.empty()) continue;

            const bool isSel = (feat.uid == ioSelectedFeatureUid);
            const keel::f32 estWidthPx = keel::clamp(keel::as<keel::f32>(charCount) * 6.0f + 10.0f, 16.0f, 640.0f);
            const keel::f32 offsetX = !feat.points.empty() ? 8.0f : (-estWidthPx * 0.4f);
            const keel::f32 offsetY = -7.0f;

            const guikit::Color txtCol = isSel ? selAccent : labelTextCol;
            map.DrawGeoText(
                labelText,
                feat.centroid,
                {offsetX, offsetY},
                isSel ? 12.0f : 10.5f,
                txtCol,
                isSel ? guikit::FontStyle::Bold : guikit::FontStyle::Regular
            );
            ++stats.renderedFeatures;
        }
    }

    // Pass 3C: Render Accepted Depth Soundings (`SOUNDG`) with Navionics Subscript Tenths
    if (cfg.showSoundings && tileZoom >= 11) {
        const guikit::Color shallowSndCol = darkChartSurface
            ? guikit::Color(125, 211, 252, 235)
            : guikit::Color(12, 74, 110, 235);
        const guikit::Color deepSndCol = darkChartSurface
            ? guikit::Color(148, 163, 184, 210)
            : guikit::Color(71, 85, 105, 215);

        for (const auto& item : declutterCache.GetAcceptedSoundings()) {
            if (item.layerIdx >= layers.size()) continue;
            const geo::GeoLayer& layer = layers[item.layerIdx];
            if (!layer.visible || item.featIdx >= layer.features.size()) continue;
            const geo::GeoFeature& feat = layer.features[item.featIdx];
            if (feat.points.empty() || feat.normPoints.empty()) continue;
            if (cfg.frustumCulling && !paddedVisNormBox.Contains(feat.normPoints[0])) {
                ++stats.culledFeatures;
                continue;
            }

            const guikit::GeoCoord pt = feat.points[0];
            const keel::f32 d = keel::max(0.0f, feat.depthMeters);
            const keel::i32 wholeM = keel::floor_i32(d + 0.04f);
            const keel::i32 tenths = keel::round_i32((d - keel::as<keel::f32>(wholeM)) * 10.0f);
            const guikit::Color sndCol = (d <= cfg.safetyContourM) ? shallowSndCol : deepSndCol;

            char wholeBuf[16]{};
            const keel::StringRef wholeRef = keel::format_buf(wholeBuf, "%d", wholeM);

            if (d < 12.0f && tenths > 0 && tenths < 10) {
                map.DrawGeoText(wholeRef, pt, {-5.0f, -5.0f}, 9.5f, sndCol, guikit::FontStyle::Bold);
                char tenthBuf[8]{};
                const keel::StringRef tenthRef = keel::format_buf(tenthBuf, "%d", tenths);
                const keel::f32 subOffX = (wholeM >= 10) ? 6.5f : 1.5f;
                map.DrawGeoText(tenthRef, pt, {subOffX, -2.0f}, 7.8f, sndCol, guikit::FontStyle::Regular);
            } else {
                map.DrawGeoText(wholeRef, pt, {-4.0f, -5.0f}, 9.5f, sndCol, guikit::FontStyle::Regular);
            }

            ++stats.renderedFeatures;
            ++stats.renderedVertices;
        }
    }

    // ========================================================================
    // 4. Interactive Hovered & Selected Feature Vector Highlight Overlay
    // ========================================================================
    if (!isPanning && hoveredMatch.feat && hoveredMatch.feat != selectedFeat) {
        RenderHighlightedFeatureOverlay(
            map, scene, *hoveredMatch.feat,
            hovAccent, handleFill, invZ, minSegWorldSq,
            /*drawHandles=*/false, s_frameCounter,
            stats.renderedVertices, &stats.pagedInFeaturesThisFrame
        );
    }

    if (selectedFeat && selectedLayer) {
        RenderHighlightedFeatureOverlay(
            map, scene, *selectedFeat,
            selAccent, handleFill, invZ, minSegWorldSq,
            /*drawHandles=*/cfg.showVertexDotsOnSelect, s_frameCounter,
            stats.renderedVertices, &stats.pagedInFeaturesThisFrame
        );
    }

    // ========================================================================
    // 5. Out-of-Core LRU Feature RAM Budget Eviction (Throttled when Idle)
    // ========================================================================
    static keel::usize s_cumulativeEvicted = 0;
    static keel::usize s_lastResidentFeatures = 0;
    static keel::usize s_lastResidentBytes = 0;
    static keel::usize s_lastMaxResidentBytes = 0;
    static keel::u64 s_lastEvictCheckFrame = 0;
    static bool s_pendingBudgetCheck = false;
    if (stats.pagedInFeaturesThisFrame > 0) {
        s_pendingBudgetCheck = true;
    }

    const keel::f32 clampedBudgetMb = keel::clamp(cfg.featureCacheBudgetMb, 1.0f, 1024.0f);
    const keel::usize maxResidentBytes = keel::as<keel::usize>(clampedBudgetMb * 1024.0f * 1024.0f);
    const bool budgetChanged = (s_lastMaxResidentBytes != maxResidentBytes);
    const bool cooldownElapsed = (s_frameCounter - s_lastEvictCheckFrame >= 45u);
    if (!isPanning && stats.cachedTilesPending == 0) {
        for (const geo::GeoLayer& layer : layers) {
            if (layer.pager) continue;
            for (const geo::GeoFeature& feat : layer.features) {
                if (feat.geometryLoaded && feat.sharedGeometry) {
                    feat.sharedGeometry.reset();
                }
            }
        }
    }
    if (!isPanning && stats.cachedTilesPending == 0 &&
        (budgetChanged || s_lastResidentFeatures == 0 || (s_pendingBudgetCheck && cooldownElapsed))) {
        s_lastMaxResidentBytes = maxResidentBytes;
        s_lastEvictCheckFrame = s_frameCounter;
        s_pendingBudgetCheck = false;
        s_cumulativeEvicted += EnforceFeatureRamBudget(
            layers,
            maxResidentBytes,
            s_frameCounter,
            &s_lastResidentFeatures,
            &s_lastResidentBytes
        );
    }
    stats.residentFeatureCount = s_lastResidentFeatures;
    stats.residentFeatureBytes = s_lastResidentBytes;
    stats.evictedFeaturesTotal = s_cumulativeEvicted;

    return stats;
}


} // namespace guikit::geo
