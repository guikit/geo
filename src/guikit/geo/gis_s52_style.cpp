#include "gis_render_internal.hpp"
#include "guikit/geo/polygon_triangulator.hpp"
#include "keel/format.h"
#include "keel/math.h"
#include "keel/time.h"
#include <cstring>

namespace guikit::geo {

namespace detail {

keel::Atomic<GisRenderEngine::BreadcrumbCallback> g_breadcrumbCallback{nullptr};

namespace {

keel::Mutex& GetCanvasLifecycleMutex() noexcept {
    static keel::Mutex s_mtx;
    return s_mtx;
}

} // namespace

void EmitRenderBreadcrumb(const char* stage, keel::i32 z, keel::i32 x, keel::i32 y) noexcept {
    if (auto cb = g_breadcrumbCallback.load(keel::MemoryOrder::Relaxed)) {
        cb(stage, z, x, y);
    }
}

keel::Mutex& GetDefaultBasemapMutex() noexcept {
    static keel::Mutex s_mtx;
    return s_mtx;
}

keel::Arc<IRasterTileProvider>& GetDefaultBasemapStorage() {
    static keel::Arc<IRasterTileProvider> s_provider;
    return s_provider;
}

keel::Arc<IRasterTileProvider>& GetLastActiveBasemapStorage() {
    static keel::Arc<IRasterTileProvider> s_lastProvider;
    return s_lastProvider;
}

keel::UniquePtr<guikit::ICanvas> CreateCanvasThreadSafe() {
    keel::LockGuard<keel::Mutex> lk(GetCanvasLifecycleMutex());
    return guikit::CreateOffscreenVectorCanvas();
}

void DestroyCanvasThreadSafe(keel::UniquePtr<guikit::ICanvas>& canvas) noexcept {
    if (!canvas) return;
    keel::LockGuard<keel::Mutex> lk(GetCanvasLifecycleMutex());
    canvas.reset();
}

[[nodiscard]] bool IsCautionOrRestrictedArea(const geo::GeoFeature& feat) noexcept {
    if (feat.chartClass != geo::ChartFeatureClass::SeabedOrMarineArea) return false;
    return feat.featureCode == "CTNARE" ||
           feat.featureCode == "RESARE" ||
           feat.featureCode == "TSEZNE" ||
           feat.featureCode == "MIPARE" ||
           feat.featureCode == "SPLARE" ||
           feat.featureCode == "FSHZNE" ||
           feat.featureCode == "ICNARE";
}

// ============================================================================
// Color & Hash Utilities
// ============================================================================

[[nodiscard]] guikit::Color WithAlpha(guikit::Color c, keel::f32 alphaFactor) noexcept {
    const keel::f32 a = keel::clamp(alphaFactor * 255.0f, 0.0f, 255.0f);
    return guikit::Color(c.r, c.g, c.b, keel::as<keel::u8>(keel::round_i32(a)));
}

[[nodiscard]] guikit::Color BrightenColor(guikit::Color c, keel::i32 delta) noexcept {
    return guikit::Color(
        keel::saturate<keel::u8>(keel::as<keel::i32>(c.r) + delta),
        keel::saturate<keel::u8>(keel::as<keel::i32>(c.g) + delta),
        keel::saturate<keel::u8>(keel::as<keel::i32>(c.b) + delta),
        c.a
    );
}

[[nodiscard]] guikit::Color LerpColor(guikit::Color a, guikit::Color b, keel::f32 t) noexcept {
    const keel::f32 ct = keel::clamp(t, 0.0f, 1.0f);
    return guikit::Color(
        keel::saturate<keel::u8>(keel::round_i32(keel::lerp(keel::as<keel::f32>(a.r), keel::as<keel::f32>(b.r), ct))),
        keel::saturate<keel::u8>(keel::round_i32(keel::lerp(keel::as<keel::f32>(a.g), keel::as<keel::f32>(b.g), ct))),
        keel::saturate<keel::u8>(keel::round_i32(keel::lerp(keel::as<keel::f32>(a.b), keel::as<keel::f32>(b.b), ct))),
        keel::saturate<keel::u8>(keel::round_i32(keel::lerp(keel::as<keel::f32>(a.a), keel::as<keel::f32>(b.a), ct)))
    );
}

// ============================================================================
// IHO S-52 Marine Symbology & Depth Shading Resolver
// ============================================================================

[[nodiscard]] bool ShouldSkipFeatureAtZoom(
    const geo::GeoFeature& feat,
    keel::i32 tileZoom,
    const GisRenderConfig& cfg
) noexcept {
    if (feat.chartClass == geo::ChartFeatureClass::CoverageMask) {
        return !cfg.showCoverageBounds;
    }
    if (!cfg.showCautionAndRestrictedZones && IsCautionOrRestrictedArea(feat)) {
        return true;
    }
    if (feat.chartClass == geo::ChartFeatureClass::DepthContour && !cfg.showDepthContours) {
        return true;
    }
    if (feat.chartClass == geo::ChartFeatureClass::Sounding && !cfg.showSoundings) {
        return true;
    }
    if (!cfg.scaleFiltering) {
        return false;
    }

    // Progressive detail disclosure by zoom level for dense bathymetric contours
    if (feat.chartClass == geo::ChartFeatureClass::DepthContour) {
        const keel::f32 d = feat.depthMeters;
        if (!keel::is_nan(d)) {
            const keel::f32 twiceD = keel::round(d * 2.0f);
            const bool isHalfMeter = (keel::abs(twiceD - keel::round(d) * 2.0f) > 0.2f);
            const bool isFiveMeter = (keel::abs(keel::fmod(d, 5.0f)) < 0.15f) || (keel::abs(d - cfg.safetyContourM) < 0.15f);
            if (tileZoom <= 9 && !isFiveMeter) {
                return true;
            }
            if (tileZoom <= 11 && isHalfMeter) {
                return true;
            }
        }
    }

    return false;
}

[[nodiscard]] guikit::Color SampleBathymetryColor(
    keel::f32 depthM,
    const GisRenderConfig& cfg
) noexcept {
    const keel::f32 safeShallow = keel::max(0.2f, keel::min(cfg.shallowContourM, cfg.safetyContourM));
    const keel::f32 safeContour = keel::max(safeShallow + 0.2f, cfg.safetyContourM);
    const keel::f32 safeDeep    = keel::max(safeContour + 1.0f, cfg.deepContourM);

    if (cfg.chartPalette == ChartPalette::MarineNight) {
        if (depthM <= 0.02f) return guikit::Color(22, 58, 48, 255);     // Intertidal dark teal
        if (depthM <= safeShallow) return guikit::Color(14, 68, 108, 255);
        if (depthM <= safeContour) {
            const keel::f32 t = (depthM - safeShallow) / keel::max(0.1f, safeContour - safeShallow);
            return LerpColor(guikit::Color(14, 68, 108, 255), guikit::Color(11, 48, 80, 255), t);
        }
        if (depthM <= safeDeep) {
            const keel::f32 t = (depthM - safeContour) / keel::max(0.1f, safeDeep - safeContour);
            return LerpColor(guikit::Color(11, 48, 80, 255), guikit::Color(8, 28, 48, 255), t);
        }
        return guikit::Color(6, 19, 34, 255);
    }

    if (cfg.chartPalette == ChartPalette::MarineDusk) {
        if (depthM <= 0.02f) return guikit::Color(72, 132, 108, 255);
        if (depthM <= safeShallow) return guikit::Color(36, 118, 178, 255);
        if (depthM <= safeContour) {
            const keel::f32 t = (depthM - safeShallow) / keel::max(0.1f, safeContour - safeShallow);
            return LerpColor(guikit::Color(44, 128, 188, 255), guikit::Color(32, 92, 142, 255), t);
        }
        if (depthM <= safeDeep) {
            const keel::f32 t = (depthM - safeContour) / keel::max(0.1f, safeDeep - safeContour);
            return LerpColor(guikit::Color(32, 92, 142, 255), guikit::Color(22, 58, 92, 255), t);
        }
        return guikit::Color(17, 42, 68, 255);
    }

    // Default: MarineDay S-52 high-contrast hydrographic chart palette
    if (depthM <= 0.02f) {
        return guikit::Color(166, 217, 184, 255); // Intertidal green-teal
    }
    if (depthM <= safeShallow) {
        const keel::f32 t = keel::clamp(depthM / safeShallow, 0.0f, 1.0f);
        return LerpColor(guikit::Color(66, 174, 242, 255), guikit::Color(94, 190, 248, 255), t);
    }
    if (depthM <= safeContour) {
        const keel::f32 t = (depthM - safeShallow) / keel::max(0.1f, safeContour - safeShallow);
        return LerpColor(guikit::Color(118, 202, 250, 255), guikit::Color(154, 219, 252, 255), t);
    }
    const keel::f32 midDeep = safeContour + (safeDeep - safeContour) * 0.45f;
    if (depthM <= midDeep) {
        const keel::f32 t = (depthM - safeContour) / keel::max(0.1f, midDeep - safeContour);
        return LerpColor(guikit::Color(182, 228, 253, 255), guikit::Color(214, 240, 254, 255), t);
    }
    if (depthM <= safeDeep) {
        const keel::f32 t = (depthM - midDeep) / keel::max(0.1f, safeDeep - midDeep);
        return LerpColor(guikit::Color(218, 241, 254, 255), guikit::Color(239, 249, 255, 255), t);
    }
    const keel::f32 tDeep = keel::clamp((depthM - safeDeep) / 15.0f, 0.0f, 1.0f);
    return LerpColor(guikit::Color(242, 250, 255, 255), guikit::Color(250, 253, 255, 255), tDeep);
}

[[nodiscard]] guikit::Color ScaleColorAlpha(guikit::Color c, keel::f32 alphaScale) noexcept {
    const keel::f32 a = keel::clamp((keel::as<keel::f32>(c.a) / 255.0f) * alphaScale, 0.0f, 1.0f);
    return WithAlpha(c, a);
}

[[nodiscard]] ResolvedFeatureStyle ResolveMarineFeatureStyle(
    const geo::GeoLayer& layer,
    const geo::GeoFeature& feat,
    const GisRenderConfig& cfg,
    bool bakeFillOpacity
) noexcept {
    const geo::LayerStyle& ls = layer.style;
    const keel::f32 strokeScale = keel::clamp(ls.strokeWidthPx / 2.0f, 0.15f, 5.0f);
    const keel::f32 fillAlphaScale = bakeFillOpacity ? keel::clamp(ls.fillOpacity, 0.0f, 1.0f) : 1.0f;
    const bool customCol = ls.useCustomColor;

    // Classic GIS mode or unclassified user features use the layer's custom style
    if (cfg.chartPalette == ChartPalette::ClassicGis ||
        feat.chartClass == geo::ChartFeatureClass::Generic) {
        return {
            .fillColor = WithAlpha(ls.primaryColor, 0.48f * fillAlphaScale),
            .strokeColor = WithAlpha(ls.primaryColor, ls.strokeOpacity),
            .strokeWidthPx = ls.strokeWidthPx,
            .renderFill = ls.renderFill && feat.HasPolygons(),
            .renderStroke = ls.renderStroke
        };
    }

    const bool isNight = (cfg.chartPalette == ChartPalette::MarineNight);
    const bool isDusk  = (cfg.chartPalette == ChartPalette::MarineDusk);

    switch (feat.chartClass) {
        case geo::ChartFeatureClass::CoverageMask: {
            const guikit::Color covCol = customCol
                ? WithAlpha(ls.primaryColor, 0.75f)
                : (isNight ? guikit::Color(120, 80, 40, 140) : guikit::Color(217, 119, 6, 150));
            return {
                .fillColor = guikit::Color::Transparent(),
                .strokeColor = covCol,
                .strokeWidthPx = 1.0f * strokeScale,
                .renderFill = false,
                .renderStroke = cfg.showCoverageBounds && ls.renderStroke
            };
        }

        case geo::ChartFeatureClass::DepthArea: {
            const keel::f32 dMin = feat.minDepthMeters;
            const keel::f32 dMax = feat.maxDepthMeters;
            keel::f32 dSample = 1.5f;
            if (!keel::is_nan(dMin) && !keel::is_nan(dMax)) {
                dSample = dMin * 0.7f + dMax * 0.3f;
            } else if (!keel::is_nan(dMin)) {
                dSample = (dMin > 0.02f || dMin < -0.02f) ? dMin : 1.5f;
            } else if (!keel::is_nan(dMax)) {
                dSample = dMax * 0.6f;
            }
            guikit::Color bathyCol = SampleBathymetryColor(dSample, cfg);
            if (customCol) {
                bathyCol = LerpColor(bathyCol, ls.primaryColor, 0.58f);
            }
            const bool explicitOutline = !ls.renderFill || (keel::abs(ls.strokeWidthPx - 2.0f) > 0.08f);
            return {
                .fillColor = ScaleColorAlpha(bathyCol, fillAlphaScale),
                .strokeColor = BrightenColor(bathyCol, isNight ? 22 : -28),
                .strokeWidthPx = 0.65f * strokeScale,
                .renderFill = ls.renderFill,
                .renderStroke = explicitOutline && ls.renderStroke
            };
        }

        case geo::ChartFeatureClass::LandArea: {
            const bool isVeg = (feat.featureCode == "VEGATN");
            const bool hasBasemapOverlay = (cfg.showOsmBasemap || cfg.customBasemapProvider != nullptr);
            if (hasBasemapOverlay && cfg.hideLandOverBasemap && !isVeg) {
                const guikit::Color landStroke = customCol
                    ? BrightenColor(ls.primaryColor, isNight ? 28 : -38)
                    : (isNight
                        ? guikit::Color(88, 74, 54, 210)
                        : (isDusk ? guikit::Color(74, 60, 42, 220) : guikit::Color(139, 110, 62, 210)));
                return {
                    .fillColor = guikit::Color::Transparent(),
                    .strokeColor = landStroke,
                    .strokeWidthPx = 1.05f * strokeScale,
                    .renderFill = false,
                    .renderStroke = ls.renderStroke
                };
            }
            guikit::Color landFill = isVeg
                ? (isNight ? guikit::Color(28, 42, 26, 220) : (isDusk ? guikit::Color(86, 112, 74, 225) : guikit::Color(200, 224, 172, 235)))
                : (isNight ? guikit::Color(36, 31, 24, 255) : (isDusk ? guikit::Color(118, 102, 76, 255) : guikit::Color(234, 218, 179, 255)));
            if (customCol) {
                landFill = LerpColor(landFill, ls.primaryColor, 0.85f);
            }
            const guikit::Color landStroke = customCol
                ? BrightenColor(ls.primaryColor, isNight ? 28 : -38)
                : (isNight
                    ? guikit::Color(88, 74, 54, 235)
                    : (isDusk ? guikit::Color(74, 60, 42, 240) : guikit::Color(139, 110, 62, 240)));
            const bool explicitOutline = !ls.renderFill || (keel::abs(ls.strokeWidthPx - 2.0f) > 0.08f);
            return {
                .fillColor = ScaleColorAlpha(landFill, fillAlphaScale),
                .strokeColor = landStroke,
                .strokeWidthPx = 1.15f * strokeScale,
                .renderFill = ls.renderFill,
                .renderStroke = explicitOutline && ls.renderStroke
            };
        }

        case geo::ChartFeatureClass::BuiltUpOrStructure: {
            guikit::Color structFill = isNight
                ? guikit::Color(58, 48, 36, 130)
                : (isDusk ? guikit::Color(138, 118, 88, 130) : guikit::Color(206, 182, 138, 120));
            guikit::Color structStroke = isNight
                ? guikit::Color(130, 110, 82, 220)
                : guikit::Color(92, 73, 46, 215);
            if (customCol) {
                structFill = WithAlpha(ls.primaryColor, 225.0f / 255.0f);
                structStroke = BrightenColor(ls.primaryColor, -35);
            }
            return {
                .fillColor = ScaleColorAlpha(structFill, fillAlphaScale),
                .strokeColor = structStroke,
                .strokeWidthPx = 1.25f * strokeScale,
                .renderFill = feat.HasPolygons() && ls.renderFill,
                .renderStroke = ls.renderStroke
            };
        }

        case geo::ChartFeatureClass::FairwayArea: {
            guikit::Color fwFill = isNight
                ? guikit::Color(48, 28, 68, 75)
                : guikit::Color(168, 85, 247, 45);
            guikit::Color fwStroke = isNight
                ? guikit::Color(192, 132, 252, 185)
                : guikit::Color(147, 51, 234, 175);
            if (customCol) {
                fwFill = WithAlpha(ls.primaryColor, 0.32f);
                fwStroke = WithAlpha(ls.primaryColor, 0.85f);
            }
            return {
                .fillColor = ScaleColorAlpha(fwFill, fillAlphaScale),
                .strokeColor = fwStroke,
                .strokeWidthPx = 1.1f * strokeScale,
                .renderFill = feat.HasPolygons() && ls.renderFill,
                .renderStroke = ls.renderStroke
            };
        }

        case geo::ChartFeatureClass::SeabedOrMarineArea: {
            guikit::Color marFill = isNight
                ? guikit::Color(18, 52, 82, 65)
                : guikit::Color(56, 189, 248, 42);
            guikit::Color marStroke = isNight
                ? guikit::Color(56, 189, 248, 140)
                : guikit::Color(2, 132, 199, 140);
            if (customCol) {
                marFill = WithAlpha(ls.primaryColor, 0.30f);
                marStroke = WithAlpha(ls.primaryColor, 0.75f);
            }
            return {
                .fillColor = ScaleColorAlpha(marFill, fillAlphaScale),
                .strokeColor = marStroke,
                .strokeWidthPx = 0.9f * strokeScale,
                .renderFill = feat.HasPolygons() && ls.renderFill,
                .renderStroke = ls.renderStroke
            };
        }

        case geo::ChartFeatureClass::DepthContour: {
            const keel::f32 d = feat.depthMeters;
            const bool isSafety = !keel::is_nan(d) && (d <= cfg.safetyContourM + 0.08f);
            const bool isMajor  = !keel::is_nan(d) && (keel::abs(keel::fmod(d, 5.0f)) < 0.12f);

            guikit::Color cntCol;
            if (customCol) {
                const keel::f32 alpha = isSafety ? 0.96f : (isMajor ? 0.86f : 0.74f);
                cntCol = WithAlpha(isSafety ? BrightenColor(ls.primaryColor, -18) : ls.primaryColor, alpha);
            } else if (isNight) {
                cntCol = isSafety
                    ? guikit::Color(56, 189, 248, 225)
                    : (isMajor ? guikit::Color(38, 132, 185, 195) : guikit::Color(28, 96, 142, 165));
            } else if (isDusk) {
                cntCol = isSafety
                    ? guikit::Color(125, 211, 252, 230)
                    : (isMajor ? guikit::Color(72, 162, 212, 200) : guikit::Color(54, 128, 178, 175));
            } else {
                cntCol = isSafety
                    ? guikit::Color(24, 92, 145, 235)
                    : (isMajor ? guikit::Color(52, 116, 168, 210) : guikit::Color(88, 146, 192, 185));
            }
            const keel::f32 widthPx = (isSafety ? 1.35f : (isMajor ? 1.05f : 0.78f)) * strokeScale;
            return {
                .fillColor = guikit::Color::Transparent(),
                .strokeColor = cntCol,
                .strokeWidthPx = widthPx,
                .renderFill = false,
                .renderStroke = ls.renderStroke && cfg.showDepthContours
            };
        }

        case geo::ChartFeatureClass::Coastline: {
            const guikit::Color coastCol = customCol
                ? WithAlpha(ls.primaryColor, 0.96f)
                : (isNight
                    ? guikit::Color(168, 142, 102, 245)
                    : (isDusk ? guikit::Color(214, 190, 145, 245) : guikit::Color(62, 47, 28, 250)));
            return {
                .fillColor = guikit::Color::Transparent(),
                .strokeColor = coastCol,
                .strokeWidthPx = 1.65f * strokeScale,
                .renderFill = false,
                .renderStroke = ls.renderStroke
            };
        }

        case geo::ChartFeatureClass::SubmarineCablePipe:
        case geo::ChartFeatureClass::NavigationLine: {
            const guikit::Color navCol = customCol
                ? WithAlpha(ls.primaryColor, 0.90f)
                : (isNight
                    ? guikit::Color(216, 110, 232, 215)
                    : guikit::Color(162, 28, 175, 215));
            return {
                .fillColor = guikit::Color::Transparent(),
                .strokeColor = navCol,
                .strokeWidthPx = 1.35f * strokeScale,
                .renderFill = false,
                .renderStroke = ls.renderStroke
            };
        }

        case geo::ChartFeatureClass::UnderwaterHazard: {
            guikit::Color hazFill = isNight
                ? guikit::Color(14, 116, 144, 115)
                : guikit::Color(56, 189, 248, 95);
            guikit::Color hazStroke = isNight
                ? guikit::Color(248, 113, 113, 210)
                : guikit::Color(30, 58, 138, 210);
            if (customCol) {
                hazFill = WithAlpha(ls.primaryColor, 0.42f);
                hazStroke = WithAlpha(ls.primaryColor, 0.90f);
            }
            return {
                .fillColor = ScaleColorAlpha(hazFill, fillAlphaScale),
                .strokeColor = hazStroke,
                .strokeWidthPx = 1.05f * strokeScale,
                .renderFill = feat.HasPolygons() && ls.renderFill,
                .renderStroke = ls.renderStroke
            };
        }

        default:
            return {
                .fillColor = WithAlpha(ls.primaryColor, 0.45f * fillAlphaScale),
                .strokeColor = WithAlpha(ls.primaryColor, ls.strokeOpacity),
                .strokeWidthPx = ls.strokeWidthPx,
                .renderFill = feat.HasPolygons() && ls.renderFill,
                .renderStroke = ls.renderStroke
            };
    }
}

[[nodiscard]] keel::i32 CartographicPainterRank(const geo::GeoFeature& feat) noexcept {
    // Standard IHO S-52 painter order: base water (DepthArea) -> LandArea -> marine/fairway areas -> structures/hazards -> contours -> coastline
    switch (feat.chartClass) {
        case geo::ChartFeatureClass::DepthArea:           return 10;
        case geo::ChartFeatureClass::LandArea:            return 15;
        case geo::ChartFeatureClass::SeabedOrMarineArea:  return 20;
        case geo::ChartFeatureClass::FairwayArea:         return 25;
        case geo::ChartFeatureClass::UnderwaterHazard:    return 30;
        case geo::ChartFeatureClass::BuiltUpOrStructure:  return 35;
        case geo::ChartFeatureClass::Generic:
            return feat.HasPolygons() ? 40 : (feat.HasLines() ? 75 : 95);
        case geo::ChartFeatureClass::DepthContour:        return 50;
        case geo::ChartFeatureClass::SubmarineCablePipe:  return 60;
        case geo::ChartFeatureClass::NavigationLine:      return 65;
        case geo::ChartFeatureClass::Coastline:           return 70;
        case geo::ChartFeatureClass::CoverageMask:        return 80;
        default:                                          return 90;
    }
}

[[nodiscard]] keel::i32 CartographicLayerRank(const geo::GeoLayer& layer) noexcept {
    switch (layer.dominantChartClass) {
        case geo::ChartFeatureClass::DepthArea:           return 10;
        case geo::ChartFeatureClass::LandArea:            return 15;
        case geo::ChartFeatureClass::SeabedOrMarineArea:  return 20;
        case geo::ChartFeatureClass::FairwayArea:         return 25;
        case geo::ChartFeatureClass::UnderwaterHazard:    return 30;
        case geo::ChartFeatureClass::BuiltUpOrStructure:  return 35;
        case geo::ChartFeatureClass::DepthContour:        return 50;
        case geo::ChartFeatureClass::SubmarineCablePipe:  return 60;
        case geo::ChartFeatureClass::NavigationLine:      return 65;
        case geo::ChartFeatureClass::Coastline:           return 70;
        case geo::ChartFeatureClass::CoverageMask:        return 80;
        default:                                          break;
    }
    keel::i32 minRank = 90;
    const keel::usize sampleLimit = keel::min<keel::usize>(layer.features.size(), 32);
    for (keel::usize i = 0; i < sampleLimit; ++i) {
        const geo::GeoFeature& f = layer.features[i];
        if (!f.HasPolygons() && !f.HasLines()) continue;
        minRank = keel::min(minRank, CartographicPainterRank(f));
    }
    return minRank;
}

[[nodiscard]] CellNorm3857Bounds ComputeCellNorm3857Bounds(
    const guikit::OsmTileCell& cell,
    guikit::MapCrs crs
) noexcept {
    if (crs == guikit::MapCrs::EPSG3857_WebMercator) {
        const keel::f64 invTileN = 1.0 / keel::as<keel::f64>(1 << keel::clamp(cell.id.z, 0, 22));
        const keel::f32 u0 = keel::as<keel::f32>(keel::as<keel::f64>(cell.id.x) * invTileN);
        const keel::f32 u1 = keel::as<keel::f32>(keel::as<keel::f64>(cell.id.x + 1) * invTileN);
        const keel::f32 v0 = keel::as<keel::f32>(keel::as<keel::f64>(cell.id.y) * invTileN);
        const keel::f32 v1 = keel::as<keel::f32>(keel::as<keel::f64>(cell.id.y + 1) * invTileN);
        return {u0, u1, v0, v1, keel::max(1e-9f, u1 - u0), keel::max(1e-9f, v1 - v0)};
    }
    const guikit::Vec2 nw3857 = guikit::MapGrid::GeoToNormalized(cell.geoBounds.nw, guikit::MapCrs::EPSG3857_WebMercator);
    const guikit::Vec2 se3857 = guikit::MapGrid::GeoToNormalized(cell.geoBounds.se, guikit::MapCrs::EPSG3857_WebMercator);
    const keel::f32 u0 = keel::min(nw3857.x, se3857.x);
    const keel::f32 u1 = keel::max(nw3857.x, se3857.x);
    const keel::f32 v0 = keel::min(nw3857.y, se3857.y);
    const keel::f32 v1 = keel::max(nw3857.y, se3857.y);
    return {u0, u1, v0, v1, keel::max(1e-9f, u1 - u0), keel::max(1e-9f, v1 - v0)};
}

// Projects a pre-computed EPSG:3857 normalized 2D point into tile pixel space [0..tileSizeF] for any active CRS.
// - For EPSG:3857 (and cylindrical CRS at z >= 8), uses the ultra-fast 2-multiply affine mapping.
// - For cylindrical CRS (EPSG:4326, EPSG:3395) at overview zooms z < 8, applies exact non-linear Y reprojection.
// - For non-cylindrical CRS (EPSG:32636 UTM, EPSG:20906 GSK-2011, EPSG:28406 Pulkovo-42, EPSG:3995 Polar),
//   applies exact 64-bit double-precision 2D reprojection so adjacent tiles match seamlessly at all zooms.
[[nodiscard]] inline guikit::Vec2 ProjectNorm3857PtToTilePx(
    guikit::Vec2 normPt3857,
    keel::f32 u0,
    keel::f32 v0,
    keel::f32 invSpanU,
    keel::f32 invSpanV,
    const guikit::OsmTileCell& cell,
    guikit::MapCrs crs,
    keel::f32 tileSizeF
) noexcept {
    if (crs == guikit::MapCrs::EPSG3857_WebMercator ||
        (guikit::MapGrid::IsCylindricalCrs(crs) && cell.id.z >= 8)) {
        return {
            (normPt3857.x - u0) * invSpanU,
            (normPt3857.y - v0) * invSpanV
        };
    }

    const keel::f64 tileN = keel::as<keel::f64>(1 << keel::clamp(cell.id.z, 0, 22));
    const keel::f64 n = keel::kPi * (1.0 - 2.0 * keel::as<keel::f64>(normPt3857.y));
    const keel::f64 latDeg = keel::rad_to_deg(keel::atan(keel::sinh(n)));

    if (guikit::MapGrid::IsCylindricalCrs(crs)) {
        const auto [uCrs, vCrs] = guikit::MapGrid::GeoToNormalizedDouble({latDeg, 0.0}, crs);
        (void)uCrs;
        return {
            (normPt3857.x - u0) * invSpanU,
            keel::as<keel::f32>((vCrs * tileN - keel::as<keel::f64>(cell.id.y)) * keel::as<keel::f64>(tileSizeF))
        };
    }

    const keel::f64 lonDeg = keel::as<keel::f64>(normPt3857.x) * 360.0 - 180.0;
    const auto [uCrs, vCrs] = guikit::MapGrid::GeoToNormalizedDouble({latDeg, lonDeg}, crs);
    return {
        keel::as<keel::f32>((uCrs * tileN - keel::as<keel::f64>(cell.id.x)) * keel::as<keel::f64>(tileSizeF)),
        keel::as<keel::f32>((vCrs * tileN - keel::as<keel::f64>(cell.id.y)) * keel::as<keel::f64>(tileSizeF))
    };
}

void ClipRingToTileGuardBand(
    keel::ArrayRef<guikit::Vec2> normRing,
    keel::f32 u0,
    keel::f32 v0,
    keel::f32 invSpanU,
    keel::f32 invSpanV,
    const guikit::OsmTileCell& cell,
    guikit::MapCrs crs,
    keel::f32 tileSizeF,
    keel::f32 minDecimateSqPx,
    keel::Array<guikit::Vec2>& outClippedPx,
    keel::Array<guikit::Vec2>& scratchA,
    keel::Array<guikit::Vec2>& scratchB
) {
    outClippedPx.clear();
    if (normRing.size() < 3) return;

    scratchA.clear();
    scratchA.reserve(normRing.size());

    // Project from normalized EPSG:3857 [0..1] to tile pixel space [0..tileSizeF] in active CRS with sub-pixel decimation
    guikit::Vec2 prevPx{-1e9f, -1e9f};
    keel::f32 minX =  1e30f;
    keel::f32 maxX = -1e30f;
    keel::f32 minY =  1e30f;
    keel::f32 maxY = -1e30f;
    for (keel::usize i = 0; i < normRing.size(); ++i) {
        const guikit::Vec2 ptPx = ProjectNorm3857PtToTilePx(
            normRing[i], u0, v0, invSpanU, invSpanV, cell, crs, tileSizeF
        );
        const keel::f32 dx = ptPx.x - prevPx.x;
        const keel::f32 dy = ptPx.y - prevPx.y;
        if (i == 0 || i + 1 == normRing.size() || (dx * dx + dy * dy >= minDecimateSqPx)) {
            scratchA.push_back(ptPx);
            prevPx = ptPx;
            if (ptPx.x < minX) minX = ptPx.x;
            if (ptPx.x > maxX) maxX = ptPx.x;
            if (ptPx.y < minY) minY = ptPx.y;
            if (ptPx.y > maxY) maxY = ptPx.y;
        }
    }

    if (scratchA.size() < 3) return;

    const keel::f32 kMinBound = -kTileGuardBandPx;
    const keel::f32 kMaxBound = tileSizeF + kTileGuardBandPx;

    // Fast AABB Trivial Reject: all projected vertices lie strictly outside one of the 4 guard-band half-planes
    if (maxX < kMinBound || minX > kMaxBound || maxY < kMinBound || minY > kMaxBound) {
        return;
    }

    // Fast AABB Trivial Accept: all projected vertices lie completely inside the tile guard-band
    if (minX >= kMinBound && maxX <= kMaxBound && minY >= kMinBound && maxY <= kMaxBound) {
        keel::swap(outClippedPx, scratchA);
        return;
    }

    // Sutherland-Hodgman 4-plane axis-aligned box clip
    // Plane 1: x >= kMinBound
    scratchB.clear();
    {
        guikit::Vec2 s = scratchA.back();
        for (const guikit::Vec2& e : scratchA) {
            const bool eIn = (e.x >= kMinBound);
            const bool sIn = (s.x >= kMinBound);
            if (eIn) {
                if (!sIn) {
                    const keel::f32 t = (kMinBound - s.x) / (e.x - s.x);
                    scratchB.push_back({kMinBound, s.y + t * (e.y - s.y)});
                }
                scratchB.push_back(e);
            } else if (sIn) {
                const keel::f32 t = (kMinBound - s.x) / (e.x - s.x);
                scratchB.push_back({kMinBound, s.y + t * (e.y - s.y)});
            }
            s = e;
        }
    }
    if (scratchB.size() < 3) return;

    // Plane 2: x <= kMaxBound
    scratchA.clear();
    {
        guikit::Vec2 s = scratchB.back();
        for (const guikit::Vec2& e : scratchB) {
            const bool eIn = (e.x <= kMaxBound);
            const bool sIn = (s.x <= kMaxBound);
            if (eIn) {
                if (!sIn) {
                    const keel::f32 t = (kMaxBound - s.x) / (e.x - s.x);
                    scratchA.push_back({kMaxBound, s.y + t * (e.y - s.y)});
                }
                scratchA.push_back(e);
            } else if (sIn) {
                const keel::f32 t = (kMaxBound - s.x) / (e.x - s.x);
                scratchA.push_back({kMaxBound, s.y + t * (e.y - s.y)});
            }
            s = e;
        }
    }
    if (scratchA.size() < 3) return;

    // Plane 3: y >= kMinBound
    scratchB.clear();
    {
        guikit::Vec2 s = scratchA.back();
        for (const guikit::Vec2& e : scratchA) {
            const bool eIn = (e.y >= kMinBound);
            const bool sIn = (s.y >= kMinBound);
            if (eIn) {
                if (!sIn) {
                    const keel::f32 t = (kMinBound - s.y) / (e.y - s.y);
                    scratchB.push_back({s.x + t * (e.x - s.x), kMinBound});
                }
                scratchB.push_back(e);
            } else if (sIn) {
                const keel::f32 t = (kMinBound - s.y) / (e.y - s.y);
                scratchB.push_back({s.x + t * (e.x - s.x), kMinBound});
            }
            s = e;
        }
    }
    if (scratchB.size() < 3) return;

    // Plane 4: y <= kMaxBound
    outClippedPx.reserve(scratchB.size() + 4);
    {
        guikit::Vec2 s = scratchB.back();
        for (const guikit::Vec2& e : scratchB) {
            const bool eIn = (e.y <= kMaxBound);
            const bool sIn = (s.y <= kMaxBound);
            if (eIn) {
                if (!sIn) {
                    const keel::f32 t = (kMaxBound - s.y) / (e.y - s.y);
                    outClippedPx.push_back({s.x + t * (e.x - s.x), kMaxBound});
                }
                outClippedPx.push_back(e);
            } else if (sIn) {
                const keel::f32 t = (kMaxBound - s.y) / (e.y - s.y);
                outClippedPx.push_back({s.x + t * (e.x - s.x), kMaxBound});
            }
            s = e;
        }
    }
    if (outClippedPx.size() < 3) {
        outClippedPx.clear();
    }
}

// Cohen-Sutherland 2D segment clipper against [-kTileGuardBandPx, tileSizeF + kTileGuardBandPx]
[[nodiscard]] bool ClipSegmentToTile(guikit::Vec2& p0, guikit::Vec2& p1, keel::f32 tileSizeF) noexcept {
    const keel::f32 kMin = -kTileGuardBandPx;
    const keel::f32 kMax = tileSizeF + kTileGuardBandPx;

    auto outCode = [kMin, kMax](keel::f32 x, keel::f32 y) noexcept -> keel::i32 {
        keel::i32 c = 0;
        if (x < kMin) c |= 1;
        else if (x > kMax) c |= 2;
        if (y < kMin) c |= 4;
        else if (y > kMax) c |= 8;
        return c;
    };

    keel::i32 c0 = outCode(p0.x, p0.y);
    keel::i32 c1 = outCode(p1.x, p1.y);

    for (keel::i32 iter = 0; iter < 8; ++iter) {
        if ((c0 | c1) == 0) return true;
        if ((c0 & c1) != 0) return false;

        const keel::i32 cOut = c0 ? c0 : c1;
        keel::f32 x = 0.0f;
        keel::f32 y = 0.0f;
        const keel::f32 dx = p1.x - p0.x;
        const keel::f32 dy = p1.y - p0.y;

        if (cOut & 8) {
            x = p0.x + dx * (kMax - p0.y) / dy;
            y = kMax;
        } else if (cOut & 4) {
            x = p0.x + dx * (kMin - p0.y) / dy;
            y = kMin;
        } else if (cOut & 2) {
            y = p0.y + dy * (kMax - p0.x) / dx;
            x = kMax;
        } else {
            y = p0.y + dy * (kMin - p0.x) / dx;
            x = kMin;
        }

        if (cOut == c0) {
            p0 = {x, y};
            c0 = outCode(p0.x, p0.y);
        } else {
            p1 = {x, y};
            c1 = outCode(p1.x, p1.y);
        }
    }
    return false;
}

keel::usize AppendPolylineToVectorPath(
    guikit::VectorPath& path,
    keel::ArrayRef<guikit::Vec2> normLine,
    keel::f32 u0,
    keel::f32 v0,
    keel::f32 invSpanU,
    keel::f32 invSpanV,
    const guikit::OsmTileCell& cell,
    guikit::MapCrs crs,
    keel::f32 tileSizeF,
    keel::f32 minDecimateSqPx,
    bool closed
) {
    if (normLine.size() < 2) return 0;

    keel::usize emittedSegments = 0;
    bool penDown = false;
    guikit::Vec2 lastDrawn{-1e9f, -1e9f};
    guikit::Vec2 prevPx = ProjectNorm3857PtToTilePx(
        normLine[0], u0, v0, invSpanU, invSpanV, cell, crs, tileSizeF
    );
    const guikit::Vec2 firstPx = prevPx;

    auto emitSegment = [&](guikit::Vec2 a, guikit::Vec2 b) {
        if (!ClipSegmentToTile(a, b, tileSizeF)) {
            penDown = false;
            return;
        }
        const keel::f32 cdx = b.x - a.x;
        const keel::f32 cdy = b.y - a.y;
        if (cdx * cdx + cdy * cdy < 0.01f) {
            return;
        }
        if (!penDown || keel::abs(a.x - lastDrawn.x) > 0.25f || keel::abs(a.y - lastDrawn.y) > 0.25f) {
            path.MoveTo(a);
        }
        path.LineTo(b);
        lastDrawn = b;
        penDown = true;
        ++emittedSegments;
    };

    for (keel::usize i = 1; i < normLine.size(); ++i) {
        const guikit::Vec2 currPx = ProjectNorm3857PtToTilePx(
            normLine[i], u0, v0, invSpanU, invSpanV, cell, crs, tileSizeF
        );
        const keel::f32 dx = currPx.x - prevPx.x;
        const keel::f32 dy = currPx.y - prevPx.y;
        const bool isLast = (i + 1 == normLine.size());
        if (!isLast && (dx * dx + dy * dy < minDecimateSqPx)) {
            continue;
        }
        emitSegment(prevPx, currPx);
        prevPx = currPx;
    }

    if (closed && normLine.size() >= 3) {
        const keel::f32 dx = firstPx.x - prevPx.x;
        const keel::f32 dy = firstPx.y - prevPx.y;
        if (dx * dx + dy * dy > 0.25f) {
            emitSegment(prevPx, firstPx);
        }
    }
    return emittedSegments;
}

void BlendPremulTileOntoOpaqueComposite(
    keel::u32* __restrict dst,
    const keel::u32* __restrict src,
    keel::u8 layerOpacity,
    keel::u32 tileSizePx
) noexcept {
    keel::usize kNumPx = 0;
    if (layerOpacity == 0 || !keel::checked_mul<keel::usize>(tileSizePx, tileSizePx, kNumPx)) return;

    if (layerOpacity == 255) {
        for (keel::usize i = 0; i < kNumPx; ++i) {
            const keel::u32 s = src[i];
            if (s == 0u) continue;
            const keel::u32 sa = s >> 24;
            if (sa == 255u) {
                dst[i] = s;
            } else {
                const keel::u32 d = dst[i];
                const keel::u32 invA = 255u - sa;
                const keel::u32 sr = s & 0xFFu;
                const keel::u32 sg = (s >> 8) & 0xFFu;
                const keel::u32 sb = (s >> 16) & 0xFFu;
                const keel::u32 dr = keel::min(255u, sr + (((d & 0xFFu) * invA + 127u) >> 8));
                const keel::u32 dg = keel::min(255u, sg + ((((d >> 8) & 0xFFu) * invA + 127u) >> 8));
                const keel::u32 db = keel::min(255u, sb + ((((d >> 16) & 0xFFu) * invA + 127u) >> 8));
                dst[i] = 0xFF000000u | (db << 16) | (dg << 8) | dr;
            }
        }
    } else {
        const keel::u32 op = layerOpacity;
        for (keel::usize i = 0; i < kNumPx; ++i) {
            const keel::u32 s = src[i];
            if (s == 0u) continue;
            const keel::u32 sa = ((s >> 24) * op + 127u) >> 8;
            if (sa == 0u) continue;
            const keel::u32 sr = ((s & 0xFFu) * op + 127u) >> 8;
            const keel::u32 sg = (((s >> 8) & 0xFFu) * op + 127u) >> 8;
            const keel::u32 sb = (((s >> 16) & 0xFFu) * op + 127u) >> 8;
            const keel::u32 d = dst[i];
            const keel::u32 invA = 255u - sa;
            const keel::u32 dr = keel::min(255u, sr + (((d & 0xFFu) * invA + 127u) >> 8));
            const keel::u32 dg = keel::min(255u, sg + ((((d >> 8) & 0xFFu) * invA + 127u) >> 8));
            const keel::u32 db = keel::min(255u, sb + ((((d >> 16) & 0xFFu) * invA + 127u) >> 8));
            dst[i] = 0xFF000000u | (db << 16) | (dg << 8) | dr;
        }
    }
}

void BlendStraightRgbaBasemapOntoComposite(
    keel::u32* __restrict dst,
    const keel::u32* __restrict src,
    keel::u8 basemapOpacity,
    keel::u32 tileSizePx
) noexcept {
    keel::usize kNumPx = 0;
    if (basemapOpacity == 0 || !keel::checked_mul<keel::usize>(tileSizePx, tileSizePx, kNumPx)) return;

    if (basemapOpacity == 255) {
        for (keel::usize i = 0; i < kNumPx; ++i) {
            const keel::u32 s = src[i];
            const keel::u32 sa = s >> 24;
            if (sa == 0u) continue;
            if (sa == 255u) {
                dst[i] = s;
            } else {
                const keel::u32 d = dst[i];
                const keel::u32 invA = 255u - sa;
                const keel::u32 sr = s & 0xFFu;
                const keel::u32 sg = (s >> 8) & 0xFFu;
                const keel::u32 sb = (s >> 16) & 0xFFu;
                const keel::u32 dr = ((sr * sa + (d & 0xFFu) * invA) + 127u) >> 8;
                const keel::u32 dg = ((sg * sa + ((d >> 8) & 0xFFu) * invA) + 127u) >> 8;
                const keel::u32 db = ((sb * sa + ((d >> 16) & 0xFFu) * invA) + 127u) >> 8;
                dst[i] = 0xFF000000u | (db << 16) | (dg << 8) | dr;
            }
        }
    } else {
        const keel::u32 op = basemapOpacity;
        for (keel::usize i = 0; i < kNumPx; ++i) {
            const keel::u32 s = src[i];
            const keel::u32 sa = ((s >> 24) * op + 127u) >> 8;
            if (sa == 0u) continue;
            const keel::u32 d = dst[i];
            const keel::u32 invA = 255u - sa;
            const keel::u32 sr = s & 0xFFu;
            const keel::u32 sg = (s >> 8) & 0xFFu;
            const keel::u32 sb = (s >> 16) & 0xFFu;
            const keel::u32 dr = ((sr * sa + (d & 0xFFu) * invA) + 127u) >> 8;
            const keel::u32 dg = ((sg * sa + ((d >> 8) & 0xFFu) * invA) + 127u) >> 8;
            const keel::u32 db = ((sb * sa + ((d >> 16) & 0xFFu) * invA) + 127u) >> 8;
            dst[i] = 0xFF000000u | (db << 16) | (dg << 8) | dr;
        }
    }
}

void RasterizeCandidatesToPixels(
    guikit::ICanvas& canvas,
    guikit::VectorPath& fillPath,
    guikit::VectorPath& strokePath,
    keel::Array<guikit::Vec2>& clippedRing,
    keel::Array<guikit::Vec2>& scratchA,
    keel::Array<guikit::Vec2>& scratchB,
    keel::ArrayRef<AsyncTileCandidate> candidates,
    const guikit::OsmTileCell& cell,
    guikit::MapCrs crs,
    keel::u32 tileSizePx,
    const guikit::Rect& queryBox,
    bool lodSimplification,
    keel::f32 lodPixelTolerance,
    keel::Array<keel::u32>& outPixels,
    bool& outHasAnyGeometry
) {
    const keel::f32 tileSizeF = keel::as<keel::f32>(tileSizePx);
    const CellNorm3857Bounds cb = ComputeCellNorm3857Bounds(cell, crs);
    const keel::f32 u0 = cb.u0;
    const keel::f32 v0 = cb.v0;
    const keel::f32 invSpanU = tileSizeF / cb.spanU;
    const keel::f32 invSpanV = tileSizeF / cb.spanV;

    outHasAnyGeometry = false;
    keel::usize numTilePx = 0;
    if (!keel::checked_mul<keel::usize>(tileSizePx, tileSizePx, numTilePx)) {
        GetTilePixelPool().Recycle(outPixels);
        return;
    }
    auto ensureCanvasTargeted = [&]() {
        if (!outHasAnyGeometry) {
            GetTilePixelPool().AcquireFilled(outPixels, numTilePx, 0u);
            canvas.SetTarget(outPixels.data(), tileSizePx, tileSizePx, tileSizePx);
            outHasAnyGeometry = true;
        }
    };

    const keel::f32 zoomDecimateBoost = (cell.id.z <= 10) ? 0.90f : ((cell.id.z == 11) ? 0.65f : 0.45f);
    const keel::f32 minDecimateSqPx = lodSimplification
        ? (lodPixelTolerance * lodPixelTolerance * zoomDecimateBoost)
        : 0.16f;

    strokePath.Clear();
    guikit::PathPaint activeStrokePaint{};
    keel::u32 activeStrokeRgba = 0;
    keel::u32 activeStrokeWidthBits = 0;
    bool activeStrokeDashed = false;
    keel::usize activeStrokeSegments = 0;

    auto flushActiveStroke = [&]() {
        if (activeStrokeSegments > 0 && !strokePath.Empty()) {
            ensureCanvasTargeted();
            canvas.DrawPath(strokePath, activeStrokePaint);
            strokePath.Clear();
            activeStrokeSegments = 0;
        }
    };

    for (const AsyncTileCandidate& cand : candidates) {
        if (!cand.payload) continue;
        const geo::DecodedGeometryPayload& geom = *cand.payload;

        const bool useLodRings = lodSimplification && (cell.id.z <= 13 || cand.maxDimPx < 96.0f);
        const bool skipTinyHoles = (lodSimplification && (cell.id.z <= 11 && cand.maxDimPx < 14.0f)) || (cand.maxDimPx < 4.0f);
        const ResolvedFeatureStyle& st = cand.style;

        // 1. Polygon Fill (each GeoPolygon gets its own DrawPath call so overlapping polygons never XOR-cancel)
        if (!geom.polygons.empty() && st.renderFill && st.fillColor.a > 0) {
            flushActiveStroke();

            const bool sealAdjacentSeam = !st.renderStroke && (st.fillColor.a == 255) &&
                (cand.maxDimPx >= 4.0f) &&
                (cand.chartClass == geo::ChartFeatureClass::DepthArea ||
                 cand.chartClass == geo::ChartFeatureClass::LandArea);

            for (const geo::GeoPolygon& poly : geom.polygons) {
                if (poly.hasValidBounds && !poly.normBounds.Intersects(queryBox)) {
                    continue;
                }
                const auto& extRing = (useLodRings && poly.normExteriorLod.size() >= 3)
                    ? poly.normExteriorLod
                    : poly.normExterior;
                ClipRingToTileGuardBand(
                    extRing, u0, v0, invSpanU, invSpanV, cell, crs, tileSizeF, minDecimateSqPx,
                    clippedRing, scratchA, scratchB
                );
                if (clippedRing.size() < 3) continue;

                fillPath.Clear();
                fillPath.AddClosedRing(clippedRing);

                bool hasValidHoles = false;
                if (!skipTinyHoles) {
                    for (const auto& holeRing : poly.normInterior) {
                        ClipRingToTileGuardBand(
                            holeRing, u0, v0, invSpanU, invSpanV, cell, crs, tileSizeF, minDecimateSqPx,
                            clippedRing, scratchA, scratchB
                        );
                        if (clippedRing.size() < 3) continue;
                        fillPath.AddClosedRing(clippedRing);
                        hasValidHoles = true;
                    }
                }

                const guikit::PathPaint fillPaint{
                    .fillColor = st.fillColor,
                    .strokeColor = st.fillColor,
                    .strokeWidthPx = sealAdjacentSeam ? 0.65f : 0.0f,
                    .fillRule = hasValidHoles ? guikit::FillRule::EvenOdd : guikit::FillRule::NonZero,
                    .renderFill = true,
                    .renderStroke = sealAdjacentSeam
                };

                ensureCanvasTargeted();
                canvas.DrawPath(fillPath, fillPaint);
            }
        }

        // 2. Polygon Outline & LineStrings (Style-Bucket Compound Path Batching)
        if (st.renderStroke && st.strokeColor.a > 0 && st.strokeWidthPx > 0.0f) {
            const keel::u32 strokeRgba = st.strokeColor.ToUint32();
            const keel::u32 strokeWidthBits = keel::bit_cast<keel::u32>(st.strokeWidthPx);
            const bool isDashed = (cand.chartClass == geo::ChartFeatureClass::NavigationLine ||
                                   cand.chartClass == geo::ChartFeatureClass::FairwayArea ||
                                   cand.chartClass == geo::ChartFeatureClass::CoverageMask);

            if (activeStrokeSegments > 0 &&
                (activeStrokeRgba != strokeRgba ||
                 activeStrokeWidthBits != strokeWidthBits ||
                 activeStrokeDashed != isDashed ||
                 activeStrokeSegments >= 256)) {
                flushActiveStroke();
            }

            if (activeStrokeSegments == 0) {
                activeStrokePaint = guikit::PathPaint{
                    .strokeColor = st.strokeColor,
                    .strokeWidthPx = st.strokeWidthPx,
                    .renderFill = false,
                    .renderStroke = true,
                    .dashed = isDashed,
                    .dashOnPx = 5.0f,
                    .dashOffPx = 4.0f
                };
                activeStrokeRgba = strokeRgba;
                activeStrokeWidthBits = strokeWidthBits;
                activeStrokeDashed = isDashed;
            }

            keel::usize addedStrokeSegs = 0;
            for (const geo::GeoPolygon& poly : geom.polygons) {
                if (poly.hasValidBounds && !poly.normBounds.Intersects(queryBox)) {
                    continue;
                }
                const auto& extRing = (useLodRings && poly.normExteriorLod.size() >= 3)
                    ? poly.normExteriorLod
                    : poly.normExterior;
                addedStrokeSegs += AppendPolylineToVectorPath(
                    strokePath, extRing, u0, v0, invSpanU, invSpanV, cell, crs, tileSizeF, minDecimateSqPx, /*closed=*/true);
                if (!skipTinyHoles) {
                    for (const auto& holeRing : poly.normInterior) {
                        addedStrokeSegs += AppendPolylineToVectorPath(
                            strokePath, holeRing, u0, v0, invSpanU, invSpanV, cell, crs, tileSizeF, minDecimateSqPx, /*closed=*/true);
                    }
                }
            }

            for (keel::usize li = 0; li < geom.normLines.size(); ++li) {
                const auto& line = (useLodRings && li < geom.normLinesLod.size() && geom.normLinesLod[li].size() >= 2)
                    ? geom.normLinesLod[li]
                    : geom.normLines[li];
                addedStrokeSegs += AppendPolylineToVectorPath(
                    strokePath, line, u0, v0, invSpanU, invSpanV, cell, crs, tileSizeF, minDecimateSqPx, /*closed=*/false);
            }

            activeStrokeSegments += addedStrokeSegs;
        }
    }

    flushActiveStroke();

    if (outHasAnyGeometry) {
        canvas.End();
    } else {
        GetTilePixelPool().Recycle(outPixels);
    }
}

void DrawLoadingTilePlaceholder(
    guikit::SceneScope& scene,
    const guikit::OsmTileCell& cell,
    keel::f32 zoom,
    keel::f32 invZ,
    bool darkChartSurface,
    bool hasPreview,
    keel::u64 frameCounter
) {
    const guikit::Rect& r = cell.worldRect;
    const keel::f32 cellScreenW = r.w * zoom;
    const keel::f32 cellScreenH = r.h * zoom;
    const keel::f32 minSidePx = keel::min(cellScreenW, cellScreenH);
    if (minSidePx < 24.0f) {
        return;
    }

    const keel::f32 x0 = r.x;
    const keel::f32 y0 = r.y;
    const keel::f32 x1 = r.Right();
    const keel::f32 y1 = r.Bottom();
    const keel::f32 cx = x0 + r.w * 0.5f;
    const keel::f32 cy = y0 + r.h * 0.5f;

    const bool evenChecker = ((cell.id.x + cell.id.y) & 1) == 0;
    const guikit::Color accentCol = darkChartSurface
        ? guikit::Color(56, 189, 248, hasPreview ? 175 : 225)
        : guikit::Color(2, 132, 199, hasPreview ? 170 : 220);
    const guikit::Color gridLineCol = darkChartSurface
        ? guikit::Color(56, 189, 248, hasPreview ? 28 : 48)
        : guikit::Color(14, 116, 144, hasPreview ? 24 : 42);

    // 1. If no parent/child mipmap preview exists yet, draw a structured nautical blueprint grid cell
    if (!hasPreview) {
        const guikit::Color cellTint = darkChartSurface
            ? (evenChecker ? guikit::Color(13, 28, 46, 220) : guikit::Color(18, 36, 58, 220))
            : (evenChecker ? guikit::Color(214, 230, 244, 225) : guikit::Color(204, 223, 240, 225));
        scene.DrawRect(r, cellTint, 0.0f, gridLineCol, 1.0f * invZ);

        // 2x2 sub-tile quadtree crosshair lines + diagonal survey lines
        scene.DrawLine({x0, cy}, {x1, cy}, gridLineCol, 1.0f * invZ);
        scene.DrawLine({cx, y0}, {cx, y1}, gridLineCol, 1.0f * invZ);
        if (minSidePx >= 64.0f) {
            const guikit::Color diagCol = WithAlpha(gridLineCol, darkChartSurface ? 0.12f : 0.10f);
            scene.DrawLine({x0, y0}, {x1, y1}, diagCol, 0.9f * invZ);
            scene.DrawLine({x1, y0}, {x0, y1}, diagCol, 0.9f * invZ);
        }
    }

    // 2. Crisp L-shaped corner brackets indicating an active background tile job
    const keel::f32 insetW = keel::min(5.0f * invZ, r.w * 0.12f);
    const keel::f32 armLenW = keel::clamp(minSidePx * 0.12f, 6.0f, 16.0f) * invZ;
    const keel::f32 bracketStrokeW = (hasPreview ? 1.35f : 1.65f) * invZ;
    const keel::f32 bx0 = x0 + insetW;
    const keel::f32 by0 = y0 + insetW;
    const keel::f32 bx1 = x1 - insetW;
    const keel::f32 by1 = y1 - insetW;

    // Top-left bracket
    scene.DrawLine({bx0, by0}, {bx0 + armLenW, by0}, accentCol, bracketStrokeW);
    scene.DrawLine({bx0, by0}, {bx0, by0 + armLenW}, accentCol, bracketStrokeW);
    // Top-right bracket
    scene.DrawLine({bx1, by0}, {bx1 - armLenW, by0}, accentCol, bracketStrokeW);
    scene.DrawLine({bx1, by0}, {bx1, by0 + armLenW}, accentCol, bracketStrokeW);
    // Bottom-left bracket
    scene.DrawLine({bx0, by1}, {bx0 + armLenW, by1}, accentCol, bracketStrokeW);
    scene.DrawLine({bx0, by1}, {bx0, by1 - armLenW}, accentCol, bracketStrokeW);
    // Bottom-right bracket
    scene.DrawLine({bx1, by1}, {bx1 - armLenW, by1}, accentCol, bracketStrokeW);
    scene.DrawLine({bx1, by1}, {bx1, by1 - armLenW}, accentCol, bracketStrokeW);

    // 3. Animated radar/sonar loading spinner + [z/x/y] tile status badge
    const keel::u64 nowMs = keel::monotonic_ms();
    const keel::u32 cellPhase = keel::as<keel::u32>((cell.id.x * 37 + cell.id.y * 17) & 63);
    const keel::f32 spinAngleDeg = keel::fmod(
        keel::as<keel::f32>(nowMs % 3600ULL) * 0.30f +
        keel::as<keel::f32>(frameCounter % 180ULL) * 4.0f +
        keel::as<keel::f32>(cellPhase) * 5.5f,
        360.0f
    );

    const keel::f32 spinnerRadiusPx = hasPreview
        ? keel::clamp(minSidePx * 0.075f, 6.5f, 11.5f)
        : keel::clamp(minSidePx * 0.11f, 8.5f, 15.5f);
    const keel::f32 spinnerRadiusW = spinnerRadiusPx * invZ;

    // When a mipmap preview is already visible, anchor a compact badge near the top-right corner
    // so the preview chart in the center remains completely unobstructed; otherwise center it.
    const guikit::Vec2 spinnerCenter = hasPreview
        ? guikit::Vec2{bx1 - (spinnerRadiusPx + 8.0f) * invZ, by0 + (spinnerRadiusPx + 8.0f) * invZ}
        : guikit::Vec2{cx, cy - (minSidePx >= 88.0f ? 8.0f * invZ : 0.0f)};

    const guikit::Color badgeBg = darkChartSurface
        ? guikit::Color(9, 20, 34, hasPreview ? 185 : 215)
        : guikit::Color(248, 250, 252, hasPreview ? 195 : 225);
    const guikit::Color trackCol = darkChartSurface
        ? guikit::Color(56, 189, 248, 52)
        : guikit::Color(2, 132, 199, 48);

    scene.DrawCircle(spinnerCenter, (spinnerRadiusPx + 3.5f) * invZ, badgeBg, trackCol, 1.1f * invZ);
    scene.DrawArc(spinnerCenter, spinnerRadiusW, spinAngleDeg, 115.0f, accentCol, 2.0f * invZ, 12);
    scene.DrawArc(
        spinnerCenter,
        spinnerRadiusW * 0.60f,
        spinAngleDeg + 180.0f,
        80.0f,
        WithAlpha(accentCol, 0.65f),
        1.4f * invZ,
        10
    );
    scene.DrawCircle(spinnerCenter, 1.8f * invZ, accentCol);

    if (!hasPreview && minSidePx >= 84.0f) {
        char labelBuf[48]{};
        const keel::StringRef labelSv = keel::format_buf(
            labelBuf, "LOADING [%d/%d/%d]", cell.id.z, cell.id.x, cell.id.y
        );
        const keel::f32 pillW = keel::min(r.w * 0.86f, 118.0f * invZ);
        const keel::f32 pillH = 16.0f * invZ;
        const guikit::Rect pillRect(
            cx - pillW * 0.5f,
            spinnerCenter.y + (spinnerRadiusPx + 6.5f) * invZ,
            pillW,
            pillH
        );
        scene.DrawRect(pillRect, badgeBg, 4.0f * invZ, trackCol, 1.0f * invZ);
        const guikit::Color textCol = darkChartSurface
            ? guikit::Color(186, 230, 253, 235)
            : guikit::Color(3, 105, 161, 235);
        scene.DrawTextBox(
            labelSv,
            pillRect,
            9.5f,
            textCol,
            0.5f,
            0.5f,
            guikit::FontStyle::Bold,
            /*scaleWithZoom=*/false
        );
    }
}

[[nodiscard]] keel::u64 ComputeLayerStyleHash(
    const geo::GeoLayer& lyr,
    const GisRenderConfig& cfg,
    guikit::MapCrs crs
) noexcept {
    keel::u64 h = keel::kFnv1aOffsetBasis;
    const keel::u32 resolvedTileSz = ResolveTileSizePx(cfg.tileSizePx);
    const bool hasBasemapOverlay = (cfg.showOsmBasemap || cfg.customBasemapProvider != nullptr);
    const bool suppressLandFill = hasBasemapOverlay && cfg.hideLandOverBasemap;
    HashCombineVal(h, crs);
    HashCombineVal(h, resolvedTileSz);
    HashCombineVal(h, cfg.chartPalette);
    HashCombineVal(h, cfg.lodSimplification);
    HashCombineVal(h, cfg.lodPixelTolerance);
    HashCombineVal(h, cfg.scaleFiltering);
    HashCombineVal(h, cfg.showDepthContours);
    HashCombineVal(h, cfg.showCoverageBounds);
    HashCombineVal(h, cfg.showCautionAndRestrictedZones);
    HashCombineVal(h, cfg.safetyContourM);
    HashCombineVal(h, cfg.shallowContourM);
    HashCombineVal(h, cfg.deepContourM);
    HashCombineVal(h, suppressLandFill);

    HashCombineVal(h, lyr.uid);
    const keel::usize fCount = lyr.isWindowStreamed
        ? keel::max(lyr.estimatedTotalFeatures, keel::as<keel::usize>(1u))
        : lyr.features.size();
    HashCombineVal(h, fCount);
    HashCombineVal(h, lyr.style.useCustomColor);
    HashCombineVal(h, lyr.style.primaryColor.r);
    HashCombineVal(h, lyr.style.primaryColor.g);
    HashCombineVal(h, lyr.style.primaryColor.b);
    HashCombineVal(h, lyr.style.strokeOpacity);
    HashCombineVal(h, lyr.style.strokeWidthPx);
    HashCombineVal(h, lyr.style.renderFill);
    HashCombineVal(h, lyr.style.renderStroke);
    return h;
}

[[nodiscard]] guikit::Color ResolveIalaBuoyColor(keel::StringRef lightOrColor, guikit::Color fallback) noexcept {
    if (lightOrColor.empty()) return fallback;
    // Check standard IHO S-57 COLOUR codes, letter abbreviations, and light strings
    if (lightOrColor == "R" || lightOrColor == "3" || lightOrColor == "RW" || lightOrColor == "WR" ||
        lightOrColor == "3,1" || lightOrColor == "1,3" ||
        lightOrColor.contains("Red") ||
        lightOrColor.contains("red")) {
        return guikit::Color(239, 68, 68, 255); // IALA Red (Port / Safe Water)
    }
    if (lightOrColor == "G" || lightOrColor == "4" ||
        lightOrColor == "4,1" || lightOrColor == "1,4" ||
        lightOrColor.contains("Green") ||
        lightOrColor.contains("green")) {
        return guikit::Color(16, 185, 129, 255); // IALA Green (Starboard)
    }
    if (lightOrColor == "Y" || lightOrColor == "6" ||
        lightOrColor == "BY" || lightOrColor == "YB" ||
        lightOrColor == "YBY" || lightOrColor == "BYB" ||
        lightOrColor == "6,2" || lightOrColor == "2,6" ||
        lightOrColor == "6,2,6" || lightOrColor == "2,6,2" ||
        lightOrColor.contains("Yellow") ||
        lightOrColor.contains("yellow")) {
        return guikit::Color(245, 158, 11, 255); // IALA Yellow / Cardinal (Black-Yellow)
    }
    if (lightOrColor == "W" || lightOrColor == "1" ||
        lightOrColor.contains("White") ||
        lightOrColor.contains("white")) {
        return guikit::Color(56, 189, 248, 255); // Lighted Mark / Sector
    }
    return fallback;
}

} // namespace detail

Color GisRenderEngine::GetWaterBackgroundColor(ChartPalette palette) noexcept {
    switch (palette) {
        case ChartPalette::MarineNight:
            return Color(6, 15, 28, 255);
        case ChartPalette::MarineDusk:
            return Color(17, 38, 60, 255);
        case ChartPalette::ClassicGis:
            return Color(226, 238, 248, 255);
        case ChartPalette::MarineDay:
        default:
            return Color(226, 238, 248, 255);
    }
}

void GisRenderEngine::SetDefaultBasemapProvider(keel::Arc<IRasterTileProvider> provider) {
    keel::LockGuard<keel::Mutex> lk(detail::GetDefaultBasemapMutex());
    detail::GetDefaultBasemapStorage() = keel::move(provider);
}

keel::Arc<IRasterTileProvider> GisRenderEngine::GetDefaultBasemapProvider() {
    keel::LockGuard<keel::Mutex> lk(detail::GetDefaultBasemapMutex());
    return detail::GetDefaultBasemapStorage();
}

void GisRenderEngine::SetBreadcrumbCallback(BreadcrumbCallback cb) noexcept {
    detail::g_breadcrumbCallback.store(cb, keel::MemoryOrder::Relaxed);
}

} // namespace guikit::geo
