#include "guikit/guikit.hpp"
#include "guikit/geo.hpp"
#include "keel/http.h"
#if defined(GUIKIT_HAS_BACKEND_THORVG) && GUIKIT_HAS_BACKEND_THORVG
#include "guikit/sdl.hpp"
#endif
#include "keel/keel.h"
#include "keel/math.h"

#if defined(GUIKIT_HAS_BACKEND_THORVG) && GUIKIT_HAS_BACKEND_THORVG
using TestCanvas = guikit::ThorCanvas;
#else
using TestCanvas = guikit::NullCanvas;
#endif

static keel::i32 g_failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            keel::eprintln("[FAIL] {} ({}:{})", #cond, __FILE__, __LINE__);          \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                        \
    do {                                                                             \
        if (keel::abs((a) - (b)) > (eps)) {                                           \
            keel::eprintln("[FAIL] {} ({}) != {} ({}) [{}:{}]",                      \
                           #a, (a), #b, (b), __FILE__, __LINE__);                    \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

using namespace guikit;

void TestMapGridAndProjections() {
    using namespace guikit;

    // 1. Round-trip Geographic <-> Normalized (u, v) across all 3 CRS projections
    const GeoCoord helgoland{54.1833, 7.8833};
    for (MapCrs crs : {MapCrs::EPSG3857_WebMercator,
                       MapCrs::EPSG3395_WorldMercator,
                       MapCrs::EPSG4326_PlateCarree}) {
        const Vec2 uv = MapGrid::GeoToNormalized(helgoland, crs);
        CHECK(uv.x > 0.5f && uv.x < 0.6f);
        CHECK(uv.y > 0.1f && uv.y < 0.45f);
        const GeoCoord back = MapGrid::NormalizedToGeo(uv, crs);
        CHECK(keel::abs(back.lat - helgoland.lat) < 1e-3);
        CHECK(keel::abs(back.lon - helgoland.lon) < 1e-3);
    }

    // 2. Noticeable difference between Spherical (EPSG:3857) and WGS84 Ellipsoidal (EPSG:3395) Mercator Y
    const auto [u3857, v3857] = MapGrid::GeoToNormalized(helgoland, MapCrs::EPSG3857_WebMercator);
    const auto [u3395, v3395] = MapGrid::GeoToNormalized(helgoland, MapCrs::EPSG3395_WorldMercator);
    CHECK(keel::abs(u3857 - u3395) < 1e-9);
    CHECK(keel::abs(v3857 - v3395) > 5e-4);

    const GeoPlanar p3857 = MapGrid::GeoToNormalizedDouble(helgoland, MapCrs::EPSG3857_WebMercator);
    const GeoPlanar p3395 = MapGrid::GeoToNormalizedDouble(helgoland, MapCrs::EPSG3395_WorldMercator);
    CHECK(keel::abs(p3857.first - p3395.first) < 1e-12);
    CHECK(keel::abs(p3857.second - p3395.second) > 5e-4);

    // 3. Tile ID & View State calculation
    MapGridConfig cfg{
        .crs = MapCrs::EPSG3857_WebMercator,
        .worldSize = 2048.0f,
        .baseZoom = 3,
        .minTileZoom = 1,
        .maxTileZoom = 18
    };
    const Vec2 helgolandWorld = MapGrid::GeoToWorld(helgoland, cfg.worldSize, cfg.crs);
    const keel::f32 sceneZoomZ7 = MapGrid::TileZoomToSceneZoom(7.0f, cfg.baseZoom);
    CHECK(keel::abs(sceneZoomZ7 - 16.0f) < 1e-4f);

    const Rect visibleWorld{
        helgolandWorld.x - 20.0f,
        helgolandWorld.y - 15.0f,
        40.0f,
        30.0f
    };
    const Rect viewportRect{0.0f, 0.0f, 640.0f, 480.0f};
    const MapViewState view = MapGrid::ComputeView(
        visibleWorld, viewportRect, sceneZoomZ7, helgolandWorld, cfg
    );
    CHECK(view.tileZoom == 7);
    CHECK(!view.visibleTiles.empty());
    CHECK(!view.meridians.empty());
    CHECK(!view.parallels.empty());
    CHECK(keel::abs(view.cursor.lat - helgoland.lat) < 1e-2);
    CHECK(keel::abs(view.cursor.lon - helgoland.lon) < 1e-2);
    CHECK(view.metricScale.screenPixels > 20.0f);
    CHECK(view.nauticalScale.screenPixels > 20.0f);

    // 4. Nautical DMM formatting check
    const auto dmmStr = MapGrid::FormatCoord(helgoland, GeoCoordFormat::NauticalDMM);
    CHECK(dmmStr.as_ref().find("54") != keel::StringRef::npos);
    CHECK(dmmStr.as_ref().find("007") != keel::StringRef::npos);
}

void TestMapViewWidgetWorkflow() {
    using namespace guikit;

    Context ui;
    TestCanvas canvas;

    const keel::u32 width = 960;
    const keel::u32 height = 540;
    keel::Array<keel::u32> pixels(width * height);
    canvas.SetTarget(pixels.data(), width, height, width);

    MapCrs crs = MapCrs::EPSG3857_WebMercator;
    keel::Optional<GeoCoord> reqCenter{};
    keel::Optional<keel::f32> reqZoom{};
    MapViewState lastView{};
    keel::usize itemCount = 0;

    auto renderFrame = [&]() {
        ui.BeginFrame({keel::as<keel::f32>(width), keel::as<keel::f32>(height)});
        if (auto map = guikit::geo::MapView(ui, {
            .crs = crs,
            .coordFormat = GeoCoordFormat::NauticalDMM,
            .initialCenter = {54.1825, 7.8853},
            .initialTileZoom = 7.0f,
            .requestCenterGeo = reqCenter,
            .requestTileZoom = reqZoom,
            .height = SizeMode::Stretch
        })) {
            map.DrawGeoLine({53.8900, 9.1433}, {54.1825, 7.8853}, Color(232, 121, 249, 220), 1.8f);
            map.DrawSeamark({54.1825, 7.8853}, {
                .id = "WP-HELGOLAND",
                .title = "Helgoland Light",
                .category = "IALA Lighthouse"
            });
            lastView = map.GetViewState();
            itemCount = map.GetItems().size();
        }
        reqCenter.reset();
        reqZoom.reset();
        ui.EndFrame(canvas);
    };

    // Frame 1 & 2: Layout and camera initialization
    renderFrame();
    renderFrame();

    CHECK(lastView.tileZoom == 7);
    CHECK(!lastView.visibleTiles.empty());
    CHECK(!lastView.meridians.empty());
    CHECK(!lastView.parallels.empty());
    CHECK(itemCount >= 1);
    CHECK_NEAR(keel::as<keel::f32>(lastView.center.lat), 54.1825f, 0.25f);
    CHECK_NEAR(keel::as<keel::f32>(lastView.center.lon), 7.8853f, 0.25f);

    // Switch CRS to EPSG:3395 and verify geographic center is preserved
    crs = MapCrs::EPSG3395_WorldMercator;
    renderFrame();
    CHECK(lastView.crs == MapCrs::EPSG3395_WorldMercator);
    CHECK_NEAR(keel::as<keel::f32>(lastView.center.lat), 54.1825f, 0.25f);
    CHECK_NEAR(keel::as<keel::f32>(lastView.center.lon), 7.8853f, 0.25f);

    // Programmatic camera jump to Strait of Gibraltar (z=8)
    reqCenter = GeoCoord{36.1095, -5.3453};
    reqZoom = 8.0f;
    renderFrame();
    CHECK(lastView.tileZoom == 8);
    CHECK_NEAR(keel::as<keel::f32>(lastView.center.lat), 36.1095f, 0.25f);
    CHECK_NEAR(keel::as<keel::f32>(lastView.center.lon), -5.3453f, 0.25f);

    // Tab-switching & 600-frame GC regression:
    // Simulate a parent stage container where another tab is active for 605 frames (pruning SceneState),
    // then switch to the MapView tab with dirty-rect tracking enabled and verify full tile grid on Frame 1.
    ui.SetDirtyRectEnabled(true);
    bool showMapTab = false;
    MapViewState tabSwitchView{};
    auto renderTabbedApp = [&]() {
        ui.BeginFrame({keel::as<keel::f32>(width), keel::as<keel::f32>(height)});
        if (auto stage = ui.Box({.width = SizeMode::Stretch, .height = SizeMode::Stretch})) {
            if (!showMapTab) {
                ui.Text("Other Tab Content");
            } else {
                if (auto map = guikit::geo::MapView(ui, {
                    .crs = MapCrs::EPSG3857_WebMercator,
                    .coordFormat = GeoCoordFormat::NauticalDMM,
                    .initialCenter = {54.1825, 7.8853},
                    .initialTileZoom = 7.0f,
                    .height = SizeMode::Stretch
                })) {
                    tabSwitchView = map.GetViewState();
                }
            }
        }
        ui.EndFrame(canvas);
    };

    for (keel::i32 i = 0; i < 605; ++i) {
        renderTabbedApp();
    }

    // Activate MapView tab: very first frame should inherit parent stage bounds and render full z=7 tile grid
    showMapTab = true;
    renderTabbedApp();
    CHECK(ui.HasDirtyDamage());
    CHECK(tabSwitchView.tileZoom == 7);
    CHECK(tabSwitchView.visibleTiles.size() >= 6);
    CHECK(!tabSwitchView.meridians.empty());
    CHECK(!tabSwitchView.parallels.empty());
    CHECK_NEAR(keel::as<keel::f32>(tabSwitchView.center.lat), 54.1825f, 0.25f);
    CHECK_NEAR(keel::as<keel::f32>(tabSwitchView.center.lon), 7.8853f, 0.25f);

    // Verify embedded GIS chart loading (assets/geo/demo_bay.geojson) and GisRenderEngine rasterization
    const auto chart = GeoJsonIO::LoadFromFile("assets/geo/demo_bay.geojson", true);
    CHECK(chart.ok);
    CHECK(!chart.layers.empty());
    CHECK(chart.totalFeatures >= 400);
    CHECK(chart.totalBounds.IsValid());

    GisRenderStats gisStats{};
    keel::u64 selUid = 0;
    GisRenderConfig gisCfg{};
    gisCfg.asyncBackgroundStreaming = false;
    ui.BeginFrame({keel::as<keel::f32>(width), keel::as<keel::f32>(height)});
    if (auto map = guikit::geo::MapView(ui, {
        .crs = MapCrs::EPSG3857_WebMercator,
        .coordFormat = GeoCoordFormat::NauticalDMM,
        .initialCenter = chart.totalBounds.Center(),
        .initialTileZoom = 13.0f,
        .requestResetView = true,
        .height = SizeMode::Stretch
    })) {
        gisStats = GisRenderEngine::RenderLayers(
            map,
            chart.layers,
            gisCfg,
            selUid,
            true
        );
    }
    ui.EndFrame(canvas);
    CHECK(gisStats.visibleLayers == chart.layers.size());
    CHECK(gisStats.totalFeatures == chart.totalFeatures);
    CHECK(gisStats.renderedFeatures > 0 || gisStats.cachedTilesRendered > 0 || gisStats.cachedTilesHit > 0);

    keel::println("  [PASS] TestMapViewWidgetWorkflow completed");
}

void TestGisDemoBayGeoJsonAndRenderEngine() {
    using namespace guikit;

    auto res = GeoJsonIO::LoadFromFile("assets/geo/demo_bay.geojson", /*splitByThematicClass=*/true);
    CHECK(res.ok);
    CHECK(res.layers.size() >= 5);
    CHECK(res.totalFeatures > 100);
    CHECK(res.totalVertices > 5000);
    CHECK(res.totalBounds.IsValid());

    Context ui;
    TestCanvas canvas;
    const keel::u32 width = 800;
    const keel::u32 height = 600;
    keel::Array<keel::u32> pixels(width * height);
    canvas.SetTarget(pixels.data(), width, height, width);

    GisRenderConfig settings{};
    settings.asyncBackgroundStreaming = false;
    GisRenderStats stats{};
    keel::u64 selectedUid = 0;

    for (keel::i32 f = 0; f < 2; ++f) {
        ui.BeginFrame({keel::as<keel::f32>(width), keel::as<keel::f32>(height)});
        if (auto map = guikit::geo::MapView(ui, {
            .crs = MapCrs::EPSG3857_WebMercator,
            .initialCenter = res.totalBounds.Center(),
            .initialTileZoom = 13.4f,
            .height = SizeMode::Stretch
        })) {
            stats = GisRenderEngine::RenderLayers(
                map,
                res.layers,
                settings,
                selectedUid,
                true
            );
        }
        ui.EndFrame(canvas);
    }

    CHECK(stats.totalFeatures == res.totalFeatures);
    CHECK(stats.renderedFeatures > 0 || stats.cachedTilesHit > 0 || stats.cachedTilesRendered > 0);
    CHECK(stats.renderedVertices > 0);
}

void TestPanelSashAndBodyOcclusionOverMapViewAndScene() {
    Context ui;
    TestCanvas canvas;
    keel::Array<keel::u32> pixels(800 * 600);
    canvas.SetTarget(pixels.data(), 800, 600, 800);

    bool showBottom = true;
    SidebarMode rightMode = SidebarMode::Expanded;
    Rect mapVp{};
    Rect bottomRect{};
    Rect rightSidebarRect{};
    Rect childBtnRect{};
    bool mapRawClicked = false;
    bool mapScopeClicked = false;
    bool mapScopeHovered = false;
    bool mapPanning = false;
    bool childBtnClicked = false;
    keel::f32 mapZoom = 1.0f;
    Vec2 isobathCenterScreen{};
    keel::u64 selectedGisUid = 0;
    GisRenderStats gisStats{};
    GisRenderConfig gisCfg{};
    gisCfg.asyncBackgroundStreaming = false;
    auto isobathLoad = GeoJsonIO::ParseThematicLayersFromString(R"({
        "type": "FeatureCollection",
        "features": [{
            "type": "Feature",
            "id": "DEPCNT-50M",
            "properties": { "OBJL": "DEPCNT", "VALDCO": 50.0, "name": "Isobath 50m" },
            "geometry": {
                "type": "LineString",
                "coordinates": [[131.8735, 43.25], [131.8735, 43.1056], [131.8735, 42.90]]
            }
        }]
    })", "Bathymetry", true);
    CHECK(isobathLoad.ok && !isobathLoad.layers.empty());

    auto renderBellhopLayout = [&]() {
        mapRawClicked = false;
        mapScopeClicked = false;
        mapScopeHovered = false;
        mapPanning = false;
        childBtnClicked = false;

        ui.BeginFrame({800.0f, 600.0f});
        if (auto root = ui.Row({ .gap = 0.0f, .padding = Padding::All(0.0f), .height = SizeMode::Stretch })) {
            if (auto leftSb = ui.Sidebar({
                .side = SidebarSide::Left,
                .width = 180.0f,
                .resizable = true,
                .padding = Padding::All(8.0f)
            })) {
                ui.Heading({ .label = "Acoustic Source" });
            }

            if (auto centerCol = ui.Column({
                .gap = 0.0f,
                .padding = Padding::All(0.0f),
                .width = SizeMode::Stretch,
                .height = SizeMode::Stretch
            })) {
                // MapView is evaluated BEFORE BottomPanel and Right Sidebar in the frame!
                if (auto map = guikit::geo::MapView(ui, {
                    .initialCenter = { 43.1056, 131.8735 },
                    .initialTileZoom = 11.0f,
                    .height = SizeMode::Stretch
                })) {
                    mapVp = map.GetScene().GetViewportRect();
                    mapScopeHovered = map.IsHovered();
                    mapScopeClicked = map.WasClicked();
                    mapPanning = map.GetScene().IsPanning();
                    mapZoom = map.GetScene().GetZoom();
                    isobathCenterScreen = map.GeoToScreen({ 43.1056, 131.8735 });
                    if (ui.IsMouseClicked()) {
                        mapRawClicked = true;
                    }
                    gisStats = GisRenderEngine::RenderLayers(
                        map,
                        isobathLoad.layers,
                        gisCfg,
                        selectedGisUid,
                        false
                    );

                    // Floating HUD card inside MapView
                    if (auto hudCard = ui.Card({ .padding = Padding::All(8.0f) })) {
                        auto btn = ui.Button({ .text = "Drop Buoy" });
                        childBtnRect = btn.rect;
                        childBtnClicked = btn.clicked;
                    }
                }

                if (auto bp = ui.BottomPanel(showBottom, {
                    .height = 160.0f,
                    .minHeight = 90.0f,
                    .maxHeight = 360.0f,
                    .resizable = true,
                    .allowCollapse = true,
                    .showToggleButton = true,
                    .padding = Padding::All(8.0f)
                })) {
                    bottomRect = bp.response.rect;
                    ui.Button({ .text = "Bellhop Profile Action" });
                }
            }

            if (auto rightSb = ui.Sidebar(rightMode, {
                .side = SidebarSide::Right,
                .width = 180.0f,
                .minWidth = 120.0f,
                .maxWidth = 340.0f,
                .resizable = true,
                .padding = Padding::All(8.0f)
            })) {
                rightSidebarRect = rightSb.response.rect;
                ui.Heading({ .label = "Inspector" });
            }
        }
        ui.EndFrame(canvas);
    };

    renderBellhopLayout();
    renderBellhopLayout();
    CHECK(mapVp.w > 300.0f && mapVp.h > 300.0f);
    CHECK(keel::abs(bottomRect.y - 440.0f) < 1.5f);
    CHECK(keel::abs(bottomRect.h - 160.0f) < 1.5f);
    CHECK(keel::abs(rightSidebarRect.x - 620.0f) < 1.5f);
    CHECK(keel::abs(rightSidebarRect.w - 180.0f) < 1.5f);

    // 1. Hover & Click/Drag on BottomPanel top splitter sash protruding 4px upward into MapView (y = 436.0f < 440.0f)
    // MapView must NOT see hover or click or pan, and BottomPanel must smoothly resize from 160px to 240px!
    ui.OnMouseMove({ 300.0f, 436.0f });
    renderBellhopLayout();
    CHECK(mapScopeHovered == false);

    ui.OnMouseDown(1);
    renderBellhopLayout();
    CHECK(mapRawClicked == false);
    CHECK(mapScopeClicked == false);
    CHECK(mapPanning == false);

    // Drag upward by 80px (y = 436 -> 356) across MapView
    ui.OnMouseMove({ 300.0f, 356.0f });
    renderBellhopLayout();
    CHECK(mapRawClicked == false);
    CHECK(mapScopeClicked == false);
    CHECK(mapPanning == false);

    ui.OnMouseUp(1);
    renderBellhopLayout();
    renderBellhopLayout();
    CHECK(keel::abs(bottomRect.h - 240.0f) < 2.0f);

    // 2. Click inside BottomPanel body, Left Sidebar body, and Right Sidebar body -> MapView never reacts!
    ui.OnMouseMove({ 300.0f, 500.0f }); // Inside BottomPanel
    ui.OnMouseDown(1);
    renderBellhopLayout();
    CHECK(mapRawClicked == false);
    CHECK(mapScopeClicked == false);
    CHECK(mapScopeHovered == false);
    ui.OnMouseUp(1);
    renderBellhopLayout();

    ui.OnMouseMove({ 80.0f, 200.0f }); // Inside Left Sidebar
    ui.OnMouseDown(1);
    renderBellhopLayout();
    CHECK(mapRawClicked == false);
    CHECK(mapScopeClicked == false);
    CHECK(mapScopeHovered == false);
    ui.OnMouseUp(1);
    renderBellhopLayout();

    ui.OnMouseMove({ 700.0f, 200.0f }); // Inside Right Sidebar
    ui.OnMouseDown(1);
    renderBellhopLayout();
    CHECK(mapRawClicked == false);
    CHECK(mapScopeClicked == false);
    CHECK(mapScopeHovered == false);
    ui.OnMouseUp(1);
    renderBellhopLayout();

    // 3. Click on Right Sidebar splitter sash protruding 4px leftward into MapView (x = 616.0f < 620.0f)
    // MapView must NOT see click or pan, and Right Sidebar must resize from 180px to 240px!
    ui.OnMouseMove({ 616.0f, 200.0f });
    ui.OnMouseDown(1);
    renderBellhopLayout();
    CHECK(mapRawClicked == false);
    CHECK(mapScopeClicked == false);
    CHECK(mapPanning == false);

    ui.OnMouseMove({ 556.0f, 200.0f });
    renderBellhopLayout();
    CHECK(mapPanning == false);

    ui.OnMouseUp(1);
    renderBellhopLayout();
    renderBellhopLayout();
    CHECK(keel::abs(rightSidebarRect.w - 240.0f) < 2.0f);

    // 4. Click on child Button inside Card inside MapView -> Button clicks, MapView canvas does NOT click!
    CHECK(childBtnRect.w > 20.0f && childBtnRect.h > 15.0f);
    ui.OnMouseMove(childBtnRect.Center());
    ui.OnMouseDown(1);
    renderBellhopLayout();
    CHECK(mapRawClicked == false);
    CHECK(mapScopeClicked == false);
    CHECK(mapScopeHovered == false);
    ui.OnMouseUp(1);
    renderBellhopLayout();
    CHECK(childBtnClicked == true);
    CHECK(mapRawClicked == false);
    CHECK(mapScopeClicked == false);

    // 5. Mouse wheel over BottomPanel or Sidebar must NOT zoom MapView
    const keel::f32 zoomBefore = mapZoom;
    ui.OnMouseMove({ 300.0f, 500.0f }); // Over BottomPanel
    ui.OnMouseWheel(0.0f, 3.0f);
    renderBellhopLayout();
    CHECK(keel::abs(mapZoom - zoomBefore) < 0.0001f);

    // 6. Clicking on unoccluded MapView canvas DOES fire mapRawClicked and mapScopeClicked
    ui.OnMouseMove({ 350.0f, 220.0f });
    ui.OnMouseDown(1);
    renderBellhopLayout();
    CHECK(mapScopeHovered == true);
    CHECK(mapRawClicked == true);
    CHECK(mapScopeClicked == true);
    ui.OnMouseUp(1);
    renderBellhopLayout();

    // 7. GIS Isobath (DEPCNT) Hover Occlusion:
    // The 50m isobath runs vertically at lon=131.8735 across both MapView and BottomPanel.
    // When hovering inside MapView at isobathCenterScreen, the isobath MUST highlight.
    // When hovering directly below inside BottomPanel or on the splitter sash, the isobath MUST NOT highlight!
    ui.OnMouseMove(isobathCenterScreen);
    renderBellhopLayout();
    CHECK(mapScopeHovered == true);
    CHECK(gisStats.hoveredFeatureUid != 0);

    ui.OnMouseMove({ isobathCenterScreen.x, bottomRect.Center().y }); // Inside BottomPanel over the same meridian
    renderBellhopLayout();
    CHECK(mapScopeHovered == false);
    CHECK(gisStats.hoveredFeatureUid == 0);

    ui.OnMouseMove({ isobathCenterScreen.x, bottomRect.y - 3.0f }); // On BottomPanel splitter sash over the same meridian
    renderBellhopLayout();
    CHECK(mapScopeHovered == false);
    CHECK(gisStats.hoveredFeatureUid == 0);

    keel::println("  [PASS] TestPanelSashAndBodyOcclusionOverMapViewAndScene completed");
}

void TestGisTileCacheAndTileSourceLambda() {
    keel::println("  -> TestGisTileCacheAndTileSourceLambda...");
    // 3. GisRenderEngine global Tier-1/Tier-2 tile cache + TilePixelBufferPool pan/zoom stress
    {
        GisRenderEngine::InvalidateTileCache();

        keel::Array<geo::GeoLayer> layers;
        layers.resize(6u);
        for (keel::usize li = 0; li < layers.size(); ++li) {
            geo::GeoLayer& lyr = layers[li];
            lyr.name = keel::format("Layer_{}", li);
            lyr.visible = true;
            lyr.style.primaryColor = Color(keel::as<keel::u8>(40 + li * 30), 120, 200, 255);
            lyr.style.useCustomColor = true;
            lyr.style.fillOpacity = 0.75f;
            lyr.style.strokeWidthPx = 1.25f;

            for (keel::i32 gx = 0; gx < 6; ++gx) {
                for (keel::i32 gy = 0; gy < 6; ++gy) {
                    geo::GeoFeature feat{};
                    feat.type = geo::GeometryType::Polygon;
                    const keel::f64 baseLat = 59.80 + gx * 0.06 + keel::as<keel::f64>(li) * 0.004;
                    const keel::f64 baseLon = 29.70 + gy * 0.08 + keel::as<keel::f64>(li) * 0.004;
                    geo::GeoPolygon poly{};
                    poly.exteriorRing.push_back({baseLat, baseLon});
                    poly.exteriorRing.push_back({baseLat, baseLon + 0.045});
                    poly.exteriorRing.push_back({baseLat + 0.040, baseLon + 0.045});
                    poly.exteriorRing.push_back({baseLat + 0.040, baseLon});
                    poly.exteriorRing.push_back({baseLat, baseLon});
                    feat.polygons.push_back(keel::move(poly));
                    lyr.features.push_back(keel::move(feat));
                }
            }
            lyr.FinalizeLayer(9000u + li);
        }

        TestCanvas canvas;
        keel::Array<keel::u32> fb(640u * 480u, 0u);
        canvas.SetTarget(fb.data(), 640, 480, 640);

        Context ui;
        GisRenderConfig cfg{};
        cfg.maxCompositeTiles = 24;
        cfg.maxLayerTiles = 24;
        cfg.asyncBackgroundStreaming = false;

        keel::u64 selectedUid = 0;
        keel::usize totalRendered = 0;
        keel::usize totalHits = 0;
        for (keel::i32 step = 0; step < 24; ++step) {
            const GeoCoord stepCenter{59.92 + (step % 4) * 0.03, 29.85 + (step / 8) * 0.04};
            const keel::f32 stepZoom = 11.0f + keel::as<keel::f32>((step / 2) % 3);
            ui.BeginFrame({640.0f, 480.0f});
            if (auto map = guikit::geo::MapView(ui, {
                .initialCenter = stepCenter,
                .initialTileZoom = stepZoom,
                .height = SizeMode::Stretch
            })) {
                if ((step % 2) == 0) {
                    map.CenterOnGeo(stepCenter, stepZoom);
                }
                const GisRenderStats st = GisRenderEngine::RenderLayers(map, layers, cfg, selectedUid, false);
                totalRendered += st.cachedTilesRendered;
                totalHits += st.cachedTilesHit;
            }
            ui.EndFrame(canvas);
        }
        CHECK(totalRendered > 0u);
        CHECK(totalHits > 0u);

        // Idle away from MapView for 180 frames -> MapViewInternalState::TrimInactiveCaches()
        // automatically triggers GisRenderEngine::TrimInactiveCaches(), then re-entering MapView re-renders cleanly
        for (keel::i32 f = 0; f < 180; ++f) {
            ui.BeginFrame({640.0f, 480.0f});
            ui.Text("Switched away from GIS map");
            ui.EndFrame(canvas);
        }
        canvas.TrimCaches();

        keel::usize postTrimRendered = 0;
        for (keel::i32 step = 0; step < 2; ++step) {
            ui.BeginFrame({640.0f, 480.0f});
            if (auto map = guikit::geo::MapView(ui, {
                .initialCenter = GeoCoord{59.92, 29.85},
                .initialTileZoom = 11.0f,
                .height = SizeMode::Stretch
            })) {
                const GisRenderStats st = GisRenderEngine::RenderLayers(map, layers, cfg, selectedUid, false);
                postTrimRendered += st.cachedTilesRendered;
            }
            ui.EndFrame(canvas);
        }
        CHECK(postTrimRendered > 0u);
        GisRenderEngine::InvalidateTileCache();
    }
    // MapViewOptions::tileSource FunctionRef lambda
    {
        TestCanvas canvas;
        keel::Array<keel::u32> fb(800u * 600u, 0u);
        canvas.SetTarget(fb.data(), 800, 600, 800);
        Context ui;
        keel::i32 baseLayerCalls = 0;
        keel::i32 overlayLayerCalls = 0;
        for (keel::i32 f = 0; f < 2; ++f) {
            ui.BeginFrame({800.0f, 600.0f});
            if (auto col = ui.Column({.gap = 8.0f, .padding = Padding::All(8.0f)})) {
                if (auto map = guikit::geo::MapView(ui, {
                    .tileSource = [&](OsmTileId /*tile*/, keel::i32 layerIndex) -> keel::StringRef {
                        if (layerIndex == 0) ++baseLayerCalls;
                        if (layerIndex == 1) ++overlayLayerCalls;
                        return {};
                    },
                    .height = 240.0f
                })) {}
            }
            ui.EndFrame(canvas);
        }
        CHECK(baseLayerCalls > 0);
        CHECK(overlayLayerCalls > 0);
    }
    {
#if __has_include("keel/http.h")
        const auto parsedHttps = keel::parse_url("https://tile.openstreetmap.org/14/9532/5841.png");
        CHECK(parsedHttps.valid && parsedHttps.is_secure && !parsedHttps.is_websocket);
        CHECK(parsedHttps.host == "tile.openstreetmap.org" && parsedHttps.port == 443);
        CHECK(parsedHttps.path_and_query == "/14/9532/5841.png");

        const auto parsedWss = keel::parse_url("wss://ais.stream.local:8443/live?v=2");
        CHECK(parsedWss.valid && parsedWss.is_secure && parsedWss.is_websocket);
        CHECK(parsedWss.host == "ais.stream.local" && parsedWss.port == 8443);
        CHECK(parsedWss.path_and_query == "/live?v=2");

        keel::HttpClient http("GuiKit-Test/1.0");
        CHECK(!keel::HttpClient::backend_name().empty());

        keel::WebSocketClient ws;
        CHECK(ws.state() == keel::WebSocketState::Disconnected);
#endif

        static constexpr keel::u8 kSample1x1Png[] = {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
            0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
            0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,
            0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
            0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D,
            0xB0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
            0x44, 0xAE, 0x42, 0x60, 0x82
        };
        keel::Array<keel::u32> rgba;
#if defined(GUIKIT_HAS_BACKEND_THORVG) && GUIKIT_HAS_BACKEND_THORVG
        CHECK(guikit::geo::DecodePngAndScaleToRgba(kSample1x1Png, 256, rgba));
        CHECK(rgba.size() == 256u * 256u);
#else
        // Without a registered image decoder backend, DecodePngAndScaleToRgba gracefully returns false.
        CHECK(!guikit::geo::DecodePngAndScaleToRgba(kSample1x1Png, 256, rgba));
#endif
    }
    keel::println("  [PASS] TestGisTileCacheAndTileSourceLambda completed");
}

void TestGisAsyncTileStreamingStress() {
    keel::println("  -> TestGisAsyncTileStreamingStress...");
    GisRenderEngine::InvalidateTileCache();

    // Build 6 dense polygon layers (> 60 polygons intersecting each visible tile) so tiles bypass
    // `isLightweightInRamTile` and genuinely stress parallel background ThorVG tile workers + tight cache eviction.
    keel::Array<geo::GeoLayer> layers;
    layers.resize(6u);
    for (keel::usize li = 0; li < layers.size(); ++li) {
        geo::GeoLayer& lyr = layers[li];
        lyr.name = keel::format("AsyncDenseLayer_{}", li);
        lyr.visible = true;
        lyr.style.primaryColor = Color(keel::as<keel::u8>(50 + li * 30), 140, 210, 255);
        lyr.style.useCustomColor = true;
        lyr.style.fillOpacity = 0.7f;
        lyr.style.strokeWidthPx = 1.1f;

        for (keel::i32 gx = 0; gx < 10; ++gx) {
            for (keel::i32 gy = 0; gy < 10; ++gy) {
                geo::GeoFeature feat{};
                feat.type = geo::GeometryType::Polygon;
                const keel::f64 lat = 44.52 + gx * 0.018 + keel::as<keel::f64>(li) * 0.001;
                const keel::f64 lon = 33.40 + gy * 0.022 + keel::as<keel::f64>(li) * 0.001;
                geo::GeoPolygon poly{};
                for (keel::i32 k = 0; k < 12; ++k) {
                    const keel::f64 ang = keel::as<keel::f64>(k) * (2.0 * keel::kPi / 12.0);
                    poly.exteriorRing.push_back({
                        lat + 0.008 * keel::sin(ang),
                        lon + 0.010 * keel::cos(ang)
                    });
                }
                poly.exteriorRing.push_back(poly.exteriorRing[0]);
                feat.polygons.push_back(keel::move(poly));
                lyr.features.push_back(keel::move(feat));
            }
        }
        lyr.FinalizeLayer(19000u + li);
    }

    TestCanvas canvas;
    keel::Array<keel::u32> fb(1024u * 768u, 0u);
    canvas.SetTarget(fb.data(), 1024, 768, 1024);

    Context ui;
    GisRenderConfig cfg{};
    cfg.asyncBackgroundStreaming = true;
    cfg.maxLayerTiles = 20;
    cfg.maxCompositeTiles = 16;

    keel::u64 selectedUid = 0;
    for (keel::i32 step = 0; step < 36; ++step) {
        const GeoCoord stepCenter{44.58 + (step % 6) * 0.012, 33.46 + (step / 6) * 0.015};
        const keel::f32 stepZoom = 12.0f + keel::as<keel::f32>((step / 3) % 3) * 0.5f;
        ui.BeginFrame({1024.0f, 768.0f});
        if (auto map = guikit::geo::MapView(ui, {
            .initialCenter = stepCenter,
            .initialTileZoom = stepZoom,
            .height = SizeMode::Stretch
        })) {
            map.CenterOnGeo(stepCenter, stepZoom);
            (void)GisRenderEngine::RenderLayers(map, layers, cfg, selectedUid, true);
        }
        ui.EndFrame(canvas);
    }

    // Hold viewport steady and verify all visible tiles finish loading (no eviction livelock)
    const GeoCoord finalCenter{44.60, 33.50};
    const keel::f32 finalZoom = 12.5f;
    GisRenderStats settledStats{};
    for (keel::i32 settle = 0; settle < 12; ++settle) {
        CHECK(GisRenderEngine::WaitForBackgroundStreamingIdle(5000));
        ui.BeginFrame({1024.0f, 768.0f});
        if (auto map = guikit::geo::MapView(ui, {
            .initialCenter = finalCenter,
            .initialTileZoom = finalZoom,
            .height = SizeMode::Stretch
        })) {
            map.CenterOnGeo(finalCenter, finalZoom);
            settledStats = GisRenderEngine::RenderLayers(map, layers, cfg, selectedUid, true);
        }
        ui.EndFrame(canvas);
        if (settledStats.cachedTilesPending == 0 && settledStats.cachedTilesHit > 0) {
            break;
        }
    }
    CHECK(settledStats.cachedTilesPending == 0u);
    CHECK(settledStats.cachedTilesHit > 0u);

    // Regression test: switching to 512px tiles with default 48-tile limits (scaled down to 16 at 512px)
    // must converge cleanly and stay 100% cache-hit across subsequent frames (zero infinite loading flicker).
    cfg.tileSizePx = 512;
    cfg.maxLayerTiles = 48;
    cfg.maxCompositeTiles = 48;
    GisRenderStats settled512{};
    for (keel::i32 settle = 0; settle < 20; ++settle) {
        CHECK(GisRenderEngine::WaitForBackgroundStreamingIdle(5000));
        ui.BeginFrame({1024.0f, 768.0f});
        if (auto map = guikit::geo::MapView(ui, {
            .initialCenter = finalCenter,
            .initialTileZoom = finalZoom,
            .height = SizeMode::Stretch
        })) {
            map.CenterOnGeo(finalCenter, finalZoom);
            settled512 = GisRenderEngine::RenderLayers(map, layers, cfg, selectedUid, true);
        }
        ui.EndFrame(canvas);
        if (settled512.cachedTilesPending == 0u && settled512.cachedTilesHit > 0u) {
            break;
        }
    }
    CHECK(settled512.cachedTilesPending == 0u);
    CHECK(settled512.cachedTilesHit > 0u);

    for (keel::i32 steady = 0; steady < 10; ++steady) {
        ui.BeginFrame({1024.0f, 768.0f});
        if (auto map = guikit::geo::MapView(ui, {
            .initialCenter = finalCenter,
            .initialTileZoom = finalZoom,
            .height = SizeMode::Stretch
        })) {
            map.CenterOnGeo(finalCenter, finalZoom);
            settled512 = GisRenderEngine::RenderLayers(map, layers, cfg, selectedUid, true);
        }
        ui.EndFrame(canvas);
        CHECK(settled512.cachedTilesPending == 0u);
        CHECK(settled512.cachedTilesRendered == 0u);
        CHECK(settled512.cachedTilesHit > 0u);
    }

    GisRenderEngine::InvalidateTileCache();
    keel::println("  [PASS] TestGisAsyncTileStreamingStress completed");
}

void TestGisFeatureSelectionAndEmptyClickDeselect() {
    keel::println("  -> TestGisFeatureSelectionAndEmptyClickDeselect...");

    keel::Array<geo::GeoLayer> layers;
    layers.resize(1);
    geo::GeoLayer& lyr = layers[0];
    lyr.name = keel::String::from("Buoys");
    lyr.visible = true;
    geo::GeoFeature buoy{};
    buoy.id = keel::String::from("buoy_1");
    buoy.name = keel::String::from("North Cardinal");
    buoy.type = geo::GeometryType::Point;
    buoy.chartClass = geo::ChartFeatureClass::Generic;
    buoy.points.push_back({44.60, 33.50});
    lyr.features.push_back(keel::move(buoy));
    lyr.FinalizeLayer(29001u);

    TestCanvas canvas;
    keel::Array<keel::u32> fb(800u * 600u, 0u);
    canvas.SetTarget(fb.data(), 800, 600, 800);

    Context ui;
    GisRenderConfig cfg{};
    cfg.useVectorTileCache = false;
    cfg.asyncBackgroundStreaming = false;
    cfg.enableFeaturePicking = true;

    keel::u64 selectedUid = 0;
    Vec2 buoyScreenPos{};
    auto runFrame = [&]() {
        ui.BeginFrame({800.0f, 600.0f});
        if (auto map = guikit::geo::MapView(ui, {
            .initialCenter = {44.60, 33.50},
            .initialTileZoom = 13.0f,
            .height = SizeMode::Stretch
        })) {
            buoyScreenPos = map.GeoToScreen({44.60, 33.50});
            (void)GisRenderEngine::RenderLayers(map, layers, cfg, selectedUid, true);
        }
        ui.EndFrame(canvas);
    };

    // Settle initial camera bounds
    runFrame();
    runFrame();
    CHECK(selectedUid == 0u);

    // 1. Click directly on the buoy -> selects it
    ui.OnMouseMove(buoyScreenPos);
    runFrame();
    ui.OnMouseDown(1);
    runFrame();
    ui.OnMouseUp(1);
    runFrame();
    runFrame();
    CHECK(selectedUid != 0u);
    const keel::u64 pickedUid = selectedUid;

    // 2. Drag-pan across empty space -> must preserve selection (panning must not deselect)
    const Vec2 emptyPos{buoyScreenPos.x + 180.0f, buoyScreenPos.y + 120.0f};
    ui.OnMouseMove(emptyPos);
    runFrame();
    ui.OnMouseDown(1);
    runFrame();
    ui.OnMouseMove({emptyPos.x + 60.0f, emptyPos.y + 40.0f});
    runFrame();
    ui.OnMouseUp(1);
    runFrame();
    runFrame();
    CHECK(selectedUid == pickedUid);

    // 3. Click on empty space without dragging -> clears selection
    const Vec2 emptyClickPos{200.0f, 160.0f};
    ui.OnMouseMove(emptyClickPos);
    runFrame();
    ui.OnMouseDown(1);
    runFrame();
    ui.OnMouseUp(1);
    runFrame();
    runFrame();
    CHECK(selectedUid == 0u);

    keel::println("  [PASS] TestGisFeatureSelectionAndEmptyClickDeselect completed");
}

int main() {
    keel::println("[RUN] Testing GuiKit Geo module...");
#if !defined(GUIKIT_HAS_BACKEND_THORVG) || !GUIKIT_HAS_BACKEND_THORVG
    if (!guikit::HasDefaultVectorCanvasFactory()) {
        guikit::RegisterDefaultVectorCanvasFactory([](bool) -> keel::UniquePtr<guikit::ICanvas> {
            return keel::make_unique<guikit::NullCanvas>();
        });
    }
#endif
    TestMapGridAndProjections();
    TestMapViewWidgetWorkflow();
    TestGisDemoBayGeoJsonAndRenderEngine();
    TestPanelSashAndBodyOcclusionOverMapViewAndScene();
    TestGisTileCacheAndTileSourceLambda();
    TestGisAsyncTileStreamingStress();
    TestGisFeatureSelectionAndEmptyClickDeselect();
    if (g_failures == 0) {
        keel::println("[PASS] All GuiKit Geo tests passed successfully!");
        return 0;
    }
    keel::eprintln("[FAIL] Total failures: {}", g_failures);
    return 1;
}
