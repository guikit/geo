#pragma once

#include "guikit/backend/vector_backend.hpp"
#include "guikit/geo/gis_render_engine.hpp"
#include "keel/array.h"
#include "keel/atomic.h"
#include "keel/core.h"
#include "keel/hash.h"
#include "keel/hash_map.h"
#include "keel/hash_set.h"
#include "keel/smart_ptr.h"
#include "keel/string.h"
#include "keel/sync.h"
#include "keel/thread.h"

namespace guikit::geo::detail {

constexpr bool kEnableFileDebugLog = false;
constexpr keel::f32 kTileGuardBandPx = 8.0f;

[[nodiscard]] constexpr keel::u32 ResolveTileSizePx(keel::u32 reqSize) noexcept {
    if (reqSize <= 192u) return 128u;
    if (reqSize >= 384u) return 512u;
    return 256u;
}

[[nodiscard]] constexpr bool HasBackgroundWorkerThreads() noexcept {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    return false;
#else
    return true;
#endif
}

[[nodiscard]] constexpr keel::u32 PackColorABGR8888(guikit::Color c) noexcept {
    return (keel::as<keel::u32>(c.a) << 24) |
           (keel::as<keel::u32>(c.b) << 16) |
           (keel::as<keel::u32>(c.g) << 8)  |
           keel::as<keel::u32>(c.r);
}

inline void HashCombineBytes(keel::u64& h, const void* data, keel::usize len) noexcept {
    h = keel::fnv1a64_bytes(data, len, h);
}

template <typename T>
inline void HashCombineVal(keel::u64& h, const T& val) noexcept {
    HashCombineBytes(h, &val, sizeof(T));
}

[[maybe_unused]] inline void AppendRenderDebugLog(keel::StringRef line) noexcept {
    if constexpr (!kEnableFileDebugLog) {
        (void)line;
        return;
    }
}

// ============================================================================
// Shared Globals, Canvas Lifecycle & S-52 Styling / Tile Rasterization
//   Implemented in gis_s52_style.cpp
// ============================================================================

void EmitRenderBreadcrumb(const char* stage, keel::i32 z = -1, keel::i32 x = -1, keel::i32 y = -1) noexcept;
keel::Mutex& GetDefaultBasemapMutex() noexcept;
keel::Arc<IRasterTileProvider>& GetDefaultBasemapStorage();
keel::Arc<IRasterTileProvider>& GetLastActiveBasemapStorage();

[[nodiscard]] keel::UniquePtr<guikit::ICanvas> CreateCanvasThreadSafe();
void DestroyCanvasThreadSafe(keel::UniquePtr<guikit::ICanvas>& canvas) noexcept;

[[nodiscard]] bool IsCautionOrRestrictedArea(const geo::GeoFeature& feat) noexcept;
[[nodiscard]] guikit::Color WithAlpha(guikit::Color c, keel::f32 alphaFactor) noexcept;
[[nodiscard]] guikit::Color BrightenColor(guikit::Color c, keel::i32 delta) noexcept;
[[nodiscard]] guikit::Color LerpColor(guikit::Color a, guikit::Color b, keel::f32 t) noexcept;
[[nodiscard]] guikit::Color ScaleColorAlpha(guikit::Color c, keel::f32 alphaScale) noexcept;
[[nodiscard]] guikit::Color ResolveIalaBuoyColor(keel::StringRef lightOrColor, guikit::Color fallback) noexcept;

[[nodiscard]] bool ShouldSkipFeatureAtZoom(
    const geo::GeoFeature& feat,
    keel::i32 tileZoom,
    const GisRenderConfig& cfg
) noexcept;

struct ResolvedFeatureStyle {
    guikit::Color fillColor{0, 0, 0, 0};
    guikit::Color strokeColor{0, 0, 0, 0};
    keel::f32 strokeWidthPx{1.0f};
    bool renderFill{false};
    bool renderStroke{false};
};

[[nodiscard]] guikit::Color SampleBathymetryColor(keel::f32 depthM, const GisRenderConfig& cfg) noexcept;
[[nodiscard]] ResolvedFeatureStyle ResolveMarineFeatureStyle(
    const geo::GeoLayer& layer,
    const geo::GeoFeature& feat,
    const GisRenderConfig& cfg,
    bool bakeFillOpacity = false
) noexcept;
[[nodiscard]] keel::i32 CartographicPainterRank(const geo::GeoFeature& feat) noexcept;
[[nodiscard]] keel::i32 CartographicLayerRank(const geo::GeoLayer& layer) noexcept;
[[nodiscard]] keel::u64 ComputeLayerStyleHash(
    const geo::GeoLayer& lyr,
    const GisRenderConfig& cfg,
    guikit::MapCrs crs
) noexcept;

struct CellNorm3857Bounds {
    keel::f32 u0{0.0f};
    keel::f32 u1{0.0f};
    keel::f32 v0{0.0f};
    keel::f32 v1{0.0f};
    keel::f32 spanU{1e-7f};
    keel::f32 spanV{1e-7f};
};

[[nodiscard]] CellNorm3857Bounds ComputeCellNorm3857Bounds(
    const guikit::OsmTileCell& cell,
    guikit::MapCrs crs
) noexcept;

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
);

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
);

void BlendPremulTileOntoOpaqueComposite(
    keel::u32* __restrict dst,
    const keel::u32* __restrict src,
    keel::u8 layerOpacity,
    keel::u32 tileSizePx
) noexcept;

void BlendStraightRgbaBasemapOntoComposite(
    keel::u32* __restrict dst,
    const keel::u32* __restrict src,
    keel::u8 basemapOpacity,
    keel::u32 tileSizePx
) noexcept;

struct AsyncTileCandidate {
    keel::u32 featureIndex{0};
    keel::u64 featureUid{0};
    keel::i64 sqliteRowId{0};
    keel::i32 srsId{4326};
    geo::ChartFeatureClass chartClass{geo::ChartFeatureClass::Generic};
    keel::f32 maxDimPx{256.0f};
    ResolvedFeatureStyle style{};
    keel::Arc<const geo::DecodedGeometryPayload> payload;
};

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
);

void DrawLoadingTilePlaceholder(
    guikit::SceneScope& scene,
    const guikit::OsmTileCell& cell,
    keel::f32 zoom,
    keel::f32 invZ,
    bool darkChartSurface,
    bool hasPreview,
    keel::u64 frameCounter
);

// ============================================================================
// Tile Buffer Pool, Async Chart Streamer & Two-Tier Tile Cache
//   Implemented in gis_tile_cache.cpp
// ============================================================================

struct TileCoordKey {
    keel::i32 z{0};
    keel::i32 x{0};
    keel::i32 y{0};

    [[nodiscard]] bool operator==(const TileCoordKey& o) const noexcept {
        return z == o.z && x == o.x && y == o.y;
    }
};

struct TileCoordKeyHasher {
    [[nodiscard]] keel::usize operator()(const TileCoordKey& k) const noexcept {
        keel::u64 h = keel::as<keel::u64>(k.z) * 0x9E3779B97F4A7C15ULL;
        h ^= keel::as<keel::u64>(k.x) * 0xBF58476D1CE4E5B9ULL;
        h ^= keel::as<keel::u64>(k.y) * 0x94D049BB133111EBULL;
        return keel::as<keel::usize>(h);
    }
};

struct CachedVectorTile {
    keel::Array<keel::u32> pixels;
    guikit::Rect normQueryBox{};
    keel::u64 styleHash{0};
    keel::u64 lastUsedFrame{0};
    keel::u64 lastRasterizedFrame{0};
    bool hasAnyGeometry{false};
};

struct CompositeTile {
    keel::Array<keel::u32> pixels;
    guikit::Rect normQueryBox{};
    keel::u64 compositeSignature{0};
    keel::u64 lastUsedFrame{0};
    keel::u64 lastUpdatedFrame{0};
    bool isComplete{false};
};

struct AsyncLayerTileJobKey {
    keel::u64 layerUid{0};
    keel::i32 z{0};
    keel::i32 x{0};
    keel::i32 y{0};
    keel::u32 tileSizePx{256};

    [[nodiscard]] bool operator==(const AsyncLayerTileJobKey& o) const noexcept {
        return layerUid == o.layerUid && z == o.z && x == o.x && y == o.y && tileSizePx == o.tileSizePx;
    }
};

struct AsyncLayerTileJobKeyHasher {
    [[nodiscard]] keel::usize operator()(const AsyncLayerTileJobKey& k) const noexcept {
        keel::u64 h = k.layerUid * 0x9E3779B97F4A7C15ULL;
        HashCombineVal(h, k.z);
        HashCombineVal(h, k.x);
        HashCombineVal(h, k.y);
        HashCombineVal(h, k.tileSizePx);
        return keel::as<keel::usize>(h);
    }
};

struct InFlightTileStamp {
    keel::u64 styleHash{0};
    keel::u64 windowUpdateFrame{0};
    keel::u64 epoch{0};
};

struct WindowGenCounter {
    alignas(8) keel::u64 value{0};

    keel::u64 FetchAddRelaxed(keel::u64 delta = 1) noexcept {
        return keel::detail::atomic_raw_fetch_add(&value, delta, keel::MemoryOrder::Relaxed);
    }

    [[nodiscard]] keel::u64 LoadRelaxed() const noexcept {
        return keel::detail::atomic_raw_load(&value, keel::MemoryOrder::Relaxed);
    }
};

struct AsyncLoadedFeaturePayload {
    keel::u64 layerUid{0};
    keel::u32 featureIndex{0};
    keel::u64 featureUid{0};
    keel::Arc<const geo::DecodedGeometryPayload> payload;
};

struct AsyncLayerTileJob {
    AsyncLayerTileJobKey jobKey{};
    keel::u64 topologyHash{0};
    keel::u64 styleHash{0};
    keel::u64 windowUpdateFrame{0};
    keel::u64 epoch{0};
    guikit::OsmTileCell cell{};
    guikit::MapCrs crs{guikit::MapCrs::EPSG3857_WebMercator};
    guikit::Rect queryBox{};
    keel::f32 distSq{0.0f};
    bool lodSimplification{true};
    keel::f32 lodPixelTolerance{1.25f};
    keel::Arc<geo::IFeaturePager> pager;
    keel::Array<AsyncTileCandidate> candidates;
};

struct AsyncLayerTileResult {
    AsyncLayerTileJobKey jobKey{};
    keel::u64 topologyHash{0};
    keel::u64 styleHash{0};
    keel::u64 windowUpdateFrame{0};
    keel::u64 epoch{0};
    guikit::Rect queryBox{};
    keel::Array<keel::u32> pixels;
    bool hasAnyGeometry{false};
    keel::Array<AsyncLoadedFeaturePayload> newlyLoadedPayloads;
};

struct AsyncWindowJob {
    keel::u64 layerUid{0};
    keel::Arc<geo::IFeaturePager> pager;
    geo::SpatialWindowRequest req{};
    keel::u64 epoch{0};
    keel::Arc<WindowGenCounter> activeGenPtr;
};

struct AsyncWindowResult {
    keel::u64 layerUid{0};
    keel::u64 generation{0};
    keel::u64 epoch{0};
    keel::Arc<geo::SpatialWindowSnapshot> snapshot;
};

struct AsyncBasemapJob {
    keel::String sourceId;
    keel::Arc<geo::IRasterTileProvider> provider;
    geo::SlippyTileKey tileKey{};
    keel::f32 distSq{0.0f};
    keel::u64 epoch{0};
};

struct AsyncBasemapResult {
    keel::String sourceId;
    geo::SlippyTileKey tileKey{};
    keel::u64 epoch{0};
    keel::Arc<const geo::RasterTileImage> image;
};

class TilePixelBufferPool {
public:
    void AcquireFilled(keel::Array<keel::u32>& outBuf, keel::usize numPixels, keel::u32 fillVal);
    void Recycle(keel::Array<keel::u32>& buf) noexcept;
    void Clear(keel::usize newNumPixels = 0u) noexcept;

private:
    static constexpr keel::usize kMaxPooledBuffers = 16u;
    static constexpr keel::usize kMaxPooledBytes = 4u * 1024u * 1024u;
    keel::Mutex m_mutex;
    keel::usize m_activeNumPixels{0};
    keel::Array<keel::Array<keel::u32>> m_freeBuffers;
};

TilePixelBufferPool& GetTilePixelPool();

class AsyncChartStreamer {
public:
    AsyncChartStreamer();
    ~AsyncChartStreamer();

    void ClearAndBumpEpoch();
    void TrimIdleMemory() noexcept;
    [[nodiscard]] keel::u64 GetCurrentEpoch() const noexcept;
    void SetActiveTileZoom(keel::i32 z) noexcept;

    void RequestSpatialWindow(
        const geo::GeoLayer& layer,
        const guikit::GeoBounds& visibleGeo,
        const guikit::Rect& visibleNormBox,
        keel::f32 tileZoom,
        bool scaleFiltering,
        keel::u64 frameTick
    );
    void DrainCompletedWindows(keel::Array<AsyncWindowResult>& outResults);
    [[nodiscard]] keel::usize GetPendingWindowCount() const;
    [[nodiscard]] bool IsLayerWindowPending(keel::u64 layerUid) const;

    [[nodiscard]] bool IsLayerTilePending(
        const AsyncLayerTileJobKey& key,
        keel::u64 styleHash,
        keel::u64 windowUpdateFrame
    ) const;
    void EnqueueLayerTileJob(AsyncLayerTileJob&& job);
    void DrainCompletedLayerTiles(keel::Array<AsyncLayerTileResult>& outResults);
    [[nodiscard]] keel::usize GetPendingLayerTileCount() const;

    void EnqueueBasemapTileJob(
        const keel::Arc<geo::IRasterTileProvider>& provider,
        const geo::SlippyTileKey& tileKey,
        keel::f32 distSq
    );
    void DrainCompletedBasemapTiles(keel::Array<AsyncBasemapResult>& outResults);
    [[nodiscard]] keel::usize GetPendingBasemapCount() const;

    bool WaitForIdle(keel::u32 timeoutMs = 5000);

private:
    struct TileWorkerScratch {
        keel::UniquePtr<guikit::ICanvas> canvas;
        guikit::VectorPath fillPath;
        guikit::VectorPath strokePath;
        keel::Array<guikit::Vec2> clippedRing;
        keel::Array<guikit::Vec2> scratchA;
        keel::Array<guikit::Vec2> scratchB;
    };

    void WindowWorkerLoop();
    void TileWorkerLoop();
    void BasemapWorkerLoop();

    keel::Atomic<bool> m_shutdown{false};
    keel::Atomic<keel::u64> m_epoch{1};
    keel::Atomic<keel::u64> m_trimGeneration{0};
    keel::Atomic<keel::i32> m_activeTileZoom{12};

    mutable keel::Mutex m_windowMutex;
    keel::CondVar m_windowCv;
    keel::HashMap<keel::u64, keel::Arc<WindowGenCounter>> m_layerWindowGenerations;
    keel::HashMap<keel::u64, AsyncWindowJob> m_pendingWindowJobs;
    keel::Array<AsyncWindowResult> m_completedWindowResults;
    keel::usize m_inFlightWindowCount{0};
    keel::u64 m_inFlightWindowLayerUid{0};
    guikit::GeoBounds m_inFlightWindowGeo{};
    keel::f32 m_inFlightWindowZoom{-1.0f};
    bool m_inFlightWindowScaleFilter{true};
    keel::Thread m_windowWorker;

    mutable keel::Mutex m_tileMutex;
    keel::CondVar m_tileCv;
    keel::Array<AsyncLayerTileJob> m_pendingTileJobs;
    keel::HashMap<AsyncLayerTileJobKey, InFlightTileStamp, AsyncLayerTileJobKeyHasher> m_inFlightTileKeys;
    keel::Array<AsyncLayerTileResult> m_completedTileResults;
    keel::usize m_inFlightTileCount{0};
    keel::Array<keel::Thread> m_tileWorkers;

    mutable keel::Mutex m_basemapMutex;
    keel::CondVar m_basemapCv;
    keel::Array<AsyncBasemapJob> m_pendingBasemapJobs;
    keel::HashSet<geo::SlippyTileKey, geo::SlippyTileKeyHash> m_inFlightBasemapKeys;
    keel::Array<AsyncBasemapResult> m_completedBasemapResults;
    keel::usize m_inFlightBasemapCount{0};
    keel::Array<keel::Thread> m_basemapWorkers;
};

AsyncChartStreamer& GetAsyncStreamer();

class PerLayerTileCacheManager {
public:
    struct TileCandidate {
        const geo::GeoFeature* feat{nullptr};
        keel::u32 featIdx{0};
        keel::i32 painterRank{0};
        keel::f32 sortDepth{0.0f};
        keel::f32 maxDimPx{256.0f};
    };

    struct LayerTileStore {
        keel::u64 activeTopologyHash{0};
        keel::u64 lastWindowUpdateFrame{1};
        keel::HashMap<TileCoordKey, CachedVectorTile, TileCoordKeyHasher> tiles{nullptr};
    };

    struct CompositeLayerRef {
        LayerTileStore* store{nullptr};
        keel::u8 opacityByte{255};
    };

    PerLayerTileCacheManager();
    ~PerLayerTileCacheManager();

    void Clear();
    void ReleaseMemory() noexcept;
    void SyncConfig(const GisRenderConfig& cfg, guikit::MapCrs crs, keel::StringRef basemapSourceId);

    [[nodiscard]] keel::u32 GetTileSizePx() const noexcept { return m_tileSizePx; }
    [[nodiscard]] keel::f32 GetTileSizeF() const noexcept { return keel::as<keel::f32>(m_tileSizePx); }
    [[nodiscard]] guikit::MapCrs GetActiveCrs() const noexcept { return m_activeCrs; }

    void GarbageCollectLayers(keel::ArrayRef<geo::GeoLayer> layers);
    [[nodiscard]] LayerTileStore& SyncLayerStore(const geo::GeoLayer& layer, guikit::MapCrs crs);
    keel::usize InvalidateTilesForDirtiedBoxes(
        keel::u64 layerUid,
        keel::ArrayRef<guikit::Rect> dirtiedNormBoxes,
        keel::u64 frameTick = 0
    ) noexcept;

    [[nodiscard]] static const CachedVectorTile* FindTile(
        LayerTileStore& store,
        const TileCoordKey& key,
        keel::u64 frameTick
    ) noexcept;

    void MarkTileEmpty(
        LayerTileStore& store,
        const TileCoordKey& key,
        const guikit::Rect& normQueryBox,
        keel::u64 styleHash,
        keel::u64 frameTick
    );

    bool StoreAsyncLayerTile(AsyncLayerTileResult&& res, keel::u64 frameTick);

    [[nodiscard]] bool HasBasemapTile(const TileCoordKey& key) const noexcept;
    [[nodiscard]] const geo::RasterTileImage* FindBasemapTile(
        const TileCoordKey& key,
        keel::u64 frameTick = 0ULL
    ) noexcept;
    [[nodiscard]] keel::u64 GetBasemapGeneration() const noexcept { return m_basemapGeneration; }
    bool StoreBasemapTile(
        keel::StringRef sourceId,
        const geo::SlippyTileKey& tileKey,
        keel::Arc<const geo::RasterTileImage> img,
        keel::u64 frameTick
    );

    [[nodiscard]] CompositeTile* FindCompositeTile(const TileCoordKey& key, keel::u64 frameTick) noexcept;
    void PinVisibleBasemapTiles(keel::ArrayRef<guikit::OsmTileCell> visibleTiles, keel::u64 frameTick) noexcept;
    void PinVisibleTiles(
        keel::ArrayRef<guikit::OsmTileCell> visibleTiles,
        keel::ArrayRef<LayerTileStore*> stores,
        keel::u64 frameTick
    ) noexcept;
    void EnforceCompositeBudget(keel::u64 protectedFrameTick) noexcept;

    CompositeTile& BuildCompositeTile(
        const TileCoordKey& key,
        const guikit::Rect& normQueryBox,
        keel::u64 compositeSignature,
        keel::u32 bgPx,
        keel::ArrayRef<CompositeLayerRef> layerRefs,
        keel::u64 frameTick,
        bool useBasemapOverlay = false,
        keel::u8 basemapOpacityByte = 255
    );

    bool SeedFallbackFromHierarchy(const TileCoordKey& key, keel::u32 bgPx, keel::u64 frameTick);

    const CachedVectorTile* RasterizeOrDispatchLayerTile(
        LayerTileStore& store,
        const TileCoordKey& key,
        keel::u64 styleHash,
        const guikit::OsmTileCell& cell,
        guikit::MapCrs crs,
        const geo::GeoLayer& layer,
        const GisRenderConfig& cfg,
        keel::u64 frameTick,
        keel::f32 distSqToCenter,
        bool allowAsyncDispatch,
        bool canRunSyncFastPath,
        keel::usize* outPagedInCount = nullptr,
        bool hasDeadline = false,
        keel::u64 deadlineUs = 0,
        keel::usize minPagedBeforeCheck = 96,
        bool* outAbortedOrQueued = nullptr
    );

    void EnforceGlobalLayerPixelBudget(
        keel::u64 protectedFrameTick,
        keel::u64 activeCompositeSignature = 0ULL
    ) noexcept;

private:
    static constexpr keel::usize kMaxMetadataTilesPerLayer = 96u;

    struct CachedBasemapTile {
        keel::Arc<const geo::RasterTileImage> image;
        keel::u64 lastUsedFrame{0};
    };

    void ReleaseTilePixels(CachedVectorTile& tile) noexcept;
    void ClearStoreTiles(LayerTileStore& store) noexcept;
    bool BlendBasemapTileOrAncestor(keel::u32* dstPixels, const TileCoordKey& key, keel::u8 opacityByte) const;
    void EvictOldestLayerTiles(LayerTileStore& store, keel::usize targetCount, keel::u64 protectedFrameTick) noexcept;
    void EvictOldestCompositeTiles(keel::usize targetCount, keel::u64 protectedFrameTick = ~keel::u64{0}) noexcept;

    keel::UniquePtr<guikit::ICanvas> m_tileCanvas;
    guikit::VectorPath m_fillPath;
    guikit::VectorPath m_strokePath;
    keel::u32 m_tileSizePx{256};
    guikit::MapCrs m_activeCrs{guikit::MapCrs::EPSG3857_WebMercator};
    keel::String m_activeBasemapSourceId;
    keel::u64 m_basemapGeneration{1};
    keel::usize m_maxLayerTiles{48};
    keel::usize m_maxCompositeTiles{48};
    keel::usize m_pixelLayerTileCount{0};
    keel::HashMap<keel::u64, LayerTileStore> m_layerStores;
    keel::HashMap<TileCoordKey, CompositeTile, TileCoordKeyHasher> m_compositeTiles;
    keel::HashMap<TileCoordKey, CachedBasemapTile, TileCoordKeyHasher> m_basemapTiles;
    mutable keel::Array<keel::u32> m_upscaledBasemapScratch;
    keel::Array<keel::u32> m_queryIndices;
    keel::Array<guikit::Vec2> m_clippedRing;
    keel::Array<guikit::Vec2> m_scratchA;
    keel::Array<guikit::Vec2> m_scratchB;
    keel::Array<TileCandidate> m_candidates;
    keel::Array<AsyncTileCandidate> m_syncAsyncCandidates;
};

PerLayerTileCacheManager& GetTileCache();

[[nodiscard]] keel::Arc<geo::IRasterTileProvider> ResolveActiveBasemapProvider(const GisRenderConfig& cfg);

// ============================================================================
// World-Space Declutter Cache, S-52 Labels, Hit-Testing & Highlight Overlays
//   Implemented in gis_symbols_hittest.cpp
// ============================================================================

[[nodiscard]] keel::StringRef ResolveFeatureLabelText(
    const geo::GeoFeature& feat,
    const GisRenderConfig& cfg
) noexcept;

[[nodiscard]] keel::StringRef FormatMapCalloutLabel(
    const geo::GeoFeature& feat,
    const GisRenderConfig& cfg,
    keel::String& scratchBuf,
    keel::usize* outCharCount = nullptr
);

class WorldDeclutterCache {
public:
    struct AcceptedItem {
        keel::u32 layerIdx{0};
        keel::u32 featIdx{0};
    };

    void Clear() noexcept;
    void ReleaseMemory() noexcept;
    void Sync(
        keel::ArrayRef<geo::GeoLayer> layers,
        const GisRenderConfig& cfg,
        keel::i32 tileZoom,
        guikit::MapCrs crs,
        const guikit::Rect& paddedVisNormBox
    );

    [[nodiscard]] keel::ArrayRef<AcceptedItem> GetAcceptedSymbols() const noexcept { return m_acceptedSymbols; }
    [[nodiscard]] keel::ArrayRef<AcceptedItem> GetAcceptedLabels() const noexcept { return m_acceptedLabels; }
    [[nodiscard]] keel::ArrayRef<AcceptedItem> GetAcceptedSoundings() const noexcept { return m_acceptedSoundings; }
    [[nodiscard]] keel::usize GetTotalDecluttered() const noexcept { return m_totalDecluttered; }

private:
    static constexpr keel::usize kCellTableSize = 131072; // 2^17 slots (~1.5 MB flat table, zero heap allocations)
    static constexpr keel::usize kCellTableMask = kCellTableSize - 1;

    struct SoundingCand {
        keel::u32 layerIdx;
        keel::u32 featIdx;
        keel::f32 depth;
        keel::f32 wx;
        keel::f32 wy;
    };

    void ClearOccupiedCells() noexcept;
    [[nodiscard]] static keel::u64 PackCell(keel::i32 cx, keel::i32 cy) noexcept;
    [[nodiscard]] static keel::usize HashCellKey(keel::u64 k) noexcept;
    [[nodiscard]] bool ContainsCell(keel::u64 key) const noexcept;
    void InsertCell(keel::u64 key) noexcept;
    [[nodiscard]] bool TryReserveWorldBox(keel::f32 wx, keel::f32 wy, keel::f32 w, keel::f32 h) noexcept;

    keel::u64 m_cachedKey{0};
    guikit::Rect m_safeNormBox{0.0f, 0.0f, 0.0f, 0.0f};
    keel::usize m_totalDecluttered{0};
    keel::Array<AcceptedItem> m_acceptedSymbols;
    keel::Array<AcceptedItem> m_acceptedLabels;
    keel::Array<AcceptedItem> m_acceptedSoundings;
    keel::Array<keel::u64> m_cellKeys;
    keel::Array<keel::u32> m_cellEpochs;
    keel::u32 m_curEpoch{1};
    keel::usize m_occupiedCount{0};
    keel::Array<keel::u32> m_queryScratch;
    keel::Array<SoundingCand> m_soundingCands;
};

WorldDeclutterCache& GetDeclutterCache();

struct HitTestMatch {
    const geo::GeoLayer* layer{nullptr};
    const geo::GeoFeature* feat{nullptr};
};

struct CursorHitCache {
    guikit::Vec2 cursorNorm{-1e9f, -1e9f};
    keel::f32 pickRadiusNorm{0.0f};
    keel::i32 tileZoom{-1};
    keel::usize layerCount{0};
    bool showCoverageBounds{false};
    bool showCautionAndRestrictedZones{true};
    keel::u64 matchedUid{0};
    bool valid{false};

    void Clear() noexcept { valid = false; matchedUid = 0; }
};

CursorHitCache& GetCursorHitCache() noexcept;

[[nodiscard]] HitTestMatch FindFeatureUnderCursor(
    guikit::Vec2 cursorNorm,
    keel::f32 pickRadiusNorm,
    keel::i32 tileZoom,
    keel::ArrayRef<geo::GeoLayer> layers,
    const GisRenderConfig& cfg,
    keel::u64 frameTick,
    keel::usize* outPagedInCount = nullptr
);

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
);

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
    keel::usize* outPagedInCount = nullptr
);

} // namespace guikit::geo::detail
