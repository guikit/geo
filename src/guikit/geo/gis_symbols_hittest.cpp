#include "gis_render_internal.hpp"
#include "keel/algo.h"
#include "keel/format.h"
#include "keel/math.h"
#include "keel/memory.h"

namespace guikit::geo {

namespace detail {

[[nodiscard]] keel::StringRef ResolveFeatureLabelText(
    const geo::GeoFeature& feat,
    const GisRenderConfig& cfg
) noexcept {
    if (!cfg.preferredLanguage.empty()) {
        const keel::StringRef lang = cfg.preferredLanguage;
        if (lang == "ru" && !feat.labelRu.empty()) {
            return feat.labelRu.as_ref();
        }
        if (lang == "en" && !feat.labelEn.empty()) {
            return feat.labelEn.as_ref();
        }
        for (const auto& [k, v] : feat.properties) {
            if (v.is_string()) {
                const keel::StringRef sv = v.as_string();
                if (!sv.empty()) {
                    const keel::StringRef kRef = k.as_ref();
                    if ((kRef.size() == 5 + lang.size() && kRef.starts_with("name:") && kRef.substr(5) == lang) ||
                        (kRef.size() == 6 + lang.size() && kRef.starts_with("label_") && kRef.substr(6) == lang)) {
                        return sv;
                    }
                }
            }
        }
    }
    if (cfg.preferRussianLabels && !feat.labelRu.empty()) {
        return feat.labelRu.as_ref();
    }
    if (!feat.labelEn.empty()) {
        return feat.labelEn.as_ref();
    }
    if (!feat.labelRu.empty()) {
        return feat.labelRu.as_ref();
    }
    if ((feat.chartClass == geo::ChartFeatureClass::Generic || feat.featureCode == "LNDELV") &&
        !feat.name.empty()) {
        return feat.name.as_ref();
    }
    return {};
}

[[maybe_unused]] [[nodiscard]] keel::usize CountUtf8CodePoints(keel::StringRef s) noexcept {
    keel::usize count = 0;
    for (keel::usize i = 0; i < s.size();) {
        const keel::u8 c = keel::as<keel::u8>(s[i]);
        keel::usize seqLen = 1;
        if ((c & 0x80u) == 0u) seqLen = 1;
        else if ((c & 0xE0u) == 0xC0u) seqLen = 2;
        else if ((c & 0xF0u) == 0xE0u) seqLen = 3;
        else if ((c & 0xF8u) == 0xF0u) seqLen = 4;
        if (i + seqLen > s.size()) break;
        ++count;
        i += seqLen;
    }
    return count;
}

[[nodiscard]] keel::StringRef FormatMapCalloutLabel(
    const geo::GeoFeature& feat,
    const GisRenderConfig& cfg,
    keel::String& scratchBuf,
    keel::usize* outCharCount
) {
    if (outCharCount) *outCharCount = 0;

    // 1. Optional filter: suppress callout labels on polygonal area features (keeping point & line labels)
    if (!cfg.showAreaFeatureLabels && feat.HasPolygons() && feat.points.empty()) {
        return {};
    }

    keel::StringRef raw = ResolveFeatureLabelText(feat, cfg);
    if (raw.empty()) return {};

    // 2. Optional S-52 filter: suppress multi-sentence INFORM / TXTDSC cautionary notes (containing '|')
    //    or regulatory caution/restricted area notes from cluttering the map surface
    if (cfg.hideMultiLineNotesOnMap) {
        if (raw.find('|') != keel::StringRef::npos ||
            raw.find('\n') != keel::StringRef::npos ||
            IsCautionOrRestrictedArea(feat)) {
            return {};
        }
    }

    // 3. Configurable character limit (UTF-8 codepoint-aware truncation so Cyrillic never splits mid-character)
    keel::usize charCount = 0;
    keel::usize cutByteOffset = raw.size();
    for (keel::usize i = 0; i < raw.size();) {
        const keel::u8 c = keel::as<keel::u8>(raw[i]);
        keel::usize seqLen = 1;
        if ((c & 0x80u) == 0u) seqLen = 1;
        else if ((c & 0xE0u) == 0xC0u) seqLen = 2;
        else if ((c & 0xF0u) == 0xE0u) seqLen = 3;
        else if ((c & 0xF8u) == 0xF0u) seqLen = 4;
        if (i + seqLen > raw.size()) break;
        ++charCount;
        i += seqLen;
        if (cfg.maxMapLabelChars > 0 && charCount == cfg.maxMapLabelChars && i < raw.size()) {
            cutByteOffset = i;
            break;
        }
    }

    if (cfg.maxMapLabelChars > 0 && cutByteOffset < raw.size()) {
        if (!cfg.truncateLongLabels) {
            return {};
        }
        scratchBuf.assign(raw.substr(0, cutByteOffset));
        for (char& ch : scratchBuf) {
            if (ch == '|' || ch == '\n' || ch == '\r') ch = ' ';
        }
        scratchBuf.append("...");
        if (outCharCount) *outCharCount = charCount + 3;
        return scratchBuf.as_ref();
    }

    if (outCharCount) *outCharCount = charCount;

    if (raw.find('|') != keel::StringRef::npos || raw.find('\n') != keel::StringRef::npos) {
        scratchBuf.assign(raw);
        for (char& ch : scratchBuf) {
            if (ch == '|' || ch == '\n' || ch == '\r') ch = ' ';
        }
        return scratchBuf.as_ref();
    }

    return raw;
}


void WorldDeclutterCache::Clear() noexcept {
    m_cachedKey = 0;
    m_safeNormBox = guikit::Rect(0.0f, 0.0f, 0.0f, 0.0f);
    m_acceptedSymbols.clear();
    m_acceptedLabels.clear();
    m_acceptedSoundings.clear();
    ClearOccupiedCells();
}

void WorldDeclutterCache::ReleaseMemory() noexcept {
    m_cachedKey = 0;
    m_safeNormBox = guikit::Rect(0.0f, 0.0f, 0.0f, 0.0f);
    m_totalDecluttered = 0;
    m_acceptedSymbols = {};
    m_acceptedLabels = {};
    m_acceptedSoundings = {};
    m_cellKeys = {};
    m_cellEpochs = {};
    m_curEpoch = 1;
    m_occupiedCount = 0;
    m_queryScratch = {};
    m_soundingCands = {};
}

void WorldDeclutterCache::Sync(
    keel::ArrayRef<geo::GeoLayer> layers,
    const GisRenderConfig& cfg,
    keel::i32 tileZoom,
    guikit::MapCrs crs,
    const guikit::Rect& paddedVisNormBox
) {
    keel::u64 key = keel::kFnv1aOffsetBasis;
    HashCombineVal(key, tileZoom);
    HashCombineVal(key, crs);
    HashCombineVal(key, cfg.preferRussianLabels);
    for (char ch : cfg.preferredLanguage) {
        HashCombineVal(key, keel::as<keel::u8>(ch));
    }
    HashCombineVal(key, cfg.showSoundings);
    HashCombineVal(key, cfg.showFeatureCallouts);
    HashCombineVal(key, cfg.scaleFiltering);
    HashCombineVal(key, cfg.showCautionAndRestrictedZones);
    HashCombineVal(key, cfg.showAreaFeatureLabels);
    HashCombineVal(key, cfg.hideMultiLineNotesOnMap);
    HashCombineVal(key, cfg.maxMapLabelChars);
    HashCombineVal(key, cfg.truncateLongLabels);

    for (const geo::GeoLayer& lyr : layers) {
        HashCombineVal(key, lyr.uid);
        HashCombineVal(key, lyr.visible);
        HashCombineVal(key, lyr.style.showLabels);
        HashCombineVal(key, lyr.style.pointRadiusPx);
        HashCombineVal(key, lyr.style.renderAsSeamark);
        const keel::usize fCount = lyr.features.size();
        HashCombineVal(key, fCount);
        HashCombineVal(key, lyr.windowGeneration);
    }

    const bool insideSafeBox = (m_safeNormBox.w > 0.0f) &&
        (paddedVisNormBox.x >= m_safeNormBox.x &&
         paddedVisNormBox.y >= m_safeNormBox.y &&
         paddedVisNormBox.Right() <= m_safeNormBox.Right() &&
         paddedVisNormBox.Bottom() <= m_safeNormBox.Bottom());

    if (key == m_cachedKey && insideSafeBox) {
        return;
    }

    m_cachedKey = key;
    const keel::f32 spanX = keel::max(1e-5f, paddedVisNormBox.w);
    const keel::f32 spanY = keel::max(1e-5f, paddedVisNormBox.h);
    m_safeNormBox = guikit::Rect(
        paddedVisNormBox.x - spanX * 0.30f,
        paddedVisNormBox.y - spanY * 0.30f,
        spanX * 1.60f,
        spanY * 1.60f
    );
    const guikit::Rect queryNormBox(
        paddedVisNormBox.x - spanX * 0.70f,
        paddedVisNormBox.y - spanY * 0.70f,
        spanX * 2.40f,
        spanY * 2.40f
    );

    m_totalDecluttered = 0;
    m_acceptedSymbols.clear();
    m_acceptedLabels.clear();
    m_acceptedSoundings.clear();
    ClearOccupiedCells();

    const keel::f32 worldPxScale = keel::as<keel::f32>(256ULL << keel::clamp(tileZoom, 0, 22));
    auto toWorldPx = [crs, worldPxScale](guikit::Vec2 norm3857, guikit::GeoCoord geo) noexcept -> guikit::Vec2 {
        if (crs == guikit::MapCrs::EPSG3857_WebMercator || crs == guikit::MapCrs::EPSG3395_WorldMercator) {
            return {norm3857.x * worldPxScale, norm3857.y * worldPxScale};
        }
        if (crs == guikit::MapCrs::EPSG4326_PlateCarree) {
            const keel::f32 ny = keel::as<keel::f32>((90.0 - keel::clamp(geo.lat, -90.0, 90.0)) * (1.0 / 180.0));
            return {norm3857.x * worldPxScale, ny * worldPxScale};
        }
        const guikit::Vec2 n = guikit::MapGrid::GeoToNormalized(geo, crs);
        return {n.x * worldPxScale, n.y * worldPxScale};
    };

    // Pass 1: Buoys, Beacons & Generic Point Features (highest priority — only scan layers with points!)
    for (keel::usize li = 0; li < layers.size(); ++li) {
        const geo::GeoLayer& layer = layers[li];
        if (!layer.visible || layer.pointFeatureCount == 0 || layer.features.empty()) continue;
        if (layer.chartClassMask != 0u &&
            !layer.HasChartClass(geo::ChartFeatureClass::BuoyOrBeacon) &&
            !layer.HasChartClass(geo::ChartFeatureClass::Generic)) {
            continue;
        }
        if (!layer.normBounds.Intersects(queryNormBox)) continue;
        const keel::f32 ptScale = keel::clamp(layer.style.pointRadiusPx / 6.5f, 0.35f, 3.5f);
        const keel::f32 boxHalf = 7.0f * ptScale;

        auto processPass1Feat = [&](keel::u32 fi) {
            const geo::GeoFeature& feat = layer.features[fi];
            if (feat.normPoints.empty() || feat.points.empty()) return;

            if (feat.chartClass == geo::ChartFeatureClass::BuoyOrBeacon) {
                const guikit::Vec2 wpt = toWorldPx(feat.normPoints[0], feat.points[0]);
                if (TryReserveWorldBox(wpt.x - boxHalf, wpt.y - boxHalf, boxHalf * 2.0f, boxHalf * 2.0f)) {
                    m_acceptedSymbols.push_back({keel::as<keel::u32>(li), fi});
                } else {
                    ++m_totalDecluttered;
                }
            } else if (feat.chartClass == geo::ChartFeatureClass::Generic &&
                       !feat.HasPolygons() && !feat.HasLines()) {
                m_acceptedSymbols.push_back({keel::as<keel::u32>(li), fi});
            }
        };

        if (layer.features.size() > 2048) {
            m_queryScratch.clear();
            layer.spatialIndex.query_box(
                keel::RTreeBox<keel::f32>::from_xywh(queryNormBox.x, queryNormBox.y, queryNormBox.w, queryNormBox.h),
                [&](keel::u32 fi) { m_queryScratch.push_back(fi); }
            );
            keel::sort(m_queryScratch);
            for (keel::u32 fi : m_queryScratch) {
                if (fi < layer.features.size()) processPass1Feat(fi);
            }
        } else {
            for (keel::usize fi = 0; fi < layer.features.size(); ++fi) {
                processPass1Feat(keel::as<keel::u32>(fi));
            }
        }
    }

    // Pass 2: Underwater Hazards, Point Structures (PILPNT / SLCONS), & Land Elevations (LNDELV)
    if (tileZoom >= 11) {
        for (keel::usize li = 0; li < layers.size(); ++li) {
            const geo::GeoLayer& layer = layers[li];
            if (!layer.visible || layer.pointFeatureCount == 0 || layer.features.empty()) continue;
            if (layer.chartClassMask != 0u &&
                !layer.HasChartClass(geo::ChartFeatureClass::UnderwaterHazard) &&
                !layer.HasChartClass(geo::ChartFeatureClass::BuiltUpOrStructure) &&
                !layer.HasChartClass(geo::ChartFeatureClass::LandArea)) {
                continue;
            }
            if (!layer.normBounds.Intersects(queryNormBox)) continue;
            const keel::f32 ptScale = keel::clamp(layer.style.pointRadiusPx / 6.5f, 0.35f, 3.5f);

            auto processPass2Feat = [&](keel::u32 fi) {
                const geo::GeoFeature& feat = layer.features[fi];
                if (feat.normPoints.empty() || feat.points.empty() ||
                    feat.HasPolygons() || feat.HasLines()) {
                    return;
                }
                const bool isHazard = (feat.chartClass == geo::ChartFeatureClass::UnderwaterHazard);
                const bool isPointStructOrLand = (tileZoom >= 12) &&
                    (feat.chartClass == geo::ChartFeatureClass::BuiltUpOrStructure ||
                     feat.chartClass == geo::ChartFeatureClass::LandArea);
                if (!isHazard && !isPointStructOrLand) {
                    return;
                }

                const guikit::Vec2 wpt = toWorldPx(feat.normPoints[0], feat.points[0]);
                const keel::f32 boxHalf = (isHazard ? 6.0f : 4.5f) * ptScale;
                if (TryReserveWorldBox(wpt.x - boxHalf, wpt.y - boxHalf, boxHalf * 2.0f, boxHalf * 2.0f)) {
                    m_acceptedSymbols.push_back({keel::as<keel::u32>(li), fi});
                } else {
                    ++m_totalDecluttered;
                }
            };

            if (layer.features.size() > 2048) {
                m_queryScratch.clear();
                layer.spatialIndex.query_box(
                    keel::RTreeBox<keel::f32>::from_xywh(queryNormBox.x, queryNormBox.y, queryNormBox.w, queryNormBox.h),
                    [&](keel::u32 fi) { m_queryScratch.push_back(fi); }
                );
                keel::sort(m_queryScratch);
                for (keel::u32 fi : m_queryScratch) {
                    if (fi < layer.features.size()) processPass2Feat(fi);
                }
            } else {
                for (keel::usize fi = 0; fi < layer.features.size(); ++fi) {
                    processPass2Feat(keel::as<keel::u32>(fi));
                }
            }
        }
    }

    // Pass 3: Cartographic Text Labels (`labelRu` / `labelEn` / Named Features)
    if (cfg.showFeatureCallouts) {
        keel::String labelScratch;
        constexpr keel::u32 kNoCalloutMask =
            (1u << keel::as<keel::u8>(geo::ChartFeatureClass::Sounding)) |
            (1u << keel::as<keel::u8>(geo::ChartFeatureClass::DepthArea)) |
            (1u << keel::as<keel::u8>(geo::ChartFeatureClass::CoverageMask));

        for (keel::usize li = 0; li < layers.size(); ++li) {
            const geo::GeoLayer& layer = layers[li];
            if (!layer.visible || !layer.style.showLabels || layer.features.empty()) continue;
            if (layer.IsExclusivelyChartClasses(kNoCalloutMask)) {
                continue;
            }
            if (!layer.normBounds.Intersects(queryNormBox)) continue;

            auto processPass3Feat = [&](keel::u32 fi) {
                const geo::GeoFeature& feat = layer.features[fi];
                if (feat.chartClass == geo::ChartFeatureClass::Sounding ||
                    feat.chartClass == geo::ChartFeatureClass::DepthArea ||
                    feat.chartClass == geo::ChartFeatureClass::CoverageMask) {
                    return;
                }
                if (tileZoom < 11 && feat.chartClass != geo::ChartFeatureClass::TextLabel) return;
                if (ShouldSkipFeatureAtZoom(feat, tileZoom, cfg)) return;

                keel::usize charCount = 0;
                const keel::StringRef labelText = FormatMapCalloutLabel(feat, cfg, labelScratch, &charCount);
                if (labelText.empty()) return;

                const guikit::Vec2 wpt = toWorldPx(feat.normCentroid, feat.centroid);
                const keel::f32 estWidthPx = keel::clamp(keel::as<keel::f32>(charCount) * 6.0f + 10.0f, 16.0f, 640.0f);
                const keel::f32 estHeightPx = 16.0f;
                const keel::f32 offsetX = !feat.points.empty() ? 8.0f : (-estWidthPx * 0.4f);
                const keel::f32 offsetY = -7.0f;

                if (TryReserveWorldBox(wpt.x + offsetX, wpt.y + offsetY - 4.0f, estWidthPx, estHeightPx)) {
                    m_acceptedLabels.push_back({keel::as<keel::u32>(li), fi});
                } else {
                    ++m_totalDecluttered;
                }
            };

            if (layer.features.size() > 2048) {
                m_queryScratch.clear();
                layer.spatialIndex.query_box(
                    keel::RTreeBox<keel::f32>::from_xywh(queryNormBox.x, queryNormBox.y, queryNormBox.w, queryNormBox.h),
                    [&](keel::u32 fi) { m_queryScratch.push_back(fi); }
                );
                keel::sort(m_queryScratch);
                for (keel::u32 fi : m_queryScratch) {
                    if (fi < layer.features.size()) processPass3Feat(fi);
                }
            } else {
                for (keel::usize fi = 0; fi < layer.features.size(); ++fi) {
                    processPass3Feat(keel::as<keel::u32>(fi));
                }
            }
        }
    }

    // Pass 4: Depth Soundings (`SOUNDG`) — sorted shallowest-first (IHO S-52 hydrographic safety rule)
    if (cfg.showSoundings && tileZoom >= 11) {
        const keel::f32 boxW = (tileZoom >= 14) ? 32.0f : ((tileZoom == 13) ? 42.0f : 54.0f);
        const keel::f32 boxH = (tileZoom >= 14) ? 18.0f : 24.0f;

        m_soundingCands.clear();

        for (keel::usize li = 0; li < layers.size(); ++li) {
            const geo::GeoLayer& layer = layers[li];
            if (!layer.visible || layer.pointFeatureCount == 0 || layer.features.empty()) continue;
            if (layer.chartClassMask != 0u && !layer.HasChartClass(geo::ChartFeatureClass::Sounding)) {
                continue;
            }
            if (!layer.normBounds.Intersects(queryNormBox)) continue;

            auto processSoundingFeat = [&](keel::u32 fi) {
                const geo::GeoFeature& feat = layer.features[fi];
                if (feat.chartClass != geo::ChartFeatureClass::Sounding ||
                    feat.normPoints.empty() || feat.points.empty()) return;
                if (keel::is_nan(feat.depthMeters)) return;

                const guikit::Vec2 wpt = toWorldPx(feat.normPoints[0], feat.points[0]);
                m_soundingCands.push_back(SoundingCand{
                    .layerIdx = keel::as<keel::u32>(li),
                    .featIdx = fi,
                    .depth = feat.depthMeters,
                    .wx = wpt.x,
                    .wy = wpt.y
                });
            };

            if (layer.features.size() > 2048) {
                m_queryScratch.clear();
                layer.spatialIndex.query_box(
                    keel::RTreeBox<keel::f32>::from_xywh(queryNormBox.x, queryNormBox.y, queryNormBox.w, queryNormBox.h),
                    [&](keel::u32 fi) { m_queryScratch.push_back(fi); }
                );
                for (keel::u32 fi : m_queryScratch) {
                    if (fi < layer.features.size()) processSoundingFeat(fi);
                }
            } else {
                for (keel::usize fi = 0; fi < layer.features.size(); ++fi) {
                    processSoundingFeat(keel::as<keel::u32>(fi));
                }
            }
        }

        keel::sort(m_soundingCands,
            [](const SoundingCand& a, const SoundingCand& b) noexcept {
                if (a.depth != b.depth) return a.depth < b.depth;
                if (a.layerIdx != b.layerIdx) return a.layerIdx < b.layerIdx;
                return a.featIdx < b.featIdx;
            });

        for (const SoundingCand& sc : m_soundingCands) {
            if (TryReserveWorldBox(sc.wx - boxW * 0.5f, sc.wy - boxH * 0.5f, boxW, boxH)) {
                m_acceptedSoundings.push_back({sc.layerIdx, sc.featIdx});
            } else {
                ++m_totalDecluttered;
            }
        }
    }
}

void WorldDeclutterCache::ClearOccupiedCells() noexcept {
    if (m_cellEpochs.size() != kCellTableSize) {
        m_cellKeys.assign(kCellTableSize, 0ULL);
        m_cellEpochs.assign(kCellTableSize, 0U);
        m_curEpoch = 1U;
    } else {
        ++m_curEpoch;
        if (m_curEpoch == 0U) {
            keel::fill(m_cellEpochs.as_mut_ref(), 0U);
            m_curEpoch = 1U;
        }
    }
    m_occupiedCount = 0;
}

[[nodiscard]] keel::u64 WorldDeclutterCache::PackCell(keel::i32 cx, keel::i32 cy) noexcept {
    return (keel::as<keel::u64>(keel::as<keel::u32>(cx)) << 32) |
           keel::as<keel::u64>(keel::as<keel::u32>(cy));
}

[[nodiscard]] keel::usize WorldDeclutterCache::HashCellKey(keel::u64 k) noexcept {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    return keel::as<keel::usize>(k) & kCellTableMask;
}

[[nodiscard]] bool WorldDeclutterCache::ContainsCell(keel::u64 key) const noexcept {
    keel::usize idx = HashCellKey(key);
    while (m_cellEpochs[idx] == m_curEpoch) {
        if (m_cellKeys[idx] == key) return true;
        idx = (idx + 1) & kCellTableMask;
    }
    return false;
}

void WorldDeclutterCache::InsertCell(keel::u64 key) noexcept {
    if (m_occupiedCount >= (kCellTableSize * 3) / 4) return;
    keel::usize idx = HashCellKey(key);
    while (m_cellEpochs[idx] == m_curEpoch) {
        if (m_cellKeys[idx] == key) return;
        idx = (idx + 1) & kCellTableMask;
    }
    m_cellEpochs[idx] = m_curEpoch;
    m_cellKeys[idx] = key;
    ++m_occupiedCount;
}

[[nodiscard]] bool WorldDeclutterCache::TryReserveWorldBox(keel::f32 wx, keel::f32 wy, keel::f32 w, keel::f32 h) noexcept {
    const keel::i32 c0 = keel::floor_i32(wx * 0.125f);
    const keel::i32 c1 = keel::floor_i32((wx + w) * 0.125f);
    const keel::i32 r0 = keel::floor_i32(wy * 0.125f);
    const keel::i32 r1 = keel::floor_i32((wy + h) * 0.125f);

    for (keel::i32 r = r0; r <= r1; ++r) {
        for (keel::i32 c = c0; c <= c1; ++c) {
            if (ContainsCell(PackCell(c, r))) {
                return false;
            }
        }
    }

    for (keel::i32 r = r0; r <= r1; ++r) {
        for (keel::i32 c = c0; c <= c1; ++c) {
            InsertCell(PackCell(c, r));
        }
    }
    return true;
}

WorldDeclutterCache& GetDeclutterCache() {
    static WorldDeclutterCache s_declutterCache;
    return s_declutterCache;
}

namespace {

[[nodiscard]] bool PointInNormRing(guikit::Vec2 pt, keel::ArrayRef<guikit::Vec2> ring) noexcept {
    if (ring.size() < 3) return false;
    bool inside = false;
    for (keel::usize i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const guikit::Vec2& vi = ring[i];
        const guikit::Vec2& vj = ring[j];
        if (((vi.y > pt.y) != (vj.y > pt.y)) &&
            (pt.x < (vj.x - vi.x) * (pt.y - vi.y) / (vj.y - vi.y + 1e-12f) + vi.x)) {
            inside = !inside;
        }
    }
    return inside;
}

[[nodiscard]] keel::f32 PointToSegmentDistSq(guikit::Vec2 p, guikit::Vec2 a, guikit::Vec2 b) noexcept {
    const keel::f32 dx = b.x - a.x;
    const keel::f32 dy = b.y - a.y;
    const keel::f32 lenSq = dx * dx + dy * dy;
    if (lenSq <= 1e-16f) {
        const keel::f32 px = p.x - a.x;
        const keel::f32 py = p.y - a.y;
        return px * px + py * py;
    }
    const keel::f32 t = keel::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / lenSq, 0.0f, 1.0f);
    const keel::f32 projX = a.x + t * dx;
    const keel::f32 projY = a.y + t * dy;
    const keel::f32 ex = p.x - projX;
    const keel::f32 ey = p.y - projY;
    return ex * ex + ey * ey;
}


} // namespace

CursorHitCache& GetCursorHitCache() noexcept {
    static CursorHitCache s_hitCache;
    return s_hitCache;
}

[[nodiscard]] HitTestMatch FindFeatureUnderCursor(
    guikit::Vec2 cursorNorm,
    keel::f32 pickRadiusNorm,
    keel::i32 tileZoom,
    keel::ArrayRef<geo::GeoLayer> layers,
    const GisRenderConfig& cfg,
    keel::u64 frameTick,
    keel::usize* outPagedInCount
) {
    CursorHitCache& hc = GetCursorHitCache();
    const keel::f32 moveDx = cursorNorm.x - hc.cursorNorm.x;
    const keel::f32 moveDy = cursorNorm.y - hc.cursorNorm.y;
    const keel::f32 moveThresholdSq = (pickRadiusNorm * 0.08f) * (pickRadiusNorm * 0.08f);
    if (hc.valid &&
        hc.tileZoom == tileZoom &&
        hc.layerCount == layers.size() &&
        hc.showCoverageBounds == cfg.showCoverageBounds &&
        hc.showCautionAndRestrictedZones == cfg.showCautionAndRestrictedZones &&
        keel::abs(hc.pickRadiusNorm - pickRadiusNorm) <= pickRadiusNorm * 0.05f &&
        (moveDx * moveDx + moveDy * moveDy) <= moveThresholdSq) {
        if (hc.matchedUid == 0) {
            return {};
        }
        const geo::GeoLayer* hitLyr = nullptr;
        const geo::GeoFeature* hitFeat = geo::FindFeatureByUid(layers, hc.matchedUid, &hitLyr);
        if (hitFeat && hitLyr && hitLyr->visible) {
            hitFeat->TouchAccess(frameTick);
            return {hitLyr, hitFeat};
        }
    }

    const guikit::Rect pickBox(
        cursorNorm.x - pickRadiusNorm,
        cursorNorm.y - pickRadiusNorm,
        pickRadiusNorm * 2.0f,
        pickRadiusNorm * 2.0f
    );
    const keel::f32 pickRadiusSq = pickRadiusNorm * pickRadiusNorm;
    const keel::f32 minPickableFeatureDim = pickRadiusNorm * 0.25f;
    const bool useLodHit = (tileZoom <= 11);

    HitTestMatch bestPoint{};
    keel::f32 bestPointDistSq = pickRadiusSq * 1.6f;

    HitTestMatch bestLine{};
    keel::f32 bestLineDistSq = pickRadiusSq;

    HitTestMatch bestPoly{};
    keel::f32 bestPolyArea = 1e30f;

    keel::usize syncHitPagedCount = 0;
    keel::Array<keel::u32> candidates;
    for (const geo::GeoLayer& layer : layers) {
        if (!layer.visible || layer.features.empty()) continue;
        if (!layer.normBounds.Intersects(pickBox)) continue;

        candidates.clear();
        layer.spatialIndex.query_box(
            keel::RTreeBox<keel::f32>::from_xywh(pickBox.x, pickBox.y, pickBox.w, pickBox.h),
            [&](keel::u32 fIdx) { candidates.push_back(fIdx); }
        );
        for (keel::u32 fIdx : candidates) {
            if (fIdx >= layer.features.size()) continue;
            const geo::GeoFeature& feat = layer.features[fIdx];
            if (ShouldSkipFeatureAtZoom(feat, tileZoom, cfg)) continue;

            // 1. Point hit-test (highest priority)
            for (const guikit::Vec2& pt : feat.normPoints) {
                const keel::f32 dx = cursorNorm.x - pt.x;
                const keel::f32 dy = cursorNorm.y - pt.y;
                const keel::f32 dSq = dx * dx + dy * dy;
                if (dSq <= bestPointDistSq) {
                    bestPointDistSq = dSq;
                    bestPoint = {&layer, &feat};
                }
            }
            if (bestPoint.feat) continue;

            // Skip sub-pixel lines/polygons at low zoom
            const keel::f32 maxDimNorm = keel::max(feat.normBounds.w, feat.normBounds.h);
            if (maxDimNorm < minPickableFeatureDim) continue;

            // 2. Line hit-test
            if (feat.HasLines()) {
                const bool wasLoaded = feat.geometryLoaded;
                bool ready = feat.TryEnsureGeometryLoadedNonBlocking(frameTick);
                if (!ready && (!cfg.asyncBackgroundStreaming || syncHitPagedCount < 2)) {
                    ready = feat.EnsureGeometryLoaded(frameTick);
                    if (ready) ++syncHitPagedCount;
                }
                if (!ready) {
                    continue;
                }
                if (!wasLoaded && outPagedInCount) {
                    ++(*outPagedInCount);
                }
                for (keel::usize li = 0; li < feat.normLines.size(); ++li) {
                    const auto& line = (useLodHit && li < feat.normLinesLod.size() && feat.normLinesLod[li].size() >= 2) ? feat.normLinesLod[li] : feat.normLines[li];
                    for (keel::usize i = 1; i < line.size(); ++i) {
                        const keel::f32 dSq = PointToSegmentDistSq(cursorNorm, line[i - 1], line[i]);
                        if (dSq <= bestLineDistSq) {
                            bestLineDistSq = dSq;
                            bestLine = {&layer, &feat};
                        }
                    }
                }
            }

            // 3. Polygon hit-test (smallest containing polygon wins; fast bbox & area reject BEFORE paging SQLite!)
            if (!feat.HasPolygons() || !feat.normBounds.Contains(cursorNorm)) {
                continue;
            }
            if (feat.chartClass == geo::ChartFeatureClass::CoverageMask && !cfg.showCoverageBounds) {
                continue;
            }
            const keel::f32 bboxArea = keel::max(1e-12f, feat.normBounds.w * feat.normBounds.h);
            const keel::f32 priorityWeight = (feat.chartClass == geo::ChartFeatureClass::DepthArea)
                ? 4.0f
                : ((feat.chartClass == geo::ChartFeatureClass::SeabedOrMarineArea) ? 2.5f : 1.0f);
            const keel::f32 weightedArea = bboxArea * priorityWeight;
            if (weightedArea > bestPolyArea) {
                continue;
            }

            const bool wasLoaded = feat.geometryLoaded;
            bool ready = feat.TryEnsureGeometryLoadedNonBlocking(frameTick);
            if (!ready && (!cfg.asyncBackgroundStreaming || syncHitPagedCount < 2)) {
                ready = feat.EnsureGeometryLoaded(frameTick);
                if (ready) ++syncHitPagedCount;
            }
            if (!ready) {
                continue;
            }
            if (!wasLoaded && outPagedInCount) {
                ++(*outPagedInCount);
            }

            for (const geo::GeoPolygon& poly : feat.polygons) {
                const auto& extRing = (useLodHit && poly.normExteriorLod.size() >= 3)
                    ? poly.normExteriorLod
                    : poly.normExterior;
                if (!PointInNormRing(cursorNorm, extRing)) continue;
                bool inHole = false;
                for (const auto& hole : poly.normInterior) {
                    if (PointInNormRing(cursorNorm, hole)) {
                        inHole = true;
                        break;
                    }
                }
                if (inHole) continue;
                bestPolyArea = weightedArea;
                bestPoly = {&layer, &feat};
            }
        }
    }

    HitTestMatch result = bestPoint.feat ? bestPoint : (bestLine.feat ? bestLine : bestPoly);
    hc.cursorNorm = cursorNorm;
    hc.pickRadiusNorm = pickRadiusNorm;
    hc.tileZoom = tileZoom;
    hc.layerCount = layers.size();
    hc.showCoverageBounds = cfg.showCoverageBounds;
    hc.showCautionAndRestrictedZones = cfg.showCautionAndRestrictedZones;
    hc.matchedUid = result.feat ? result.feat->uid : 0ULL;
    hc.valid = true;
    return result;
}


void DrawWorldRing(
    guikit::MapViewScope& map,
    guikit::SceneScope& scene,
    keel::ArrayRef<guikit::GeoCoord> ring,
    guikit::Color strokeColor,
    keel::f32 strokeWidthWorld,
    keel::f32 minSegmentWorldSq,
    bool closed,
    bool drawVertexHandles,
    keel::f32 handleRadiusWorld,
    guikit::Color handleFill,
    keel::usize& ioRenderedVerts
) {
    if (ring.size() < 2) return;

    const guikit::Rect visW = scene.GetVisibleWorldRect();
    const keel::f32 padW = keel::max(strokeWidthWorld * 4.0f, keel::max(visW.w, visW.h) * 0.05f);
    const keel::f32 clipMinX = visW.x - padW;
    const keel::f32 clipMinY = visW.y - padW;
    const keel::f32 clipMaxX = visW.Right() + padW;
    const keel::f32 clipMaxY = visW.Bottom() + padW;

    auto drawClippedWorldSegment = [&](guikit::Vec2 a, guikit::Vec2 b) {
        const keel::f32 dx = b.x - a.x;
        const keel::f32 dy = b.y - a.y;
        keel::f32 t0 = 0.0f;
        keel::f32 t1 = 1.0f;
        auto clipTest = [&](keel::f32 p, keel::f32 q) noexcept -> bool {
            if (keel::abs(p) < 1e-8f) return q >= 0.0f;
            const keel::f32 r = q / p;
            if (p < 0.0f) {
                if (r > t1) return false;
                if (r > t0) t0 = r;
            } else {
                if (r < t0) return false;
                if (r < t1) t1 = r;
            }
            return true;
        };
        if (!clipTest(-dx, a.x - clipMinX) ||
            !clipTest( dx, clipMaxX - a.x) ||
            !clipTest(-dy, a.y - clipMinY) ||
            !clipTest( dy, clipMaxY - a.y)) {
            return;
        }
        const guikit::Vec2 ca(a.x + t0 * dx, a.y + t0 * dy);
        const guikit::Vec2 cb(a.x + t1 * dx, a.y + t1 * dy);
        scene.DrawLine(ca, cb, strokeColor, strokeWidthWorld);
    };

    guikit::Vec2 firstW = map.GeoToWorld(ring.front());
    guikit::Vec2 prevW = firstW;
    ++ioRenderedVerts;

    if (drawVertexHandles &&
        firstW.x >= clipMinX && firstW.x <= clipMaxX &&
        firstW.y >= clipMinY && firstW.y <= clipMaxY) {
        scene.DrawCircle(firstW, handleRadiusWorld, handleFill, strokeColor, strokeWidthWorld * 0.6f);
    }

    for (keel::usize i = 1; i < ring.size(); ++i) {
        const guikit::Vec2 currW = map.GeoToWorld(ring[i]);
        const keel::f32 dx = currW.x - prevW.x;
        const keel::f32 dy = currW.y - prevW.y;
        const keel::f32 distSq = dx * dx + dy * dy;

        const bool isLast = (i + 1 == ring.size());
        if (!isLast && distSq < minSegmentWorldSq) {
            continue;
        }

        drawClippedWorldSegment(prevW, currW);
        prevW = currW;
        ++ioRenderedVerts;

        if (drawVertexHandles &&
            currW.x >= clipMinX && currW.x <= clipMaxX &&
            currW.y >= clipMinY && currW.y <= clipMaxY) {
            scene.DrawCircle(currW, handleRadiusWorld, handleFill, strokeColor, strokeWidthWorld * 0.6f);
        }
    }

    if (closed && ring.size() >= 3) {
        const keel::f32 dx = firstW.x - prevW.x;
        const keel::f32 dy = firstW.y - prevW.y;
        if (dx * dx + dy * dy > 1e-6f) {
            drawClippedWorldSegment(prevW, firstW);
        }
    }
}

void RenderHighlightedFeatureOverlay(
    guikit::MapViewScope& map,
    guikit::SceneScope& scene,
    const geo::GeoFeature& feat,
    guikit::Color accentCol,
    guikit::Color handleFill,
    keel::f32 invZ,
    keel::f32 minSegWorldSq,
    bool drawHandles,
    keel::u64 frameTick,
    keel::usize& ioRenderedVerts,
    keel::usize* outPagedInCount
) {
    const bool wasLoaded = feat.geometryLoaded;
    if (!feat.EnsureGeometryLoaded(frameTick)) {
        return;
    }
    if (!wasLoaded && outPagedInCount) {
        ++(*outPagedInCount);
    }

    const keel::f32 strokeW = 2.4f * invZ;
    const keel::f32 glowW = 5.2f * invZ;
    const keel::f32 handleRadiusW = 3.2f * invZ;
    const guikit::Color glowCol = WithAlpha(accentCol, 0.30f);
    const bool allowHandles = drawHandles && (feat.VertexCount() <= 160);

    for (const geo::GeoPolygon& poly : feat.polygons) {
        DrawWorldRing(map, scene, poly.exteriorRing, glowCol, glowW, minSegWorldSq, true, false, 0.0f, handleFill, ioRenderedVerts);
        DrawWorldRing(map, scene, poly.exteriorRing, accentCol, strokeW, minSegWorldSq, true, allowHandles, handleRadiusW, handleFill, ioRenderedVerts);
        for (const auto& hole : poly.interiorRings) {
            DrawWorldRing(map, scene, hole, accentCol, strokeW * 0.85f, minSegWorldSq, true, allowHandles, handleRadiusW, handleFill, ioRenderedVerts);
        }
    }

    for (const auto& line : feat.lines) {
        DrawWorldRing(map, scene, line, glowCol, glowW, minSegWorldSq, false, false, 0.0f, handleFill, ioRenderedVerts);
        DrawWorldRing(map, scene, line, accentCol, strokeW, minSegWorldSq, false, allowHandles, handleRadiusW, handleFill, ioRenderedVerts);
    }

    for (const guikit::GeoCoord& pt : feat.points) {
        map.DrawGeoCircle(pt, 9.5f, WithAlpha(accentCol, 0.28f), accentCol, 2.0f);
        map.DrawGeoCircle(pt, 4.5f, accentCol, handleFill, 1.5f);
        ++ioRenderedVerts;
    }
}


} // namespace detail

keel::usize GisRenderEngine::EnforceFeatureRamBudget(
    keel::ArrayRef<geo::GeoLayer> layers,
    keel::usize maxResidentBytes,
    keel::u64 protectedTick,
    keel::usize* outResidentFeatures,
    keel::usize* outResidentBytes
) {
    keel::usize totalResidentBytes = 0;
    keel::usize totalResidentFeatures = 0;
    keel::usize streamedChunkBytes = 0;

    for (const geo::GeoLayer& layer : layers) {
        if (!layer.pager) continue;
        if (layer.isWindowStreamed) {
            const keel::usize chBytes = layer.pager->GetCachedChunkBytes();
            const keel::usize chFeats = layer.pager->GetCachedFeatureCount();
            totalResidentBytes += chBytes;
            streamedChunkBytes += chBytes;
            totalResidentFeatures += chFeats;
            if (layer.pointFeatureCount == 0) {
                for (const geo::GeoFeature& feat : layer.features) {
                    if (feat.residentHeapBytes > 0) {
                        totalResidentBytes += feat.residentHeapBytes;
                    }
                }
            }
        } else {
            for (const geo::GeoFeature& feat : layer.features) {
                if (feat.residentHeapBytes > 0) {
                    totalResidentBytes += feat.residentHeapBytes;
                    ++totalResidentFeatures;
                }
            }
        }
    }

    keel::usize evictedCount = 0;
    if (totalResidentBytes > maxResidentBytes) {
        const keel::usize targetBytes = (maxResidentBytes * 85u) / 100u;

        // 1. First evict cold decoded WKB feature payloads (polygons/lines) so lightweight chunk metadata stays cached
        {
            struct EvictCandidate {
                const geo::GeoFeature* feat{nullptr};
                keel::u64 lastAccessTick{0};
                keel::u32 bytes{0};
                bool isWindowStreamed{false};
            };
            keel::Array<EvictCandidate> evictPool;
            evictPool.reserve(keel::min<keel::usize>(totalResidentFeatures, 65536u));

            for (const geo::GeoLayer& layer : layers) {
                if (!layer.pager) continue;
                if (layer.isWindowStreamed && layer.pointFeatureCount > 0 &&
                    layer.polygonFeatureCount == 0 && layer.lineFeatureCount == 0) {
                    continue;
                }
                for (const geo::GeoFeature& feat : layer.features) {
                    if (feat.residentHeapBytes > 0 && feat.lastAccessTick < protectedTick) {
                        if (layer.isWindowStreamed && !feat.points.empty() &&
                            feat.polygons.empty() && feat.lines.empty()) {
                            continue;
                        }
                        evictPool.push_back({
                            .feat = &feat,
                            .lastAccessTick = feat.lastAccessTick,
                            .bytes = feat.residentHeapBytes,
                            .isWindowStreamed = layer.isWindowStreamed
                        });
                    }
                }
            }

            // Sort least-recently-used first; break ties by evicting largest allocations first
            keel::sort(evictPool,
                [](const EvictCandidate& a, const EvictCandidate& b) noexcept {
                    if (a.lastAccessTick != b.lastAccessTick) {
                        return a.lastAccessTick < b.lastAccessTick;
                    }
                    return a.bytes > b.bytes;
                });

            for (const EvictCandidate& cand : evictPool) {
                if (totalResidentBytes <= targetBytes) {
                    break;
                }
                const keel::usize beforeBytes = cand.feat->residentHeapBytes;
                cand.feat->UnloadGeometryPayload();
                cand.feat->UnloadPropertiesPayload();
                const keel::usize afterBytes = cand.feat->residentHeapBytes;
                if (beforeBytes > afterBytes) {
                    const keel::usize freed = beforeBytes - afterBytes;
                    totalResidentBytes = (totalResidentBytes > freed) ? (totalResidentBytes - freed) : 0;
                    if (!cand.isWindowStreamed && afterBytes == 0 && totalResidentFeatures > 0) {
                        --totalResidentFeatures;
                    }
                    ++evictedCount;
                }
            }
        }

        // 2. Only evict cold spatial metadata chunks if decoded geometry eviction was not enough
        if (totalResidentBytes > targetBytes && streamedChunkBytes > 0) {
            for (const geo::GeoLayer& layer : layers) {
                if (!layer.pager || !layer.isWindowStreamed) continue;
                const keel::usize lyrBytes = layer.pager->GetCachedChunkBytes();
                if (lyrBytes == 0) continue;
                const keel::usize lyrTarget = (maxResidentBytes == 0)
                    ? 0u
                    : keel::as<keel::usize>((keel::as<keel::f64>(targetBytes) * keel::as<keel::f64>(lyrBytes)) /
                                          keel::as<keel::f64>(keel::max<keel::usize>(1u, totalResidentBytes)));
                evictedCount += layer.pager->EvictChunksToBudget(lyrTarget, protectedTick);
            }

            // Recompute resident totals after chunk eviction
            totalResidentBytes = 0;
            totalResidentFeatures = 0;
            for (const geo::GeoLayer& layer : layers) {
                if (!layer.pager) continue;
                if (layer.isWindowStreamed) {
                    totalResidentBytes += layer.pager->GetCachedChunkBytes();
                    totalResidentFeatures += layer.pager->GetCachedFeatureCount();
                    if (layer.pointFeatureCount == 0) {
                        for (const geo::GeoFeature& feat : layer.features) {
                            if (feat.residentHeapBytes > 0) {
                                totalResidentBytes += feat.residentHeapBytes;
                            }
                        }
                    }
                } else {
                    for (const geo::GeoFeature& feat : layer.features) {
                        if (feat.residentHeapBytes > 0) {
                            totalResidentBytes += feat.residentHeapBytes;
                            ++totalResidentFeatures;
                        }
                    }
                }
            }
        }
    }

    if (outResidentFeatures) *outResidentFeatures = totalResidentFeatures;
    if (outResidentBytes) *outResidentBytes = totalResidentBytes;
    return evictedCount;
}


} // namespace guikit::geo
