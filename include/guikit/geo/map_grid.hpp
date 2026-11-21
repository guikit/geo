#pragma once

#include "guikit/core/types.hpp"
#include "keel/array.h"
#include "keel/core.h"
#include "keel/format.h"
#include "keel/function_ref.h"
#include "keel/math.h"
#include "keel/static_array.h"
#include "keel/string.h"

namespace guikit::geo {

/// Planar 2D double-precision coordinate pair supporting both structured bindings (`auto [x, y]`) and `.first`/`.second`.
struct GeoPlanar {
    keel::f64 first{0.0};
    keel::f64 second{0.0};
};

/// Coordinate Reference System (CRS) / Map Projection model.
enum class MapCrs : keel::u8 {
    EPSG3857_WebMercator = 0,   ///< Spherical Web Mercator (OSM, OpenSeaMap, Google/Mapbox standard)
    EPSG3395_WorldMercator,     ///< WGS84 Ellipsoidal Mercator (True conformal nautical chart projection)
    EPSG4326_PlateCarree,       ///< Equirectangular WGS84 Geographic Lon/Lat grid
    EPSG32636_Utm36N,           ///< WGS84 / UTM Zone 36N (30°E..36°E, CM 33°E — Black Sea / Sevastopol)
    EPSG20906_Gsk2011Zone6,     ///< GSK-2011 / Gauss-Kruger Zone 6 (CM 33°E — RF State Geodetic Standard)
    EPSG28406_Pulkovo42Zone6,   ///< Pulkovo 1942 (SK-42) / Gauss-Kruger Zone 6 (CM 33°E — Krassowsky 1940 + GOST)
    EPSG3995_ArcticPolar        ///< WGS84 / Arctic Polar Stereographic (Standard parallel 71°N — IHO High-Latitude)
};

/// Display formatting mode for geographic and geodetic planar coordinates.
enum class GeoCoordFormat : keel::u8 {
    NauticalDMM = 0,            ///< Degrees & Decimal Minutes: 54° 10.92' N, 007° 53.20' E (1' lat = 1 NM)
    DecimalDegrees,             ///< Signed/cardinal decimal degrees: 54.1820° N, 007.8867° E
    DMS,                        ///< Degrees, Minutes, Seconds: 54° 10' 55" N, 007° 53' 12" E
    UTM_Meters,                 ///< WGS84 UTM Zone + Easting/Northing in meters (e.g. 36N E 691425 m, N 4938121 m)
    GaussKruger_Meters          ///< GOST Gauss-Kruger Zone + X (North) / Y (East) in meters (GSK-2011 / SK-42)
};

/// Reference ellipsoid parameters (semi-major axis a and flattening f).
struct EllipsoidSpec {
    keel::f64 a{6378137.0};
    keel::f64 f{1.0 / 298.257223563};

    [[nodiscard]] static constexpr EllipsoidSpec Wgs84() noexcept {
        return {6378137.0, 1.0 / 298.257223563};
    }
    [[nodiscard]] static constexpr EllipsoidSpec Gsk2011() noexcept {
        return {6378136.5, 1.0 / 298.2564151};
    }
    [[nodiscard]] static constexpr EllipsoidSpec Krassowsky1940() noexcept {
        return {6378245.0, 1.0 / 298.3};
    }
    [[nodiscard]] static constexpr EllipsoidSpec Pz90_11() noexcept {
        return {6378136.0, 1.0 / 298.25784};
    }
};

/// 7-parameter Helmert datum transformation to WGS84 according to GOST 32453-2017.
struct Helmert7Spec {
    keel::f64 dx{0.0};             ///< X translation in meters (to WGS84)
    keel::f64 dy{0.0};             ///< Y translation in meters (to WGS84)
    keel::f64 dz{0.0};             ///< Z translation in meters (to WGS84)
    keel::f64 wxArcSec{0.0};       ///< X rotation in arc-seconds
    keel::f64 wyArcSec{0.0};       ///< Y rotation in arc-seconds
    keel::f64 wzArcSec{0.0};       ///< Z rotation in arc-seconds
    keel::f64 scalePpm{0.0};       ///< Scale difference in parts per million (10^-6)
    bool isIdentity{true};

    [[nodiscard]] static constexpr Helmert7Spec Identity() noexcept {
        return {};
    }
    /// GOST 32453-2017: Pulkovo 1942 (SK-42) -> WGS84 / PZ-90.11
    [[nodiscard]] static constexpr Helmert7Spec Sk42ToWgs84() noexcept {
        return {+23.57, -140.95, -79.80, 0.0, -0.35, -0.79, -0.22, false};
    }
    /// GOST 32453-2017: SK-95 -> WGS84 / PZ-90.11
    [[nodiscard]] static constexpr Helmert7Spec Sk95ToWgs84() noexcept {
        return {+24.47, -130.89, -81.56, 0.0, 0.0, -0.13, -0.22, false};
    }
    /// GOST 32453-2017: PZ-90.11 -> WGS84
    [[nodiscard]] static constexpr Helmert7Spec Pz90ToWgs84() noexcept {
        return {-0.013, +0.106, +0.022, +0.0023, -0.00354, +0.00421, -0.008, false};
    }
};

/// Parametric Transverse Mercator / Gauss-Kruger configuration.
struct TransverseMercatorSpec {
    EllipsoidSpec ellipsoid{EllipsoidSpec::Wgs84()};
    Helmert7Spec datumToWgs84{Helmert7Spec::Identity()};
    keel::f64 centralMeridianDeg{33.0};
    keel::f64 scaleFactor{0.9996};
    keel::f64 falseEasting{500000.0};
    keel::f64 falseNorthing{0.0};
};

/// Geographic coordinate in degrees (WGS84).
struct GeoCoord {
    keel::f64 lat{0.0};            ///< Latitude in degrees [-90..+90] (North positive)
    keel::f64 lon{0.0};            ///< Longitude in degrees [-180..+180] (East positive)
};

/// Geographic bounding box defined by North-West and South-East corners.
struct GeoBounds {
    GeoCoord nw{};              ///< North-West corner (max lat, min lon)
    GeoCoord se{};              ///< South-East corner (min lat, max lon)

    [[nodiscard]] constexpr keel::f64 WidthLon() const noexcept { return se.lon - nw.lon; }
    [[nodiscard]] constexpr keel::f64 HeightLat() const noexcept { return nw.lat - se.lat; }
    [[nodiscard]] constexpr keel::f64 SpanLon() const noexcept { return se.lon >= nw.lon ? (se.lon - nw.lon) : (nw.lon - se.lon); }
    [[nodiscard]] constexpr keel::f64 SpanLat() const noexcept { return nw.lat >= se.lat ? (nw.lat - se.lat) : (se.lat - nw.lat); }
    [[nodiscard]] constexpr GeoCoord Center() const noexcept {
        return {(nw.lat + se.lat) * 0.5, (nw.lon + se.lon) * 0.5};
    }
    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return (nw.lat != 0.0 || nw.lon != 0.0 || se.lat != 0.0 || se.lon != 0.0) &&
               (nw.lat >= se.lat) && (se.lon >= nw.lon);
    }
};

using GeoBBox = GeoBounds;

/// Slippy Map tile pyramid address [z/x/y].
struct OsmTileId {
    keel::i32 z{0};                   ///< Zoom level [0..19]
    keel::i32 x{0};                   ///< Tile column [0..2^z - 1] (West -> East)
    keel::i32 y{0};                   ///< Tile row [0..2^z - 1] (North -> South)

    [[nodiscard]] constexpr bool operator==(const OsmTileId& o) const noexcept {
        return z == o.z && x == o.x && y == o.y;
    }
};

/// Resolved geometry and geographic bounds of a single visible tile in Scene world space.
struct OsmTileCell {
    OsmTileId id{};
    Rect worldRect{};           ///< Bounding rectangle in Scene world coordinates
    GeoBounds geoBounds{};      ///< Geographic NW/SE bounds of this tile
};

/// Single meridian (constant longitude) or parallel (constant latitude) in the nautical graticule.
struct GraticuleLine {
    keel::f64 degrees{0.0};        ///< Latitude or longitude in degrees
    keel::f32 worldPos{0.0f};       ///< X (for meridian) or Y (for parallel) in Scene world coordinates
    keel::i32 stepIndex{0};           ///< Signed integer multiple of stepDeg from 0° (for pan-invariant ruler zebra bars)
    bool isMajor{false};        ///< True if whole degree or major 5x subdivision
    bool isZero{false};         ///< True for Equator (0° lat) or Prime Meridian (0° lon)
};

/// Computed physical scale bar specification (Metric or Nautical).
struct MapScaleBar {
    keel::f64 distanceValue{0.0};  ///< Numeric distance value (in meters or nautical miles)
    keel::f32 screenPixels{80.0f};  ///< Exact bar length in screen pixels at current view center
    char label[32]{};           ///< Formatted label (e.g. "500 m", "2 km", "5 NM", "2 cab")
};

/// Universal callback for resolving a tile image source (file path, URI, or cached PNG/SVG asset).
/// `layerIndex == 0` is the base map (e.g. OSM), `layerIndex == 1` is an overlay (e.g. OpenSeaMap seamark).
using TileSourceCallback = keel::FunctionRef<keel::StringRef(OsmTileId tile, keel::i32 layerIndex)>;

/// Configuration for map projection, tile pyramid, and nautical graticule calculation.
struct MapGridConfig {
    MapCrs crs{MapCrs::EPSG3857_WebMercator};
    keel::f32 worldSize{2048.0f};   ///< Scene world extent (0..worldSize in X and Y)
    keel::i32 baseZoom{3};            ///< Tile zoom level corresponding to sceneZoom == 1.0f (2048 / 256 = 8 = 2^3)
    keel::i32 minTileZoom{1};
    keel::i32 maxTileZoom{18};
    keel::f32 zoomBias{0.0f};       ///< Optional LOD offset applied before selecting integer tile level z
    TileSourceCallback tileSource{};
};

/// Complete per-frame calculated state of the map view, visible tile pyramid, and nautical graticule.
struct MapViewState {
    MapCrs crs{MapCrs::EPSG3857_WebMercator};
    keel::f32 continuousZoom{3.0f}; ///< Smooth fractional zoom level: baseZoom + log2(sceneZoom)
    keel::i32 tileZoom{3};            ///< Active discrete tile pyramid level z
    keel::i32 totalTilesPerAxis{8};   ///< 2^z tiles along each axis
    keel::i32 minTileX{0};
    keel::i32 maxTileX{0};
    keel::i32 minTileY{0};
    keel::i32 maxTileY{0};
    keel::i32 visibleTileCount{0};

    GeoCoord center{};          ///< Geographic coordinates at the center of the viewport
    GeoCoord cursor{};          ///< Geographic coordinates directly under the mouse cursor
    GeoBounds visibleBounds{};  ///< Geographic NW/SE bounds of the visible viewport

    keel::f64 metersPerPixel{1.0}; ///< Ground resolution at center latitude (m / screen px)
    keel::f64 graticuleStepArcMin{60.0}; ///< Active nautical graticule step in arc-minutes (1' = 1 NM)
    MapScaleBar metricScale{};  ///< Metric scale bar (m / km)
    MapScaleBar nauticalScale{};///< Nautical scale bar (NM / cables, 1 NM = 1852 m)

    keel::Array<OsmTileCell> visibleTiles{};
    keel::Array<GraticuleLine> meridians{}; ///< Visible vertical longitude lines
    keel::Array<GraticuleLine> parallels{}; ///< Visible horizontal latitude lines

    MapViewState() = default;
    MapViewState(MapViewState&&) noexcept = default;
    MapViewState& operator=(MapViewState&&) noexcept = default;

    MapViewState(const MapViewState& o) { *this = o.clone(); }
    MapViewState& operator=(const MapViewState& o) {
        if (this != &o) {
            *this = o.clone();
        }
        return *this;
    }

    [[nodiscard]] MapViewState clone() const {
        MapViewState out{};
        out.crs = crs;
        out.continuousZoom = continuousZoom;
        out.tileZoom = tileZoom;
        out.totalTilesPerAxis = totalTilesPerAxis;
        out.minTileX = minTileX;
        out.maxTileX = maxTileX;
        out.minTileY = minTileY;
        out.maxTileY = maxTileY;
        out.visibleTileCount = visibleTileCount;
        out.center = center;
        out.cursor = cursor;
        out.visibleBounds = visibleBounds;
        out.metersPerPixel = metersPerPixel;
        out.graticuleStepArcMin = graticuleStepArcMin;
        out.metricScale = metricScale;
        out.nauticalScale = nauticalScale;
        out.visibleTiles = visibleTiles.clone();
        out.meridians = meridians.clone();
        out.parallels = parallels.clone();
        return out;
    }
};

/// Pure mathematical engine for Web Mercator / WGS84 / UTM / Gauss-Kruger / Polar Stereographic projections,
/// GOST 32453-2017 Helmert datum transformations, OSM [z/x/y] tile grids, and OpenSeaMap nautical graticules.
class MapGrid {
public:
    static constexpr keel::f64 kPi = keel::kPi;
    static constexpr keel::f64 kEarthRadiusMeters = 6378137.0;
    static constexpr keel::f64 kEarthCircumferenceMeters = 2.0 * kPi * kEarthRadiusMeters; // 40075016.6856 m
    static constexpr keel::f64 kWgs84Eccentricity = 0.08181919084262149; // WGS84 ellipsoid eccentricity e
    static constexpr keel::f64 kMetersPerNauticalMile = 1852.0;          // Exact international nautical mile (1' latitude)
    static constexpr keel::f64 kMaxMercatorLat = 85.05112877980659;      // atan(sinh(pi)) * 180 / pi

    /// Returns true if the CRS is a standard axis-aligned cylindrical projection (3857, 3395, 4326).
    [[nodiscard]] static constexpr bool IsCylindricalCrs(MapCrs crs) noexcept {
        return crs == MapCrs::EPSG3857_WebMercator ||
               crs == MapCrs::EPSG3395_WorldMercator ||
               crs == MapCrs::EPSG4326_PlateCarree;
    }

    /// Returns a concise human-readable EPSG & projection label for UI telemetry.
    [[nodiscard]] static constexpr keel::StringRef CrsDisplayName(MapCrs crs) noexcept {
        switch (crs) {
            case MapCrs::EPSG3857_WebMercator:    return "EPSG:3857 (Web Mercator)";
            case MapCrs::EPSG3395_WorldMercator:  return "EPSG:3395 (WGS84 World Mercator)";
            case MapCrs::EPSG4326_PlateCarree:    return "EPSG:4326 (WGS84 Equirectangular)";
            case MapCrs::EPSG32636_Utm36N:        return "EPSG:32636 (WGS84 / UTM Zone 36N)";
            case MapCrs::EPSG20906_Gsk2011Zone6:  return "EPSG:20906 (GSK-2011 / GK Zone 6)";
            case MapCrs::EPSG28406_Pulkovo42Zone6:return "EPSG:28406 (Pulkovo 1942 / SK-42 Z6)";
            case MapCrs::EPSG3995_ArcticPolar:    return "EPSG:3995 (Arctic Polar Stereo 71°N)";
        }
        return "EPSG:3857 (Web Mercator)";
    }

    /// Clamps latitude to the valid domain of the specified CRS.
    [[nodiscard]] static constexpr keel::f64 ClampLat(keel::f64 lat, MapCrs crs = MapCrs::EPSG3857_WebMercator) noexcept {
        if (crs == MapCrs::EPSG3995_ArcticPolar) {
            return keel::clamp(lat, -30.0, 89.9999);
        }
        const bool isMerc = (crs == MapCrs::EPSG3857_WebMercator || crs == MapCrs::EPSG3395_WorldMercator);
        const keel::f64 limit = isMerc ? kMaxMercatorLat : 89.9999;
        return keel::clamp(lat, -limit, limit);
    }

    /// Wraps or clamps longitude to [-180..+180].
    [[nodiscard]] static constexpr keel::f64 ClampLon(keel::f64 lon) noexcept {
        return keel::clamp(lon, -180.0, 180.0);
    }

    /// Applies a 7-parameter GOST 32453-2017 Helmert transformation from source ellipsoid to WGS84 (or inverse if `inverse == true`).
    [[nodiscard]] static GeoCoord TransformDatumHelmert(
        GeoCoord geo,
        const EllipsoidSpec& srcEllipsoid,
        const EllipsoidSpec& dstEllipsoid,
        const Helmert7Spec& toWgs84,
        bool inverse = false
    ) noexcept {
        if (toWgs84.isIdentity) {
            return geo;
        }

        constexpr keel::f64 kArcSecToRad = keel::kPi / (180.0 * 3600.0);

        const keel::f64 phi = keel::deg_to_rad(keel::clamp(geo.lat, -89.999999, 89.999999));
        const keel::f64 lam = keel::deg_to_rad(geo.lon);
        const keel::f64 sinPhi = keel::sin(phi);
        const keel::f64 cosPhi = keel::cos(phi);
        const keel::f64 sinLam = keel::sin(lam);
        const keel::f64 cosLam = keel::cos(lam);

        const keel::f64 e1Sq = 2.0 * srcEllipsoid.f - srcEllipsoid.f * srcEllipsoid.f;
        const keel::f64 n1 = srcEllipsoid.a / keel::sqrt(1.0 - e1Sq * sinPhi * sinPhi);
        const keel::f64 x1 = n1 * cosPhi * cosLam;
        const keel::f64 y1 = n1 * cosPhi * sinLam;
        const keel::f64 z1 = n1 * (1.0 - e1Sq) * sinPhi;

        const keel::f64 s = (inverse ? -1.0 : 1.0);
        const keel::f64 dx = s * toWgs84.dx;
        const keel::f64 dy = s * toWgs84.dy;
        const keel::f64 dz = s * toWgs84.dz;
        const keel::f64 wx = s * toWgs84.wxArcSec * kArcSecToRad;
        const keel::f64 wy = s * toWgs84.wyArcSec * kArcSecToRad;
        const keel::f64 wz = s * toWgs84.wzArcSec * kArcSecToRad;
        const keel::f64 m  = 1.0 + s * toWgs84.scalePpm * 1e-6;

        // GOST 32453-2017 Eq. (13) coordinate frame rotation + translation
        const keel::f64 x2 = m * ( x1 + wz * y1 - wy * z1) + dx;
        const keel::f64 y2 = m * (-wz * x1 + y1 + wx * z1) + dy;
        const keel::f64 z2 = m * ( wy * x1 - wx * y1 + z1) + dz;

        const keel::f64 e2Sq = 2.0 * dstEllipsoid.f - dstEllipsoid.f * dstEllipsoid.f;
        const keel::f64 b2 = dstEllipsoid.a * (1.0 - dstEllipsoid.f);
        const keel::f64 ep2Sq = (dstEllipsoid.a * dstEllipsoid.a - b2 * b2) / (b2 * b2);
        const keel::f64 p = keel::hypot(x2, y2);
        if (p < 1e-9) {
            return {z2 >= 0.0 ? 90.0 : -90.0, geo.lon};
        }

        keel::f64 theta = keel::atan2(z2 * dstEllipsoid.a, p * b2);
        keel::f64 phi2 = 0.0;
        for (keel::i32 i = 0; i < 2; ++i) {
            const keel::f64 sinT = keel::sin(theta);
            const keel::f64 cosT = keel::cos(theta);
            phi2 = keel::atan2(
                z2 + ep2Sq * b2 * sinT * sinT * sinT,
                p - e2Sq * dstEllipsoid.a * cosT * cosT * cosT
            );
            theta = keel::atan((1.0 - dstEllipsoid.f) * keel::tan(phi2));
        }
        const keel::f64 lam2 = keel::atan2(y2, x2);

        return {
            keel::clamp(keel::rad_to_deg(phi2), -89.999999, 89.999999),
            ClampLon(keel::rad_to_deg(lam2))
        };
    }

    /// Forward Krüger 4th-order (O(e^8)) Conformal-Latitude Transverse Mercator:
    /// Converts WGS84 (lat, lon) into metric (Easting, Northing) according to `spec`.
    [[nodiscard]] static GeoPlanar TransverseMercatorForward(
        GeoCoord wgs84,
        const TransverseMercatorSpec& spec
    ) noexcept {
        const GeoCoord localGeo = TransformDatumHelmert(
            wgs84, EllipsoidSpec::Wgs84(), spec.ellipsoid, spec.datumToWgs84, /*inverse=*/true
        );

        const keel::f64 f = spec.ellipsoid.f;
        const keel::f64 n = f / (2.0 - f);
        const keel::f64 n2 = n * n;
        const keel::f64 n3 = n2 * n;
        const keel::f64 n4 = n2 * n2;
        const keel::f64 e = keel::sqrt(2.0 * f - f * f);
        const keel::f64 A = (spec.ellipsoid.a / (1.0 + n)) * (1.0 + 0.25 * n2 + (1.0 / 64.0) * n4);

        const keel::f64 alpha1 = (1.0 / 2.0) * n - (2.0 / 3.0) * n2 + (5.0 / 16.0) * n3 + (41.0 / 180.0) * n4;
        const keel::f64 alpha2 = (13.0 / 48.0) * n2 - (3.0 / 5.0) * n3 + (557.0 / 1440.0) * n4;
        const keel::f64 alpha3 = (61.0 / 240.0) * n3 - (103.0 / 140.0) * n4;
        const keel::f64 alpha4 = (49561.0 / 161280.0) * n4;

        const keel::f64 phi = keel::deg_to_rad(keel::clamp(localGeo.lat, -89.9999, 89.9999));
        keel::f64 dLonDeg = localGeo.lon - spec.centralMeridianDeg;
        while (dLonDeg > 180.0) dLonDeg -= 360.0;
        while (dLonDeg < -180.0) dLonDeg += 360.0;
        const keel::f64 dLam = keel::deg_to_rad(keel::clamp(dLonDeg, -75.0, 75.0));

        const keel::f64 sinPhi = keel::sin(phi);
        const keel::f64 Q = keel::asinh(keel::tan(phi)) - e * keel::atanh(e * sinPhi);
        const keel::f64 tanChi = keel::sinh(Q);
        const keel::f64 cosChi = 1.0 / keel::hypot(1.0, tanChi);

        const keel::f64 xiP = keel::atan2(tanChi, keel::cos(dLam));
        const keel::f64 etaP = keel::atanh(keel::clamp(cosChi * keel::sin(dLam), -0.9999999, 0.9999999));

        const keel::f64 xi = xiP
            + alpha1 * keel::sin(2.0 * xiP) * keel::cosh(2.0 * etaP)
            + alpha2 * keel::sin(4.0 * xiP) * keel::cosh(4.0 * etaP)
            + alpha3 * keel::sin(6.0 * xiP) * keel::cosh(6.0 * etaP)
            + alpha4 * keel::sin(8.0 * xiP) * keel::cosh(8.0 * etaP);

        const keel::f64 eta = etaP
            + alpha1 * keel::cos(2.0 * xiP) * keel::sinh(2.0 * etaP)
            + alpha2 * keel::cos(4.0 * xiP) * keel::sinh(4.0 * etaP)
            + alpha3 * keel::cos(6.0 * xiP) * keel::sinh(6.0 * etaP)
            + alpha4 * keel::cos(8.0 * xiP) * keel::sinh(8.0 * etaP);

        const keel::f64 easting  = spec.falseEasting  + spec.scaleFactor * A * eta;
        const keel::f64 northing = spec.falseNorthing + spec.scaleFactor * A * xi;
        return {easting, northing};
    }

    /// Inverse Krüger 4th-order (O(e^8)) Conformal-Latitude Transverse Mercator:
    /// Converts metric (Easting, Northing) back into WGS84 (lat, lon) degrees.
    [[nodiscard]] static GeoCoord TransverseMercatorInverse(
        keel::f64 easting,
        keel::f64 northing,
        const TransverseMercatorSpec& spec
    ) noexcept {
        const keel::f64 f = spec.ellipsoid.f;
        const keel::f64 n = f / (2.0 - f);
        const keel::f64 n2 = n * n;
        const keel::f64 n3 = n2 * n;
        const keel::f64 n4 = n2 * n2;
        const keel::f64 eSq = 2.0 * f - f * f;
        const keel::f64 e = keel::sqrt(eSq);
        const keel::f64 A = (spec.ellipsoid.a / (1.0 + n)) * (1.0 + 0.25 * n2 + (1.0 / 64.0) * n4);

        const keel::f64 beta1 = (1.0 / 2.0) * n - (2.0 / 3.0) * n2 + (37.0 / 96.0) * n3 - (1.0 / 360.0) * n4;
        const keel::f64 beta2 = (1.0 / 48.0) * n2 + (1.0 / 15.0) * n3 - (437.0 / 1440.0) * n4;
        const keel::f64 beta3 = (17.0 / 480.0) * n3 - (37.0 / 840.0) * n4;
        const keel::f64 beta4 = (4397.0 / 161280.0) * n4;

        const keel::f64 invScaleA = 1.0 / (spec.scaleFactor * A);
        const keel::f64 xi  = keel::clamp((northing - spec.falseNorthing) * invScaleA, -1.56, 1.56);
        const keel::f64 eta = keel::clamp((easting  - spec.falseEasting)  * invScaleA, -2.20, 2.20);

        const keel::f64 xiP = xi
            - beta1 * keel::sin(2.0 * xi) * keel::cosh(2.0 * eta)
            - beta2 * keel::sin(4.0 * xi) * keel::cosh(4.0 * eta)
            - beta3 * keel::sin(6.0 * xi) * keel::cosh(6.0 * eta)
            - beta4 * keel::sin(8.0 * xi) * keel::cosh(8.0 * eta);

        const keel::f64 etaP = eta
            - beta1 * keel::cos(2.0 * xi) * keel::sinh(2.0 * eta)
            - beta2 * keel::cos(4.0 * xi) * keel::sinh(4.0 * eta)
            - beta3 * keel::cos(6.0 * xi) * keel::sinh(6.0 * eta)
            - beta4 * keel::cos(8.0 * xi) * keel::sinh(8.0 * eta);

        const keel::f64 sinhEtaP = keel::sinh(etaP);
        const keel::f64 coshEtaP = keel::cosh(etaP);
        const keel::f64 sinChi = keel::clamp(keel::sin(xiP) / coshEtaP, -0.999999999, 0.999999999);
        const keel::f64 dLam = keel::atan2(sinhEtaP, keel::cos(xiP));

        // Karney's quadratic Newton iteration for geodetic tangent tau = tan(phi)
        const keel::f64 Q = keel::atanh(sinChi);
        const keel::f64 sinhQ = keel::sinh(Q);
        keel::f64 tau = sinhQ;
        for (keel::i32 iter = 0; iter < 5; ++iter) {
            const keel::f64 tauHyp = keel::hypot(1.0, tau);
            const keel::f64 sigma = keel::sinh(e * keel::atanh(e * tau / tauHyp));
            const keel::f64 tauP = tau * keel::hypot(1.0, sigma) - sigma * tauHyp;
            const keel::f64 dTau = ((sinhQ - tauP) / keel::hypot(1.0, tauP)) *
                                ((1.0 + (1.0 - eSq) * tau * tau) / ((1.0 - eSq) * tauHyp));
            tau += dTau;
            if (keel::abs(dTau) < 1e-13) break;
        }

        keel::f64 lonDeg = spec.centralMeridianDeg + keel::rad_to_deg(dLam);
        while (lonDeg > 180.0) lonDeg -= 360.0;
        while (lonDeg < -180.0) lonDeg += 360.0;

        const GeoCoord localGeo{
            keel::clamp(keel::rad_to_deg(keel::atan(tau)), -89.999999, 89.999999),
            ClampLon(lonDeg)
        };
        return TransformDatumHelmert(
            localGeo, spec.ellipsoid, EllipsoidSpec::Wgs84(), spec.datumToWgs84, /*inverse=*/false
        );
    }

    /// Forward WGS84 Polar Stereographic (Variant B, standard parallel +-71°):
    /// `northPole == true` -> EPSG:3995 (Arctic, 71°N, CM 0°E); `false` -> EPSG:3031 (Antarctic, 71°S, CM 0°E).
    [[nodiscard]] static GeoPlanar PolarStereographicForward(
        GeoCoord wgs84,
        bool northPole = true
    ) noexcept {
        constexpr keel::f64 a = kEarthRadiusMeters;
        constexpr keel::f64 e = kWgs84Eccentricity;
        constexpr keel::f64 eSq = e * e;
        constexpr keel::f64 phiC = keel::deg_to_rad(71.0);

        const keel::f64 sinPhiC = keel::sin(phiC);
        const keel::f64 mC = keel::cos(phiC) / keel::sqrt(1.0 - eSq * sinPhiC * sinPhiC);
        const keel::f64 tC = keel::tan(0.25 * kPi - 0.5 * phiC) /
                          keel::pow((1.0 - e * sinPhiC) / (1.0 + e * sinPhiC), 0.5 * e);

        const keel::f64 sign = northPole ? 1.0 : -1.0;
        const keel::f64 phi = keel::deg_to_rad(keel::clamp(sign * wgs84.lat, -30.0, 89.999999));
        const keel::f64 lam = keel::deg_to_rad(sign * wgs84.lon);

        const keel::f64 sinPhi = keel::sin(phi);
        const keel::f64 t = keel::tan(0.25 * kPi - 0.5 * phi) /
                         keel::pow((1.0 - e * sinPhi) / (1.0 + e * sinPhi), 0.5 * e);
        const keel::f64 rho = (a * mC * t) / tC;

        const keel::f64 x = sign * rho * keel::sin(lam);
        const keel::f64 y = -sign * rho * keel::cos(lam);
        return {x, y};
    }

    /// Inverse WGS84 Polar Stereographic (Variant B, EPSG:3995 / EPSG:3031):
    /// Converts metric (x, y) back to WGS84 (lat, lon) degrees.
    [[nodiscard]] static GeoCoord PolarStereographicInverse(
        keel::f64 x,
        keel::f64 y,
        bool northPole = true
    ) noexcept {
        constexpr keel::f64 a = kEarthRadiusMeters;
        constexpr keel::f64 e = kWgs84Eccentricity;
        constexpr keel::f64 eSq = e * e;
        constexpr keel::f64 phiC = keel::deg_to_rad(71.0);

        const keel::f64 sinPhiC = keel::sin(phiC);
        const keel::f64 mC = keel::cos(phiC) / keel::sqrt(1.0 - eSq * sinPhiC * sinPhiC);
        const keel::f64 tC = keel::tan(0.25 * kPi - 0.5 * phiC) /
                          keel::pow((1.0 - e * sinPhiC) / (1.0 + e * sinPhiC), 0.5 * e);

        const keel::f64 sign = northPole ? 1.0 : -1.0;
        const keel::f64 sx = sign * x;
        const keel::f64 sy = -sign * y;
        const keel::f64 rho = keel::hypot(sx, sy);
        if (rho < 1e-9) {
            return {sign * 90.0, 0.0};
        }

        const keel::f64 t = (rho * tC) / (a * mC);
        keel::f64 phi = 0.5 * kPi - 2.0 * keel::atan(t);
        for (keel::i32 iter = 0; iter < 6; ++iter) {
            const keel::f64 eSin = e * keel::sin(phi);
            const keel::f64 cf = keel::pow((1.0 - eSin) / (1.0 + eSin), 0.5 * e);
            phi = 0.5 * kPi - 2.0 * keel::atan(t * cf);
        }

        const keel::f64 lat = sign * keel::rad_to_deg(phi);
        const keel::f64 lon = sign * keel::rad_to_deg(keel::atan2(sx, sy));
        return {keel::clamp(lat, -89.999999, 89.999999), ClampLon(lon)};
    }

    /// Returns the TransverseMercatorSpec for a given MapCrs preset.
    [[nodiscard]] static constexpr TransverseMercatorSpec GetTransverseSpecForMapCrs(MapCrs crs) noexcept {
        if (crs == MapCrs::EPSG20906_Gsk2011Zone6) {
            return {
                .ellipsoid = EllipsoidSpec::Gsk2011(),
                .datumToWgs84 = Helmert7Spec::Identity(),
                .centralMeridianDeg = 33.0,
                .scaleFactor = 1.0,
                .falseEasting = 6500000.0,
                .falseNorthing = 0.0
            };
        }
        if (crs == MapCrs::EPSG28406_Pulkovo42Zone6) {
            return {
                .ellipsoid = EllipsoidSpec::Krassowsky1940(),
                .datumToWgs84 = Helmert7Spec::Sk42ToWgs84(),
                .centralMeridianDeg = 33.0,
                .scaleFactor = 1.0,
                .falseEasting = 6500000.0,
                .falseNorthing = 0.0
            };
        }
        // Default: EPSG:32636 (WGS84 / UTM Zone 36N, CM = 33°E)
        return {
            .ellipsoid = EllipsoidSpec::Wgs84(),
            .datumToWgs84 = Helmert7Spec::Identity(),
            .centralMeridianDeg = 33.0,
            .scaleFactor = 0.9996,
            .falseEasting = 500000.0,
            .falseNorthing = 0.0
        };
    }

    /// Projects geographic coordinates (lat, lon) to double-precision normalized [0..1]x[0..1] map coordinates.
    [[nodiscard]] static GeoPlanar GeoToNormalizedDouble(
        GeoCoord geo,
        MapCrs crs = MapCrs::EPSG3857_WebMercator
    ) noexcept {
        if (crs == MapCrs::EPSG32636_Utm36N ||
            crs == MapCrs::EPSG20906_Gsk2011Zone6 ||
            crs == MapCrs::EPSG28406_Pulkovo42Zone6) {
            const TransverseMercatorSpec spec = GetTransverseSpecForMapCrs(crs);
            const auto [easting, northing] = TransverseMercatorForward(geo, spec);
            const keel::f64 u = 0.5 + (easting - spec.falseEasting) / kEarthCircumferenceMeters;
            const keel::f64 v = 0.5 - (northing - spec.falseNorthing) / kEarthCircumferenceMeters;
            return {keel::clamp(u, 0.0, 1.0), keel::clamp(v, 0.0, 1.0)};
        }
        if (crs == MapCrs::EPSG3995_ArcticPolar) {
            const auto [px, py] = PolarStereographicForward(geo, /*northPole=*/true);
            const keel::f64 u = 0.5 + px / kEarthCircumferenceMeters;
            const keel::f64 v = 0.5 - py / kEarthCircumferenceMeters;
            return {keel::clamp(u, 0.0, 1.0), keel::clamp(v, 0.0, 1.0)};
        }

        const keel::f64 lon = ClampLon(geo.lon);
        const keel::f64 lat = ClampLat(geo.lat, crs);
        const keel::f64 x = (lon + 180.0) / 360.0;

        keel::f64 y = 0.5;
        if (crs == MapCrs::EPSG4326_PlateCarree) {
            y = (90.0 - lat) / 180.0;
        } else if (crs == MapCrs::EPSG3395_WorldMercator) {
            const keel::f64 phi = keel::deg_to_rad(lat);
            const keel::f64 eSin = kWgs84Eccentricity * keel::sin(phi);
            const keel::f64 conformalFactor = keel::pow((1.0 - eSin) / (1.0 + eSin), 0.5 * kWgs84Eccentricity);
            const keel::f64 mercY = keel::log(keel::tan(0.25 * kPi + 0.5 * phi) * conformalFactor);
            y = (1.0 - mercY / kPi) * 0.5;
        } else {
            const keel::f64 phi = keel::deg_to_rad(lat);
            const keel::f64 mercY = keel::log(keel::tan(0.25 * kPi + 0.5 * phi));
            y = (1.0 - mercY / kPi) * 0.5;
        }

        return {keel::clamp(x, 0.0, 1.0), keel::clamp(y, 0.0, 1.0)};
    }

    /// Projects geographic coordinates (lat, lon) to normalized [0..1]x[0..1] map coordinates.
    /// Origin (0,0) is Top-Left (North-West), (1,1) is Bottom-Right (South-East).
    [[nodiscard]] static Vec2 GeoToNormalized(GeoCoord geo, MapCrs crs = MapCrs::EPSG3857_WebMercator) noexcept {
        const auto [u, v] = GeoToNormalizedDouble(geo, crs);
        return {keel::as<keel::f32>(u), keel::as<keel::f32>(v)};
    }

    /// Unprojects normalized [0..1]x[0..1] map coordinates back to geographic (lat, lon) in degrees.
    [[nodiscard]] static GeoCoord NormalizedToGeo(Vec2 norm, MapCrs crs = MapCrs::EPSG3857_WebMercator) noexcept {
        const keel::f64 nx = keel::clamp(keel::as<keel::f64>(norm.x), 0.0, 1.0);
        const keel::f64 ny = keel::clamp(keel::as<keel::f64>(norm.y), 0.0, 1.0);

        if (crs == MapCrs::EPSG32636_Utm36N ||
            crs == MapCrs::EPSG20906_Gsk2011Zone6 ||
            crs == MapCrs::EPSG28406_Pulkovo42Zone6) {
            const TransverseMercatorSpec spec = GetTransverseSpecForMapCrs(crs);
            const keel::f64 easting  = spec.falseEasting  + (nx - 0.5) * kEarthCircumferenceMeters;
            const keel::f64 northing = spec.falseNorthing + (0.5 - ny) * kEarthCircumferenceMeters;
            return TransverseMercatorInverse(easting, northing, spec);
        }
        if (crs == MapCrs::EPSG3995_ArcticPolar) {
            const keel::f64 px = (nx - 0.5) * kEarthCircumferenceMeters;
            const keel::f64 py = (0.5 - ny) * kEarthCircumferenceMeters;
            return PolarStereographicInverse(px, py, /*northPole=*/true);
        }

        const keel::f64 lon = nx * 360.0 - 180.0;

        keel::f64 lat = 0.0;
        if (crs == MapCrs::EPSG4326_PlateCarree) {
            lat = 90.0 - ny * 180.0;
        } else if (crs == MapCrs::EPSG3395_WorldMercator) {
            const keel::f64 mercY = kPi * (1.0 - 2.0 * ny);
            const keel::f64 t = keel::exp(-mercY);
            keel::f64 phi = 0.5 * kPi - 2.0 * keel::atan(t);
            for (keel::i32 iter = 0; iter < 6; ++iter) {
                const keel::f64 eSin = kWgs84Eccentricity * keel::sin(phi);
                const keel::f64 cf = keel::pow((1.0 - eSin) / (1.0 + eSin), 0.5 * kWgs84Eccentricity);
                phi = 0.5 * kPi - 2.0 * keel::atan(t * cf);
            }
            lat = keel::rad_to_deg(phi);
        } else {
            const keel::f64 n = kPi * (1.0 - 2.0 * ny);
            lat = keel::rad_to_deg(keel::atan(keel::sinh(n)));
        }

        return {ClampLat(lat, crs), ClampLon(lon)};
    }

    /// Converts geographic coordinates (lat, lon) to Scene World Coordinates (0..worldSize).
    [[nodiscard]] static Vec2 GeoToWorld(GeoCoord geo, keel::f32 worldSize, MapCrs crs = MapCrs::EPSG3857_WebMercator) noexcept {
        const Vec2 n = GeoToNormalized(geo, crs);
        return {n.x * worldSize, n.y * worldSize};
    }

    /// Converts Scene World Coordinates (0..worldSize) to geographic coordinates (lat, lon).
    [[nodiscard]] static GeoCoord WorldToGeo(Vec2 worldPos, keel::f32 worldSize, MapCrs crs = MapCrs::EPSG3857_WebMercator) noexcept {
        const keel::f32 inv = (worldSize > 0.0f) ? (1.0f / worldSize) : 1.0f;
        return NormalizedToGeo({worldPos.x * inv, worldPos.y * inv}, crs);
    }

    /// Converts a geographic Coordinate into fractional slippy tile coordinates at zoom level z.
    [[nodiscard]] static Vec2 GeoToTileFraction(GeoCoord geo, keel::i32 z, MapCrs crs = MapCrs::EPSG3857_WebMercator) noexcept {
        const keel::i32 clampedZ = keel::clamp(z, 0, 22);
        const keel::f32 n = keel::as<keel::f32>(1 << clampedZ);
        const Vec2 norm = GeoToNormalized(geo, crs);
        return {norm.x * n, norm.y * n};
    }

    /// Converts a geographic coordinate into integer [z/x/y] tile indices.
    [[nodiscard]] static OsmTileId GeoToTileId(GeoCoord geo, keel::i32 z, MapCrs crs = MapCrs::EPSG3857_WebMercator) noexcept {
        const keel::i32 clampedZ = keel::clamp(z, 0, 22);
        const keel::i32 n = 1 << clampedZ;
        const Vec2 f = GeoToTileFraction(geo, clampedZ, crs);
        const keel::i32 tx = keel::clamp(keel::floor_i32(f.x), 0, n - 1);
        const keel::i32 ty = keel::clamp(keel::floor_i32(f.y), 0, n - 1);
        return {clampedZ, tx, ty};
    }

    /// Returns the Scene world-space rectangle occupied by tile [z/x/y].
    [[nodiscard]] static Rect TileToWorldRect(OsmTileId tile, keel::f32 worldSize) noexcept {
        const keel::i32 clampedZ = keel::clamp(tile.z, 0, 22);
        const keel::f32 n = keel::as<keel::f32>(1 << clampedZ);
        const keel::f32 tileWorldW = worldSize / n;
        return Rect(
            keel::as<keel::f32>(tile.x) * tileWorldW,
            keel::as<keel::f32>(tile.y) * tileWorldW,
            tileWorldW,
            tileWorldW
        );
    }

    /// Returns the geographic NW and SE bounds of tile [z/x/y] (evaluating all 4 corners + center for rotated/polar CRS).
    [[nodiscard]] static GeoBounds TileToGeoBounds(OsmTileId tile, MapCrs crs = MapCrs::EPSG3857_WebMercator) noexcept {
        const keel::i32 clampedZ = keel::clamp(tile.z, 0, 22);
        const keel::f32 invN = 1.0f / keel::as<keel::f32>(1 << clampedZ);
        const keel::f32 x0 = keel::as<keel::f32>(tile.x) * invN;
        const keel::f32 y0 = keel::as<keel::f32>(tile.y) * invN;
        const keel::f32 x1 = keel::as<keel::f32>(tile.x + 1) * invN;
        const keel::f32 y1 = keel::as<keel::f32>(tile.y + 1) * invN;

        const GeoCoord c00 = NormalizedToGeo({x0, y0}, crs);
        const GeoCoord c11 = NormalizedToGeo({x1, y1}, crs);
        if (IsCylindricalCrs(crs)) {
            return {c00, c11};
        }
        const GeoCoord c10 = NormalizedToGeo({x1, y0}, crs);
        const GeoCoord c01 = NormalizedToGeo({x0, y1}, crs);
        const GeoCoord cMid = NormalizedToGeo({0.5f * (x0 + x1), 0.5f * (y0 + y1)}, crs);
        const keel::f64 maxLat = Max5(c00.lat, c10.lat, c01.lat, c11.lat, cMid.lat);
        const keel::f64 minLat = Min5(c00.lat, c10.lat, c01.lat, c11.lat, cMid.lat);
        const keel::f64 minLon = Min5(c00.lon, c10.lon, c01.lon, c11.lon, cMid.lon);
        const keel::f64 maxLon = Max5(c00.lon, c10.lon, c01.lon, c11.lon, cMid.lon);
        return {{maxLat, minLon}, {minLat, maxLon}};
    }

    /// Computes the Scene zoom factor required to achieve a target continuous tile zoom level.
    [[nodiscard]] static keel::f32 TileZoomToSceneZoom(keel::f32 targetTileZoom, keel::i32 baseZoom = 3) noexcept {
        return keel::pow(2.0f, targetTileZoom - keel::as<keel::f32>(baseZoom));
    }

    /// Computes ground resolution in meters per screen pixel at a given latitude and continuous tile zoom.
    /// At standard 256px tile size, zoom 0 has 256 pixels across 40,075,016.686 meters at the equator (or planar extent).
    [[nodiscard]] static keel::f64 MetersPerPixel(
        keel::f64 latDeg,
        keel::f32 continuousZoom,
        keel::f32 screenTileSize = 256.0f,
        MapCrs crs = MapCrs::EPSG3857_WebMercator
    ) noexcept {
        const keel::f64 worldPixels = keel::as<keel::f64>(screenTileSize) * keel::pow(2.0, keel::as<keel::f64>(continuousZoom));
        if (!IsCylindricalCrs(crs)) {
            return keel::max(0.0001, kEarthCircumferenceMeters / worldPixels);
        }
        const keel::f64 cosLat = keel::cos(keel::deg_to_rad(ClampLat(latDeg)));
        return keel::max(0.0001, (kEarthCircumferenceMeters * cosLat) / worldPixels);
    }

    /// Selects an optimal nautical graticule spacing in arc-minutes (1' = 1 Nautical Mile)
    /// so that grid lines are spaced roughly `targetPixelSpacing` pixels apart on screen.
    [[nodiscard]] static keel::f64 SelectNauticalGraticuleStepArcMin(
        keel::f64 visibleLonSpanDeg,
        keel::f32 viewportWidthPx,
        keel::f32 targetPixelSpacing = 110.0f
    ) noexcept {
        static constexpr keel::StaticArray<keel::f64, 16> kStepsArcMin = {
            1800.0, // 30°
            1200.0, // 20°
            600.0,  // 10°
            300.0,  // 5°
            120.0,  // 2°
            60.0,   // 1° (60 NM)
            30.0,   // 30' (30 NM)
            15.0,   // 15' (15 NM)
            10.0,   // 10' (10 NM)
            5.0,    // 5' (5 NM)
            2.0,    // 2' (2 NM)
            1.0,    // 1' (1 Nautical Mile)
            0.5,    // 0.5' (5 cables)
            0.2,    // 0.2' (2 cables)
            0.1,    // 0.1' (1 cable = 185.2 m)
            0.05    // 0.05'
        };
        if (viewportWidthPx <= 1.0f || visibleLonSpanDeg <= 0.0) {
            return 60.0;
        }
        const keel::f64 desiredDeg = visibleLonSpanDeg * (keel::as<keel::f64>(targetPixelSpacing) / keel::as<keel::f64>(viewportWidthPx));
        const keel::f64 desiredArcMin = desiredDeg * 60.0;

        keel::f64 best = kStepsArcMin.front();
        keel::f64 bestDiff = keel::abs(best - desiredArcMin);
        for (keel::f64 step : kStepsArcMin) {
            const keel::f64 diff = keel::abs(step - desiredArcMin);
            if (diff < bestDiff) {
                bestDiff = diff;
                best = step;
            }
        }
        return best;
    }

    /// Computes a clean 1-2-5 physical scale bar in meters/kilometers.
    [[nodiscard]] static MapScaleBar ComputeMetricScaleBar(keel::f64 metersPerPixel, keel::f32 maxBarPixels = 120.0f) noexcept {
        MapScaleBar bar{};
        if (metersPerPixel <= 0.0 || maxBarPixels <= 10.0f) return bar;

        const keel::f64 maxMeters = metersPerPixel * keel::as<keel::f64>(maxBarPixels);
        const keel::f64 exp10 = keel::pow(10.0, keel::floor(keel::log10(keel::max(1.0, maxMeters))));
        keel::f64 niceMeters = exp10;
        if (5.0 * exp10 <= maxMeters) {
            niceMeters = 5.0 * exp10;
        } else if (2.0 * exp10 <= maxMeters) {
            niceMeters = 2.0 * exp10;
        }

        bar.distanceValue = niceMeters;
        bar.screenPixels = keel::as<keel::f32>(niceMeters / metersPerPixel);
        if (niceMeters >= 1000.0) {
            keel::format_buf(bar.label, sizeof(bar.label), "%.0f km", niceMeters / 1000.0);
        } else {
            keel::format_buf(bar.label, sizeof(bar.label), "%.0f m", niceMeters);
        }
        return bar;
    }

    /// Computes a clean 1-2-5 nautical scale bar in Nautical Miles (NM) or Cables (cab, 0.1 NM = 185.2 m).
    [[nodiscard]] static MapScaleBar ComputeNauticalScaleBar(keel::f64 metersPerPixel, keel::f32 maxBarPixels = 120.0f) noexcept {
        MapScaleBar bar{};
        if (metersPerPixel <= 0.0 || maxBarPixels <= 10.0f) return bar;

        const keel::f64 nmPerPixel = metersPerPixel / kMetersPerNauticalMile;
        const keel::f64 maxNm = nmPerPixel * keel::as<keel::f64>(maxBarPixels);

        if (maxNm >= 1.0) {
            const keel::f64 exp10 = keel::pow(10.0, keel::floor(keel::log10(maxNm)));
            keel::f64 niceNm = exp10;
            if (5.0 * exp10 <= maxNm) {
                niceNm = 5.0 * exp10;
            } else if (2.0 * exp10 <= maxNm) {
                niceNm = 2.0 * exp10;
            }
            bar.distanceValue = niceNm;
            bar.screenPixels = keel::as<keel::f32>((niceNm * kMetersPerNauticalMile) / metersPerPixel);
            keel::format_buf(bar.label, sizeof(bar.label), "%.0f NM", niceNm);
        } else {
            // Sub-mile nautical scale in cables (1 NM = 10 cables = 1852 m)
            const keel::f64 maxCables = keel::max(0.1, maxNm * 10.0);
            keel::f64 niceCables = 0.5;
            if (maxCables >= 5.0) niceCables = 5.0;
            else if (maxCables >= 2.0) niceCables = 2.0;
            else if (maxCables >= 1.0) niceCables = 1.0;

            bar.distanceValue = niceCables * 0.1;
            bar.screenPixels = keel::as<keel::f32>((bar.distanceValue * kMetersPerNauticalMile) / metersPerPixel);
            keel::format_buf(bar.label, sizeof(bar.label), "%.1f NM (%.0f cab)", bar.distanceValue, niceCables);
        }
        return bar;
    }

    /// Computes the full visible tile grid, nautical graticule, geographic bounds, and scale bars for the current Scene view.
    [[nodiscard]] static MapViewState ComputeView(
        Rect visibleWorldRect,
        Rect viewportScreenRect,
        keel::f32 sceneZoom,
        Vec2 mouseWorldPos,
        const MapGridConfig& cfg = {}
    ) {
        MapViewState out{};
        out.crs = cfg.crs;

        const keel::f32 safeWorld = keel::max(1.0f, cfg.worldSize);
        const keel::f32 safeZoom = keel::max(0.0001f, sceneZoom);

        // 1. Continuous and integer tile pyramid zoom level
        out.continuousZoom = keel::as<keel::f32>(cfg.baseZoom) + keel::log2(safeZoom) + cfg.zoomBias;
        out.tileZoom = keel::clamp(
            keel::floor_i32(out.continuousZoom + 0.5f),
            cfg.minTileZoom,
            cfg.maxTileZoom
        );
        out.totalTilesPerAxis = 1 << out.tileZoom;

        // 2. Clamp visible world rect to [0..worldSize]
        const keel::f32 vx0 = keel::clamp(visibleWorldRect.x, 0.0f, safeWorld);
        const keel::f32 vy0 = keel::clamp(visibleWorldRect.y, 0.0f, safeWorld);
        const keel::f32 vx1 = keel::clamp(visibleWorldRect.Right(), 0.0f, safeWorld);
        const keel::f32 vy1 = keel::clamp(visibleWorldRect.Bottom(), 0.0f, safeWorld);

        const Vec2 worldCenter{
            visibleWorldRect.x + visibleWorldRect.w * 0.5f,
            visibleWorldRect.y + visibleWorldRect.h * 0.5f
        };
        out.center = WorldToGeo(worldCenter, safeWorld, cfg.crs);
        out.cursor = WorldToGeo(mouseWorldPos, safeWorld, cfg.crs);
        if (IsCylindricalCrs(cfg.crs)) {
            out.visibleBounds.nw = WorldToGeo({vx0, vy0}, safeWorld, cfg.crs);
            out.visibleBounds.se = WorldToGeo({vx1, vy1}, safeWorld, cfg.crs);
        } else {
            const GeoCoord c00 = WorldToGeo({vx0, vy0}, safeWorld, cfg.crs);
            const GeoCoord c10 = WorldToGeo({vx1, vy0}, safeWorld, cfg.crs);
            const GeoCoord c01 = WorldToGeo({vx0, vy1}, safeWorld, cfg.crs);
            const GeoCoord c11 = WorldToGeo({vx1, vy1}, safeWorld, cfg.crs);
            out.visibleBounds.nw.lat = Max5(c00.lat, c10.lat, c01.lat, c11.lat, out.center.lat);
            out.visibleBounds.se.lat = Min5(c00.lat, c10.lat, c01.lat, c11.lat, out.center.lat);
            out.visibleBounds.nw.lon = Min5(c00.lon, c10.lon, c01.lon, c11.lon, out.center.lon);
            out.visibleBounds.se.lon = Max5(c00.lon, c10.lon, c01.lon, c11.lon, out.center.lon);
        }

        // 3. Visible [z/x/y] tile range & frustum culling
        const keel::f32 nF = keel::as<keel::f32>(out.totalTilesPerAxis);
        const keel::f32 tileWorldW = safeWorld / nF;

        out.minTileX = keel::clamp(keel::floor_i32(vx0 / tileWorldW), 0, out.totalTilesPerAxis - 1);
        out.maxTileX = keel::clamp(
            keel::floor_i32(keel::max(vx0, vx1 - 1e-4f) / tileWorldW), 0, out.totalTilesPerAxis - 1
        );
        out.minTileY = keel::clamp(keel::floor_i32(vy0 / tileWorldW), 0, out.totalTilesPerAxis - 1);
        out.maxTileY = keel::clamp(
            keel::floor_i32(keel::max(vy0, vy1 - 1e-4f) / tileWorldW), 0, out.totalTilesPerAxis - 1
        );

        const keel::i32 countX = keel::max(0, out.maxTileX - out.minTileX + 1);
        const keel::i32 countY = keel::max(0, out.maxTileY - out.minTileY + 1);
        out.visibleTileCount = countX * countY;
        out.visibleTiles.reserve(keel::as<keel::usize>(out.visibleTileCount));

        for (keel::i32 ty = out.minTileY; ty <= out.maxTileY; ++ty) {
            for (keel::i32 tx = out.minTileX; tx <= out.maxTileX; ++tx) {
                const OsmTileId id{out.tileZoom, tx, ty};
                out.visibleTiles.push_back(OsmTileCell{
                    .id = id,
                    .worldRect = TileToWorldRect(id, safeWorld),
                    .geoBounds = TileToGeoBounds(id, cfg.crs)
                });
            }
        }

        // 4. Physical ground resolution & Metric / Nautical scale bars
        // Screen size of a base tile is (safeWorld / 2^baseZoom) * sceneZoom
        const keel::f32 baseTileWorld = safeWorld / keel::as<keel::f32>(1 << cfg.baseZoom);
        const keel::f64 effContinuousZ = keel::as<keel::f64>(cfg.baseZoom) + keel::log2(keel::as<keel::f64>(safeZoom));
        out.metersPerPixel = MetersPerPixel(out.center.lat, keel::as<keel::f32>(effContinuousZ), baseTileWorld, cfg.crs);
        out.metricScale = ComputeMetricScaleBar(out.metersPerPixel, 120.0f);
        out.nauticalScale = ComputeNauticalScaleBar(out.metersPerPixel, 120.0f);

        // 5. OpenSeaMap Nautical Graticule (meridians & parallels in degrees and nautical arc-minutes)
        // Use un-clamped visibleWorldRect.w so panning near map boundaries never alters the graticule step!
        const keel::f64 unClampedLonSpan = IsCylindricalCrs(cfg.crs)
            ? ((visibleWorldRect.w > 0.0f)
                ? (keel::as<keel::f64>(visibleWorldRect.w) / keel::as<keel::f64>(safeWorld)) * 360.0
                : (360.0 / keel::as<keel::f64>(safeZoom)))
            : keel::max(0.0001, out.visibleBounds.WidthLon());
        out.graticuleStepArcMin = SelectNauticalGraticuleStepArcMin(
            keel::max(0.0001, unClampedLonSpan),
            keel::max(120.0f, viewportScreenRect.w),
            115.0f
        );
        const keel::f64 stepDeg = out.graticuleStepArcMin / 60.0;

        if (stepDeg > 0.0) {
            // Meridians (West -> East), padded by 1 step so border ruler zebra bars clip smoothly at edges
            const keel::i32 iLonStart = keel::floor_i32(out.visibleBounds.nw.lon / stepDeg) - 1;
            const keel::i32 iLonEnd   = keel::ceil_i32(out.visibleBounds.se.lon / stepDeg) + 1;
            for (keel::i32 idx = iLonStart; idx <= iLonEnd; ++idx) {
                const keel::f64 lon = keel::as<keel::f64>(idx) * stepDeg;
                if (lon < -180.0 - 1e-6 || lon > 180.0 + 1e-6) continue;
                const keel::f32 wx = GeoToWorld({out.center.lat, lon}, safeWorld, cfg.crs).x;
                const bool isZero = (idx == 0);
                const bool isMajor = isZero || (idx % 5 == 0) || (keel::abs(keel::fmod(lon, 1.0)) < 1e-5 && out.graticuleStepArcMin < 60.0);
                out.meridians.push_back(GraticuleLine{
                    .degrees = lon,
                    .worldPos = wx,
                    .stepIndex = idx,
                    .isMajor = isMajor,
                    .isZero = isZero
                });
            }

            // Parallels (South -> North), padded by 1 step so border ruler zebra bars clip smoothly at edges
            const keel::i32 iLatStart = keel::floor_i32(out.visibleBounds.se.lat / stepDeg) - 1;
            const keel::i32 iLatEnd   = keel::ceil_i32(out.visibleBounds.nw.lat / stepDeg) + 1;
            for (keel::i32 idx = iLatStart; idx <= iLatEnd; ++idx) {
                const keel::f64 lat = keel::as<keel::f64>(idx) * stepDeg;
                const keel::f64 maxL = IsCylindricalCrs(cfg.crs)
                    ? ((cfg.crs == MapCrs::EPSG4326_PlateCarree) ? 89.9 : kMaxMercatorLat)
                    : 89.9;
                if (lat < -maxL || lat > maxL) continue;
                const keel::f32 wy = GeoToWorld({lat, out.center.lon}, safeWorld, cfg.crs).y;
                const bool isZero = (idx == 0);
                const bool isMajor = isZero || (idx % 5 == 0) || (keel::abs(keel::fmod(lat, 1.0)) < 1e-5 && out.graticuleStepArcMin < 60.0);
                out.parallels.push_back(GraticuleLine{
                    .degrees = lat,
                    .worldPos = wy,
                    .stepIndex = idx,
                    .isMajor = isMajor,
                    .isZero = isZero
                });
            }
        }

        return out;
    }

    /// Formats a single latitude or longitude value according to the selected maritime/cartographic format.
    [[nodiscard]] static keel::FixedString<48> FormatAxis(keel::f64 deg, bool isLat, GeoCoordFormat fmt = GeoCoordFormat::NauticalDMM) noexcept {
        const char hemi = isLat ? (deg >= 0.0 ? 'N' : 'S') : (deg >= 0.0 ? 'E' : 'W');
        const keel::f64 absDeg = keel::abs(deg);

        if (fmt == GeoCoordFormat::DecimalDegrees) {
            return keel::format_fixed<48>(isLat ? "%07.4f° %c" : "%08.4f° %c", absDeg, hemi);
        }
        if (fmt == GeoCoordFormat::DMS) {
            keel::i32 d = keel::floor_i32(absDeg);
            keel::f64 remMin = (absDeg - keel::as<keel::f64>(d)) * 60.0;
            keel::i32 m = keel::floor_i32(remMin);
            keel::f64 s = (remMin - keel::as<keel::f64>(m)) * 60.0;
            if (s >= 59.95) { s = 0.0; ++m; }
            if (m >= 60) { m = 0; ++d; }
            return keel::format_fixed<48>(isLat ? "%02d° %02d' %04.1f\" %c" : "%03d° %02d' %04.1f\" %c", d, m, s, hemi);
        }
        // Standard OpenSeaMap / Marine GPS Degrees & Decimal Minutes (DD° MM.mm')
        keel::i32 d = keel::floor_i32(absDeg);
        keel::f64 m = (absDeg - keel::as<keel::f64>(d)) * 60.0;
        if (m >= 59.995) { m = 0.0; ++d; }
        return keel::format_fixed<48>(isLat ? "%02d° %05.2f' %c" : "%03d° %05.2f' %c", d, m, hemi);
    }

    /// Formats a short graticule ruler label (e.g. "54° 15.0' N") for border ticks.
    [[nodiscard]] static keel::FixedString<32> FormatGraticuleTick(keel::f64 deg, bool isLat, keel::f64 stepArcMin) noexcept {
        const char hemi = isLat ? (deg >= 0.0 ? 'N' : 'S') : (deg >= 0.0 ? 'E' : 'W');
        const keel::f64 absDeg = keel::abs(deg);
        if (stepArcMin >= 60.0) {
            return keel::format_fixed<32>("%.0f° %c", absDeg, hemi);
        }
        if (stepArcMin >= 1.0) {
            keel::i32 d = keel::floor_i32(absDeg + 1e-7);
            keel::f64 m = (absDeg - keel::as<keel::f64>(d)) * 60.0;
            if (m >= 59.95) { m = 0.0; ++d; }
            return keel::format_fixed<32>("%d° %02.0f' %c", d, m, hemi);
        }
        keel::i32 d = keel::floor_i32(absDeg + 1e-7);
        keel::f64 m = (absDeg - keel::as<keel::f64>(d)) * 60.0;
        if (m >= 59.995) { m = 0.0; ++d; }
        return keel::format_fixed<32>("%d° %04.1f' %c", d, m, hemi);
    }

    /// Formats a full (lat, lon) or planar metric (UTM / GOST Gauss-Kruger) coordinate string.
    [[nodiscard]] static keel::FixedString<96> FormatCoord(
        GeoCoord c,
        GeoCoordFormat fmt = GeoCoordFormat::NauticalDMM,
        MapCrs activeCrs = MapCrs::EPSG3857_WebMercator
    ) noexcept {
        if (fmt == GeoCoordFormat::UTM_Meters) {
            const keel::i32 zone = (activeCrs == MapCrs::EPSG32636_Utm36N)
                ? 36
                : keel::clamp(keel::floor_i32((ClampLon(c.lon) + 180.0) / 6.0) + 1, 1, 60);
            const bool north = (c.lat >= 0.0);
            const TransverseMercatorSpec spec{
                .ellipsoid = EllipsoidSpec::Wgs84(),
                .datumToWgs84 = Helmert7Spec::Identity(),
                .centralMeridianDeg = keel::as<keel::f64>(zone * 6 - 183),
                .scaleFactor = 0.9996,
                .falseEasting = 500000.0,
                .falseNorthing = north ? 0.0 : 10000000.0
            };
            const auto [easting, northing] = TransverseMercatorForward(c, spec);
            return keel::format_fixed<96>("UTM %02d%c  E %.1f m, N %.1f m",
                zone, north ? 'N' : 'S', easting, northing);
        }
        if (fmt == GeoCoordFormat::GaussKruger_Meters) {
            keel::f64 posLon = ClampLon(c.lon);
            if (posLon < 0.0) posLon += 360.0;
            const keel::i32 zone = (activeCrs == MapCrs::EPSG20906_Gsk2011Zone6 || activeCrs == MapCrs::EPSG28406_Pulkovo42Zone6)
                ? 6
                : keel::clamp(keel::floor_i32(posLon / 6.0) + 1, 1, 60);
            const bool useSk42 = (activeCrs == MapCrs::EPSG28406_Pulkovo42Zone6);
            const TransverseMercatorSpec spec{
                .ellipsoid = useSk42 ? EllipsoidSpec::Krassowsky1940() : EllipsoidSpec::Gsk2011(),
                .datumToWgs84 = useSk42 ? Helmert7Spec::Sk42ToWgs84() : Helmert7Spec::Identity(),
                .centralMeridianDeg = keel::as<keel::f64>(zone * 6 - 3),
                .scaleFactor = 1.0,
                .falseEasting = keel::as<keel::f64>(zone) * 1000000.0 + 500000.0,
                .falseNorthing = 0.0
            };
            const auto [yEast, xNorth] = TransverseMercatorForward(c, spec);
            return keel::format_fixed<96>("%s Z%d  X %.1f m, Y %.1f m",
                useSk42 ? "SK-42" : "GSK-2011", zone, xNorth, yEast);
        }
        const auto latAxis = FormatAxis(c.lat, true, fmt);
        const auto lonAxis = FormatAxis(c.lon, false, fmt);
        return keel::format_fixed<96>("%s, %s", latAxis, lonAxis);
    }

private:
    [[nodiscard]] static constexpr keel::f64 Max5(keel::f64 a, keel::f64 b, keel::f64 c, keel::f64 d, keel::f64 e) noexcept {
        return keel::max(a, keel::max(b, keel::max(c, keel::max(d, e))));
    }
    [[nodiscard]] static constexpr keel::f64 Min5(keel::f64 a, keel::f64 b, keel::f64 c, keel::f64 d, keel::f64 e) noexcept {
        return keel::min(a, keel::min(b, keel::min(c, keel::min(d, e))));
    }
};

} // namespace guikit::geo

namespace guikit {
using geo::EllipsoidSpec;
using geo::GeoBBox;
using geo::GeoBounds;
using geo::GeoCoord;
using geo::GeoCoordFormat;
using geo::GeoPlanar;
using geo::GraticuleLine;
using geo::Helmert7Spec;
using geo::MapCrs;
using geo::MapGrid;
using geo::MapGridConfig;
using geo::MapScaleBar;
using geo::MapViewState;
using geo::OsmTileCell;
using geo::OsmTileId;
using geo::TileSourceCallback;
using geo::TransverseMercatorSpec;
} // namespace guikit
