#include "guikit/geo/map_view.hpp"
#include "guikit/context.hpp"
#include "guikit/geo/gis_render_engine.hpp"
#include "keel/core.h"
#include "keel/format.h"

namespace guikit {

namespace {

keel::u64 g_lastAnyMapViewOpenSeq = 0;

struct MapViewInternalState {
    bool cameraInitialized{false};
    MapCrs lastCrs{MapCrs::EPSG3857_WebMercator};
    GeoCoord lastCenterGeo{54.1825, 7.8853};
    keel::Optional<Vec2> lastMouseWorld{keel::kNullOpt};
    keel::u64 lastOpenSeq{0};

    void TrimInactiveCaches() noexcept {
        if (lastOpenSeq == g_lastAnyMapViewOpenSeq) {
            geo::GisRenderEngine::TrimInactiveCaches();
        }
    }
};

void RenderMapBackgroundLayers(
    SceneScope& scene,
    const MapViewOptions& opts,
    const MapViewState& view,
    bool lightMode
) {
    const Color tileEvenFill = lightMode ? Color::Rgba(238, 245, 252, 95) : Color::Rgba(18, 30, 46, 95);
    const Color tileOddFill  = lightMode ? Color::Rgba(226, 237, 248, 95) : Color::Rgba(24, 38, 58, 95);

    // Base OSM [z/x/y] Tile Pyramid Background & Optional Raster Tile Source
    for (const OsmTileCell& cell : view.visibleTiles) {
        const Rect& r = cell.worldRect;
        const bool isCheckerOdd = ((cell.id.x + cell.id.y) & 1) != 0;
        const Color cellFill = isCheckerOdd ? tileOddFill : tileEvenFill;

        bool drewBaseTile = false;
        if (opts.tileSource) {
            const keel::StringRef baseUri = opts.tileSource(cell.id, 0);
            if (!baseUri.empty()) {
                scene.DrawImage(baseUri, r);
                drewBaseTile = true;
            }
            const keel::StringRef overlayUri = opts.tileSource(cell.id, 1);
            if (!overlayUri.empty()) {
                scene.DrawImage(overlayUri, r);
            }
        }

        if (!drewBaseTile) {
            scene.DrawRect(r, cellFill);
        }
    }
}

} // namespace

MapViewScope::MapViewScope(
    SceneScope&& scene,
    const MapViewOptions& opts,
    const MapViewState& viewState,
    bool lightMode
) noexcept
    : m_scene(keel::move(scene)),
      m_opts(opts),
      m_view(viewState.clone()),
      m_lightMode(lightMode),
      m_active(static_cast<bool>(m_scene)) {}

MapViewScope::MapViewScope(
    SceneScope&& scene,
    const MapViewOptions& opts,
    MapViewState&& viewState,
    bool lightMode
) noexcept
    : m_scene(keel::move(scene)),
      m_opts(opts),
      m_view(keel::move(viewState)),
      m_lightMode(lightMode),
      m_active(static_cast<bool>(m_scene)) {}

MapViewScope::MapViewScope(MapViewScope&& other) noexcept
    : m_scene(keel::move(other.m_scene)),
      m_opts(other.m_opts),
      m_view(keel::move(other.m_view)),
      m_lightMode(other.m_lightMode),
      m_active(other.m_active) {
    other.m_active = false;
}

MapViewScope::~MapViewScope() noexcept {
    if (m_active && static_cast<bool>(m_scene)) {
        RenderForegroundOverlays();
    }
}

Vec2 MapViewScope::GeoToWorld(GeoCoord coord) const noexcept {
    return MapGrid::GeoToWorld(coord, m_opts.worldSize, m_opts.crs);
}

GeoCoord MapViewScope::WorldToGeo(Vec2 worldPt) const noexcept {
    return MapGrid::WorldToGeo(worldPt, m_opts.worldSize, m_opts.crs);
}

Vec2 MapViewScope::GeoToScreen(GeoCoord coord) const noexcept {
    return m_scene.WorldToScreen(GeoToWorld(coord));
}

GeoCoord MapViewScope::ScreenToGeo(Vec2 screenPt) const noexcept {
    return WorldToGeo(m_scene.ScreenToWorld(screenPt));
}

void MapViewScope::CenterOnGeo(GeoCoord coord, keel::Optional<keel::f32> tileZoom) noexcept {
    keel::Optional<keel::f32> sceneZoom{keel::kNullOpt};
    if (tileZoom.has_value()) {
        sceneZoom = MapGrid::TileZoomToSceneZoom(*tileZoom, m_opts.baseZoom);
    }
    m_scene.CenterOn(GeoToWorld(coord), sceneZoom);
}

void MapViewScope::ResetView() noexcept {
    CenterOnGeo(m_opts.initialCenter, m_opts.initialTileZoom);
}

SceneItemScope MapViewScope::GeoItem(
    GeoCoord coord,
    keel::f32 hitRadiusPx,
    const SceneItemOptions& opts
) {
    const keel::f32 invZ = 1.0f / keel::max(0.0001f, m_scene.GetZoom());
    const Vec2 wPos = GeoToWorld(coord);
    const keel::f32 r = keel::max(2.0f, hitRadiusPx) * invZ;
    SceneItemOptions itemOpts = opts;
    if (!itemOpts.hitRect.has_value()) {
        itemOpts.hitRect = Rect(wPos.x - r, wPos.y - r, r * 2.0f, r * 2.0f);
    }
    return m_scene.Item(itemOpts);
}

void MapViewScope::DrawGeoLine(
    GeoCoord from,
    GeoCoord to,
    Color color,
    keel::f32 strokeWidthPx
) {
    const keel::f32 invZ = 1.0f / keel::max(0.0001f, m_scene.GetZoom());
    m_scene.DrawLine(GeoToWorld(from), GeoToWorld(to), color, strokeWidthPx * invZ);
}

void MapViewScope::DrawGeoPolyline(
    keel::ArrayRef<GeoCoord> coords,
    Color color,
    keel::f32 strokeWidthPx,
    bool closed
) {
    if (coords.size() < 2) return;
    const keel::f32 invZ = 1.0f / keel::max(0.0001f, m_scene.GetZoom());
    const keel::f32 w = strokeWidthPx * invZ;
    for (keel::usize i = 0; i + 1 < coords.size(); ++i) {
        m_scene.DrawLine(GeoToWorld(coords[i]), GeoToWorld(coords[i + 1]), color, w);
    }
    if (closed && coords.size() >= 3) {
        m_scene.DrawLine(GeoToWorld(coords.back()), GeoToWorld(coords.front()), color, w);
    }
}

void MapViewScope::DrawGeoCircle(
    GeoCoord center,
    keel::f32 radiusPx,
    Color fill,
    Color stroke,
    keel::f32 strokeWidthPx
) {
    const keel::f32 invZ = 1.0f / keel::max(0.0001f, m_scene.GetZoom());
    m_scene.DrawCircle(GeoToWorld(center), radiusPx * invZ, fill, stroke, strokeWidthPx * invZ);
}

void MapViewScope::DrawGeoArc(
    GeoCoord center,
    keel::f32 radiusPx,
    keel::f32 startAngleDeg,
    keel::f32 sweepAngleDeg,
    Color stroke,
    keel::f32 strokeWidthPx,
    keel::i32 segments
) {
    const keel::f32 invZ = 1.0f / keel::max(0.0001f, m_scene.GetZoom());
    m_scene.DrawArc(
        GeoToWorld(center),
        radiusPx * invZ,
        startAngleDeg,
        sweepAngleDeg,
        stroke,
        strokeWidthPx * invZ,
        segments
    );
}

void MapViewScope::DrawGeoText(
    keel::StringRef text,
    GeoCoord coord,
    Vec2 offsetPx,
    keel::f32 fontSize,
    Color color,
    FontStyle style
) {
    const keel::f32 invZ = 1.0f / keel::max(0.0001f, m_scene.GetZoom());
    const Vec2 wPos = GeoToWorld(coord);
    m_scene.DrawText(
        text,
        {wPos.x + offsetPx.x * invZ, wPos.y + offsetPx.y * invZ},
        fontSize,
        color,
        style,
        /*scaleWithZoom=*/false
    );
}

SceneItemScope MapViewScope::DrawSeamark(
    GeoCoord coord,
    const MapSeamarkOptions& opts
) {
    const keel::f32 invZ = 1.0f / keel::max(0.0001f, m_scene.GetZoom());
    const Vec2 wPos = GeoToWorld(coord);
    const keel::f32 r = keel::max(2.0f, opts.radiusPx) * invZ;
    const Rect hitBox(wPos.x - r * 2.0f, wPos.y - r * 2.0f, r * 4.0f, r * 4.0f);

    SceneItemScope item = m_scene.Item({
        .id = opts.id,
        .title = opts.title,
        .category = opts.category,
        .hitRect = hitBox,
        .selectable = opts.selectable
    });

    const Color seamarkMagenta = m_lightMode ? Color::Rgba(185, 28, 140, 235) : Color::Rgba(232, 121, 249, 240);
    const Color seamarkCyan    = m_lightMode ? Color::Rgba(2, 132, 199, 230)  : Color::Rgba(56, 189, 248, 235);
    const Color baseCol = opts.color.value_or(seamarkMagenta);
    const Color markCol = (item.IsSelected() || item.IsHovered()) ? seamarkCyan : baseCol;

    if (opts.showLightSector) {
        m_scene.DrawArc(wPos, r * 1.9f, opts.sectorStartDeg, opts.sectorSweepDeg, markCol, 1.8f * invZ, 16);
    }

    // IALA diamond buoy / beacon silhouette
    m_scene.DrawTriangle(
        {wPos.x, wPos.y - r * 1.25f},
        {wPos.x - r * 0.9f, wPos.y},
        {wPos.x + r * 0.9f, wPos.y},
        markCol.WithAlpha(70),
        markCol,
        1.5f * invZ
    );
    m_scene.DrawTriangle(
        {wPos.x, wPos.y + r * 1.25f},
        {wPos.x - r * 0.9f, wPos.y},
        {wPos.x + r * 0.9f, wPos.y},
        markCol.WithAlpha(70),
        markCol,
        1.5f * invZ
    );
    m_scene.DrawCircle(wPos, 2.5f * invZ, markCol);

    if (!opts.title.empty()) {
        m_scene.DrawText(
            opts.title,
            {wPos.x + (opts.radiusPx + 4.0f) * invZ, wPos.y - 6.0f * invZ},
            11.0f,
            markCol,
            FontStyle::Bold,
            /*scaleWithZoom=*/false
        );
    }

    return item;
}

void MapViewScope::RenderForegroundOverlays() {
    const keel::f32 z = keel::max(0.0001f, m_scene.GetZoom());
    const keel::f32 invZ = 1.0f / z;
    const Rect visWorld = m_scene.GetVisibleWorldRect();

    const Color seamarkMagenta = m_lightMode ? Color::Rgba(185, 28, 140, 235) : Color::Rgba(232, 121, 249, 240);
    const Color seamarkCyan    = m_lightMode ? Color::Rgba(2, 132, 199, 230)  : Color::Rgba(56, 189, 248, 235);

    // 1. OpenSeaMap Nautical Graticule (Degrees & Nautical Arc-Minutes, 1' = 1 NM) — rendered over chart tiles
    if (m_opts.showNauticalGraticule) {
        const Color gratMinorCol = m_lightMode ? Color::Rgba(15, 118, 110, 105)  : Color::Rgba(20, 184, 166, 110);
        const Color gratMajorCol = m_lightMode ? Color::Rgba(13, 148, 136, 175)  : Color::Rgba(45, 212, 191, 175);
        const Color gratZeroCol  = m_lightMode ? Color::Rgba(220, 38, 38, 200)   : Color::Rgba(248, 113, 113, 205);

        const keel::f32 vy0 = keel::clamp(visWorld.y, 0.0f, m_opts.worldSize);
        const keel::f32 vy1 = keel::clamp(visWorld.Bottom(), 0.0f, m_opts.worldSize);
        const keel::f32 vx0 = keel::clamp(visWorld.x, 0.0f, m_opts.worldSize);
        const keel::f32 vx1 = keel::clamp(visWorld.Right(), 0.0f, m_opts.worldSize);

        if (MapGrid::IsCylindricalCrs(m_opts.crs)) {
            for (const GraticuleLine& m : m_view.meridians) {
                const Color col = m.isZero ? gratZeroCol : (m.isMajor ? gratMajorCol : gratMinorCol);
                const keel::f32 w = (m.isZero ? 1.8f : (m.isMajor ? 1.25f : 0.85f)) * invZ;
                m_scene.DrawLine({m.worldPos, vy0}, {m.worldPos, vy1}, col, w);
            }

            for (const GraticuleLine& p : m_view.parallels) {
                const Color col = p.isZero ? gratZeroCol : (p.isMajor ? gratMajorCol : gratMinorCol);
                const keel::f32 w = (p.isZero ? 1.8f : (p.isMajor ? 1.25f : 0.85f)) * invZ;
                m_scene.DrawLine({vx0, p.worldPos}, {vx1, p.worldPos}, col, w);
            }
        } else {
            // Curved / converging meridians & parallels for Transverse Mercator (UTM / GSK-2011 / SK-42) and Polar Stereographic
            const keel::f64 minLat = keel::clamp(m_view.visibleBounds.se.lat, -89.9, 89.9);
            const keel::f64 maxLat = keel::clamp(m_view.visibleBounds.nw.lat, -89.9, 89.9);
            const keel::f64 minLon = keel::clamp(m_view.visibleBounds.nw.lon, -180.0, 180.0);
            const keel::f64 maxLon = keel::clamp(m_view.visibleBounds.se.lon, -180.0, 180.0);
            constexpr keel::i32 kMeridianSegs = 12;
            constexpr keel::i32 kParallelSegs = 18;

            for (const GraticuleLine& m : m_view.meridians) {
                const Color col = m.isZero ? gratZeroCol : (m.isMajor ? gratMajorCol : gratMinorCol);
                const keel::f32 w = (m.isZero ? 1.8f : (m.isMajor ? 1.25f : 0.85f)) * invZ;
                Vec2 prevW = GeoToWorld({minLat, m.degrees});
                for (keel::i32 s = 1; s <= kMeridianSegs; ++s) {
                    const keel::f64 t = keel::as<keel::f64>(s) / keel::as<keel::f64>(kMeridianSegs);
                    const keel::f64 lat = minLat + t * (maxLat - minLat);
                    const Vec2 currW = GeoToWorld({lat, m.degrees});
                    m_scene.DrawLine(prevW, currW, col, w);
                    prevW = currW;
                }
            }

            for (const GraticuleLine& p : m_view.parallels) {
                const Color col = p.isZero ? gratZeroCol : (p.isMajor ? gratMajorCol : gratMinorCol);
                const keel::f32 w = (p.isZero ? 1.8f : (p.isMajor ? 1.25f : 0.85f)) * invZ;
                Vec2 prevW = GeoToWorld({p.degrees, minLon});
                for (keel::i32 s = 1; s <= kParallelSegs; ++s) {
                    const keel::f64 t = keel::as<keel::f64>(s) / keel::as<keel::f64>(kParallelSegs);
                    const keel::f64 lon = minLon + t * (maxLon - minLon);
                    const Vec2 currW = GeoToWorld({p.degrees, lon});
                    m_scene.DrawLine(prevW, currW, col, w);
                    prevW = currW;
                }
            }
        }
    }

    // 2. OSM [z/x/y] Tile Pyramid Wireframe & Coordinates — rendered over chart tiles when enabled
    if (m_opts.showTileGrid) {
        const Color tileBorderCol = m_lightMode ? Color::Rgba(59, 130, 246, 150)  : Color::Rgba(56, 189, 248, 145);
        const Color subTileCol    = m_lightMode ? Color::Rgba(59, 130, 246, 65)   : Color::Rgba(56, 189, 248, 60);
        const Color tileIdTextCol = m_lightMode ? Color::Rgba(30, 58, 138, 215)   : Color::Rgba(125, 211, 252, 215);
        const Color tileMetaCol   = m_lightMode ? Color::Rgba(51, 65, 85, 185)    : Color::Rgba(148, 163, 184, 185);

        for (const OsmTileCell& cell : m_view.visibleTiles) {
            const Rect& r = cell.worldRect;
            const keel::f32 screenTileW = r.w * z;

            if (m_opts.showSubTileQuadtree && screenTileW >= 110.0f) {
                const keel::f32 midX = r.x + r.w * 0.5f;
                const keel::f32 midY = r.y + r.h * 0.5f;
                m_scene.DrawLine({midX, r.y}, {midX, r.Bottom()}, subTileCol, 1.0f * invZ);
                m_scene.DrawLine({r.x, midY}, {r.Right(), midY}, subTileCol, 1.0f * invZ);
            }

            m_scene.DrawRect(r, Color::Transparent(), 0.0f, tileBorderCol, 1.0f * invZ);

            if (screenTileW >= 64.0f) {
                char labelBuf[48]{};
                const keel::StringRef labelSv = keel::format_buf(
                    labelBuf, "z=%d  [%d, %d]", cell.id.z, cell.id.x, cell.id.y);
                m_scene.DrawTextBox(
                    labelSv,
                    Rect(r.x, r.y + r.h * 0.34f, r.w, r.h * 0.18f),
                    12.0f,
                    tileIdTextCol,
                    0.5f,
                    0.5f,
                    FontStyle::Monospace,
                    /*scaleWithZoom=*/false
                );
            }

            if (screenTileW >= 150.0f) {
                char uriBuf[64]{};
                const keel::StringRef uriSv = keel::format_buf(
                    uriBuf, "/tile/%d/%d/%d.png", cell.id.z, cell.id.x, cell.id.y);
                m_scene.DrawTextBox(
                    uriSv,
                    Rect(r.x, r.y + r.h * 0.52f, r.w, r.h * 0.14f),
                    10.5f,
                    tileMetaCol,
                    0.5f,
                    0.5f,
                    FontStyle::Monospace,
                    /*scaleWithZoom=*/false
                );

                const auto nwText = MapGrid::FormatCoord(cell.geoBounds.nw, m_opts.coordFormat, m_opts.crs);
                m_scene.DrawText(
                    nwText.as_ref(),
                    {r.x + 6.0f * invZ, r.y + 6.0f * invZ},
                    9.5f,
                    tileMetaCol,
                    FontStyle::Monospace,
                    /*scaleWithZoom=*/false
                );
            }
        }
    }

    // 3. OpenSeaMap Nautical Border Rulers (Top Longitude & Left Latitude Bars)
    if (m_opts.showBorderRulers) {
        const keel::f32 rulerThick = 20.0f * invZ;
        const keel::f32 leftBarW = rulerThick * 3.9f;
        const keel::f32 topRulerStartX = visWorld.x + leftBarW;
        const keel::f32 leftRulerStartY = visWorld.y + rulerThick;

        const Color rulerBg     = m_lightMode ? Color::Rgba(248, 250, 252, 236) : Color::Rgba(11, 18, 30, 236);
        const Color rulerBorder = m_lightMode ? Color::Rgba(148, 163, 184, 200) : Color::Rgba(51, 65, 85, 210);
        const Color rulerText   = m_lightMode ? Color::Rgba(15, 23, 42, 240)    : Color::Rgba(226, 232, 240, 240);
        const Color tickAccent  = m_lightMode ? Color::Rgba(13, 148, 136, 220)  : Color::Rgba(45, 212, 191, 220);

        const Rect topBar(topRulerStartX, visWorld.y, keel::max(0.0f, visWorld.w - leftBarW), rulerThick);
        const Rect leftBar(visWorld.x, leftRulerStartY, leftBarW, keel::max(0.0f, visWorld.h - rulerThick));
        m_scene.DrawRect(topBar, rulerBg);
        m_scene.DrawRect(leftBar, rulerBg);

        for (keel::usize i = 0; i + 1 < m_view.meridians.size(); ++i) {
            const GraticuleLine& m = m_view.meridians[i];
            const keel::i32 absIdx = m.stepIndex < 0 ? -m.stepIndex : m.stepIndex;
            if ((absIdx & 1) == 0) {
                const keel::f32 nextX = m_view.meridians[i + 1].worldPos;
                const keel::f32 x0 = keel::max(topRulerStartX, keel::min(m.worldPos, nextX));
                const keel::f32 x1 = keel::min(visWorld.Right(), keel::max(m.worldPos, nextX));
                if (x1 > x0) {
                    m_scene.DrawRect(
                        Rect(x0, visWorld.y + rulerThick - 4.0f * invZ, x1 - x0, 4.0f * invZ),
                        tickAccent.WithAlpha(95)
                    );
                }
            }
        }

        for (keel::usize i = 0; i + 1 < m_view.parallels.size(); ++i) {
            const GraticuleLine& p = m_view.parallels[i];
            const keel::i32 absIdx = p.stepIndex < 0 ? -p.stepIndex : p.stepIndex;
            if ((absIdx & 1) == 0) {
                const keel::f32 nextY = m_view.parallels[i + 1].worldPos;
                const keel::f32 y0 = keel::max(leftRulerStartY, keel::min(p.worldPos, nextY));
                const keel::f32 y1 = keel::min(visWorld.Bottom(), keel::max(p.worldPos, nextY));
                if (y1 > y0) {
                    m_scene.DrawRect(
                        Rect(leftBar.Right() - 4.0f * invZ, y0, 4.0f * invZ, y1 - y0),
                        tickAccent.WithAlpha(95)
                    );
                }
            }
        }

        m_scene.DrawLine({topRulerStartX, visWorld.y + rulerThick}, {visWorld.Right(), visWorld.y + rulerThick}, rulerBorder, 1.0f * invZ);
        m_scene.DrawLine({leftBar.Right(), leftBar.y}, {leftBar.Right(), leftBar.Bottom()}, rulerBorder, 1.0f * invZ);

        for (const GraticuleLine& m : m_view.meridians) {
            if (m.worldPos >= topRulerStartX && m.worldPos <= visWorld.Right()) {
                m_scene.DrawLine({m.worldPos, visWorld.y}, {m.worldPos, visWorld.y + rulerThick}, tickAccent, 1.3f * invZ);
            }
        }
        for (const GraticuleLine& p : m_view.parallels) {
            if (p.worldPos >= leftRulerStartY && p.worldPos <= visWorld.Bottom()) {
                m_scene.DrawLine({leftBar.x, p.worldPos}, {leftBar.Right(), p.worldPos}, tickAccent, 1.3f * invZ);
            }
        }

        m_scene.DrawRect(Rect(visWorld.x, visWorld.y, leftBarW, rulerThick), rulerBg, 0.0f, rulerBorder, 1.0f * invZ);

        for (const GraticuleLine& m : m_view.meridians) {
            if (m.worldPos >= topRulerStartX && m.worldPos + 44.0f * invZ <= visWorld.Right()) {
                const auto lbl = MapGrid::FormatGraticuleTick(m.degrees, /*isLat=*/false, m_view.graticuleStepArcMin);
                m_scene.DrawText(lbl.as_ref(), {m.worldPos + 4.0f * invZ, visWorld.y + 3.0f * invZ}, 10.0f, rulerText, FontStyle::Monospace, false);
            }
        }
        for (const GraticuleLine& p : m_view.parallels) {
            if (p.worldPos >= leftRulerStartY + 14.0f * invZ && p.worldPos <= visWorld.Bottom()) {
                const auto lbl = MapGrid::FormatGraticuleTick(p.degrees, /*isLat=*/true, m_view.graticuleStepArcMin);
                m_scene.DrawText(lbl.as_ref(), {leftBar.x + 4.0f * invZ, p.worldPos - 13.0f * invZ}, 10.0f, rulerText, FontStyle::Monospace, false);
            }
        }

        char stepBuf[32]{};
        const keel::StringRef stepSv = (m_view.graticuleStepArcMin >= 60.0)
            ? keel::format_buf(stepBuf, "Grid %.0f°", m_view.graticuleStepArcMin / 60.0)
            : keel::format_buf(stepBuf, "Grid %.1f' NM", m_view.graticuleStepArcMin);
        m_scene.DrawText(stepSv, {visWorld.x + 4.0f * invZ, visWorld.y + 3.0f * invZ}, 10.0f, tickAccent, FontStyle::Bold, false);
    }

    // 2. Dual Scale Bar Overlay (Nautical Miles NM/cab + Metric km/m)
    if (m_opts.showScaleBarOverlay) {
        const keel::f32 leftOffset = m_opts.showBorderRulers ? 90.0f : 14.0f;
        const keel::f32 boxW = 240.0f * invZ;
        const keel::f32 boxH = 54.0f * invZ;
        const keel::f32 boxX = visWorld.x + leftOffset * invZ;
        const keel::f32 boxY = visWorld.Bottom() - (boxH + 14.0f * invZ);

        const Color hudBg     = m_lightMode ? Color::Rgba(255, 255, 255, 232) : Color::Rgba(11, 18, 30, 232);
        const Color hudBorder = m_lightMode ? Color::Rgba(148, 163, 184, 210) : Color::Rgba(51, 65, 85, 220);
        const Color hudText   = m_lightMode ? Color::Rgba(15, 23, 42, 245)    : Color::Rgba(226, 232, 240, 245);

        m_scene.DrawRect(Rect(boxX, boxY, boxW, boxH), hudBg, 5.0f * invZ, hudBorder, 1.0f * invZ);

        const keel::f32 nmLenWorld = m_view.nauticalScale.screenPixels * invZ;
        const keel::f32 nmY = boxY + 18.0f * invZ;
        const keel::f32 barStartX = boxX + 10.0f * invZ;
        m_scene.DrawLine({barStartX, nmY}, {barStartX + nmLenWorld, nmY}, seamarkMagenta, 2.2f * invZ);
        m_scene.DrawLine({barStartX, nmY - 4.0f * invZ}, {barStartX, nmY + 4.0f * invZ}, seamarkMagenta, 1.8f * invZ);
        m_scene.DrawLine({barStartX + nmLenWorld, nmY - 4.0f * invZ}, {barStartX + nmLenWorld, nmY + 4.0f * invZ}, seamarkMagenta, 1.8f * invZ);
        m_scene.DrawText(m_view.nauticalScale.label, {barStartX + nmLenWorld + 8.0f * invZ, nmY - 7.0f * invZ}, 10.5f, hudText, FontStyle::Monospace, false);

        const keel::f32 mLenWorld = m_view.metricScale.screenPixels * invZ;
        const keel::f32 mY = boxY + 38.0f * invZ;
        m_scene.DrawLine({barStartX, mY}, {barStartX + mLenWorld, mY}, seamarkCyan, 2.2f * invZ);
        m_scene.DrawLine({barStartX, mY - 4.0f * invZ}, {barStartX, mY + 4.0f * invZ}, seamarkCyan, 1.8f * invZ);
        m_scene.DrawLine({barStartX + mLenWorld, mY - 4.0f * invZ}, {barStartX + mLenWorld, mY + 4.0f * invZ}, seamarkCyan, 1.8f * invZ);
        m_scene.DrawText(m_view.metricScale.label, {barStartX + mLenWorld + 8.0f * invZ, mY - 7.0f * invZ}, 10.5f, hudText, FontStyle::Monospace, false);
    }
}

MapViewScope MapView::Open(
    Context& ctx,
    const MapViewOptions& opts,
    keel::SourceLocation loc
) {
    const Id id = ctx.MakeChildId(loc);
    auto& mapState = ctx.GetOrCreatePersistentState<MapViewInternalState>(id);
    auto& sceneState = ctx.GetOrCreatePersistentState<SceneState>(id);
    mapState.lastOpenSeq = ++g_lastAnyMapViewOpenSeq;

    const keel::f32 worldSize = keel::max(256.0f, opts.worldSize);

    // Pre-schedule camera centering before Scene::Open runs UpdateSceneCameraTransform
    if (!mapState.cameraInitialized || !sceneState.initialized || opts.requestResetView) {
        mapState.cameraInitialized = true;
        mapState.lastCrs = opts.crs;
        mapState.lastCenterGeo = opts.initialCenter;
        sceneState.pendingCenterWorld = MapGrid::GeoToWorld(opts.initialCenter, worldSize, opts.crs);
        sceneState.pendingCenterZoom = MapGrid::TileZoomToSceneZoom(opts.initialTileZoom, opts.baseZoom);
    } else if (opts.requestCenterGeo.has_value() || opts.requestTileZoom.has_value()) {
        const GeoCoord targetGeo = opts.requestCenterGeo.value_or(mapState.lastCenterGeo);
        sceneState.pendingCenterWorld = MapGrid::GeoToWorld(targetGeo, worldSize, opts.crs);
        if (opts.requestTileZoom.has_value()) {
            sceneState.pendingCenterZoom = MapGrid::TileZoomToSceneZoom(*opts.requestTileZoom, opts.baseZoom);
        }
        mapState.lastCrs = opts.crs;
        mapState.lastCenterGeo = targetGeo;
    } else if (opts.crs != mapState.lastCrs) {
        // Preserve geographic center across CRS projection switches
        sceneState.pendingCenterWorld = MapGrid::GeoToWorld(mapState.lastCenterGeo, worldSize, opts.crs);
        mapState.lastCrs = opts.crs;
    }

    MapViewOptions safeOpts = opts;
    safeOpts.tooltip = ctx.AllocText(opts.tooltip);

    const SceneOptions sceneOpts{
        .worldSize = Vec2{worldSize, worldSize},
        .backgroundStyle = SceneBackground::Solid,
        .width = safeOpts.width,
        .height = safeOpts.height,
        .minHeight = safeOpts.minHeight,
        .minZoom = safeOpts.minZoom,
        .maxZoom = safeOpts.maxZoom,
        .zoomStep = safeOpts.zoomStep,
        .zoomable = safeOpts.zoomable,
        .pannable = safeOpts.pannable,
        .selectableItems = safeOpts.selectableItems,
        .showScrollbars = safeOpts.showScrollbars,
        .showControls = safeOpts.showControls,
        .showZoomPercent = false,
        .inspectorPlacement = SceneInspectorPlacement::TopLeft,
        .inspectorOffset = safeOpts.showBorderRulers ? Vec2{80.0f, 20.0f} : Vec2{4.0f, 0.0f},
        .clampToWorld = safeOpts.clampToWorld,
        .background = safeOpts.background,
        .border = safeOpts.border,
        .tooltip = safeOpts.tooltip,
        .size = safeOpts.size,
        .disabled = safeOpts.disabled
    };

    SceneScope scene = guikit::OpenSceneWithId(ctx, id, sceneOpts);
    const UiTheme& activeTheme = ctx.GetTheme();
    const bool lightMode = (activeTheme.mode == ThemeMode::Light);

    const keel::f32 z = keel::max(0.0001f, scene.GetZoom());
    const Rect vp = scene.GetViewportRect();
    const Rect visWorld = scene.GetVisibleWorldRect();
    Vec2 mouseWorld = scene.GetMouseWorldPos();
    if (scene.IsHovered()) {
        mapState.lastMouseWorld = mouseWorld;
    } else if (mapState.lastMouseWorld.has_value()) {
        mouseWorld = *mapState.lastMouseWorld;
    } else {
        mouseWorld = visWorld.Center();
    }

    const MapGridConfig gridCfg{
        .crs = safeOpts.crs,
        .worldSize = worldSize,
        .baseZoom = safeOpts.baseZoom,
        .minTileZoom = safeOpts.minTileZoom,
        .maxTileZoom = safeOpts.maxTileZoom,
        .zoomBias = safeOpts.zoomBias,
        .tileSource = safeOpts.tileSource
    };

    MapViewState view = MapGrid::ComputeView(visWorld, vp, z, mouseWorld, gridCfg);
    if (vp.w > 1.0f && vp.h > 1.0f) {
        mapState.lastCenterGeo = view.center;
    }

    RenderMapBackgroundLayers(scene, safeOpts, view, lightMode);
    safeOpts.tileSource = nullptr;

    return MapViewScope(keel::move(scene), safeOpts, keel::move(view), lightMode);
}

} // namespace guikit
