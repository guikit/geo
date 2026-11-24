#include "gis_render_internal.hpp"
#include "keel/algo.h"
#include "keel/math.h"
#include "keel/memory.h"
#include "keel/thread_pool.h"
#include "keel/time.h"

namespace guikit::geo::detail {

void TilePixelBufferPool::AcquireFilled(keel::Array<keel::u32>& outBuf, keel::usize numPixels, keel::u32 fillVal) {
    if (outBuf.capacity() != numPixels) {
        if (outBuf.capacity() > 0u) {
            outBuf = keel::Array<keel::u32>{};
        }
        keel::LockGuard<keel::Mutex> lk(m_mutex);
        if (m_activeNumPixels == numPixels && !m_freeBuffers.empty()) {
            outBuf = keel::move(m_freeBuffers.back());
            m_freeBuffers.pop_back();
        }
    }
    outBuf.assign(numPixels, fillVal);
}

void TilePixelBufferPool::Recycle(keel::Array<keel::u32>& buf) noexcept {
    const keel::usize cap = buf.capacity();
    if (cap == 0u) {
        return;
    }
    buf.clear();
    {
        keel::LockGuard<keel::Mutex> lk(m_mutex);
        if (m_activeNumPixels == 0u) {
            m_activeNumPixels = cap;
        }
        if (cap == m_activeNumPixels &&
            m_freeBuffers.size() < kMaxPooledBuffers &&
            (m_freeBuffers.size() + 1u) * cap * sizeof(keel::u32) <= kMaxPooledBytes) {
            m_freeBuffers.push_back(keel::move(buf));
            return;
        }
    }
    buf = keel::Array<keel::u32>{};
}

void TilePixelBufferPool::Clear(keel::usize newNumPixels) noexcept {
    keel::Array<keel::Array<keel::u32>> toFree;
    {
        keel::LockGuard<keel::Mutex> lk(m_mutex);
        m_activeNumPixels = newNumPixels;
        toFree = keel::move(m_freeBuffers);
        m_freeBuffers = {};
    }
}

TilePixelBufferPool& GetTilePixelPool() {
    static TilePixelBufferPool s_pool;
    return s_pool;
}

AsyncChartStreamer::AsyncChartStreamer() {
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    if (auto t = keel::Thread::spawn([this]() { WindowWorkerLoop(); }, "gis-window"); t.is_ok()) {
        m_windowWorker = keel::move(t.value());
    }

    const keel::u32 hw = keel::max(2u, keel::hardware_concurrency());
    const keel::u32 numTileWorkers = keel::clamp<keel::u32>(hw / 2u, 2u, 4u);
    for (keel::u32 i = 0; i < numTileWorkers; ++i) {
        if (auto t = keel::Thread::spawn([this]() { TileWorkerLoop(); }, "gis-tile"); t.is_ok()) {
            m_tileWorkers.push_back(keel::move(t.value()));
        }
    }

    for (keel::usize i = 0; i < 2; ++i) {
        if (auto t = keel::Thread::spawn([this]() { BasemapWorkerLoop(); }, "gis-basemap"); t.is_ok()) {
            m_basemapWorkers.push_back(keel::move(t.value()));
        }
    }
#endif
}

AsyncChartStreamer::~AsyncChartStreamer() {
    {
        keel::LockGuard<keel::Mutex> lkWindow(m_windowMutex);
            keel::LockGuard<keel::Mutex> lkTile(m_tileMutex);
            keel::LockGuard<keel::Mutex> lkBasemap(m_basemapMutex);
        m_shutdown.store(true, keel::MemoryOrder::Release);
        for (auto [uid, genPtr] : m_layerWindowGenerations) {
            if (genPtr) genPtr->FetchAddRelaxed(1);
        }
        m_pendingWindowJobs.clear();
        m_pendingTileJobs.clear();
        m_pendingBasemapJobs.clear();
    }
    m_windowCv.notify_all();
    m_tileCv.notify_all();
    m_basemapCv.notify_all();

    if (m_windowWorker.joinable()) m_windowWorker.join();
    for (auto& t : m_tileWorkers) {
        if (t.joinable()) t.join();
    }
    for (auto& t : m_basemapWorkers) {
        if (t.joinable()) t.join();
    }
}

void AsyncChartStreamer::ClearAndBumpEpoch() {
    const keel::u64 newEpoch = m_epoch.fetch_add(1, keel::MemoryOrder::AcqRel) + 1;
    {
        keel::LockGuard<keel::Mutex> lk(m_windowMutex);
        for (auto [uid, genPtr] : m_layerWindowGenerations) {
            if (genPtr) genPtr->FetchAddRelaxed(1);
        }
        m_pendingWindowJobs.clear();
        m_completedWindowResults.clear();
    }
    {
        keel::LockGuard<keel::Mutex> lk(m_tileMutex);
        m_pendingTileJobs.clear();
        m_inFlightTileKeys.clear();
        for (AsyncLayerTileResult& res : m_completedTileResults) {
            GetTilePixelPool().Recycle(res.pixels);
        }
        m_completedTileResults.clear();
    }
    {
        keel::LockGuard<keel::Mutex> lk(m_basemapMutex);
        m_pendingBasemapJobs.clear();
        m_inFlightBasemapKeys.clear();
        m_completedBasemapResults.clear();
    }
    (void)newEpoch;
}

void AsyncChartStreamer::TrimIdleMemory() noexcept {
    ClearAndBumpEpoch();
    {
        keel::LockGuard<keel::Mutex> lk(m_windowMutex);
        m_pendingWindowJobs = {};
        m_completedWindowResults = {};
    }
    {
        keel::LockGuard<keel::Mutex> lk(m_tileMutex);
        m_pendingTileJobs = {};
        m_inFlightTileKeys = {};
        m_completedTileResults = {};
        m_trimGeneration.fetch_add(1, keel::MemoryOrder::AcqRel);
    }
    {
        keel::LockGuard<keel::Mutex> lk(m_basemapMutex);
        m_pendingBasemapJobs = {};
        m_inFlightBasemapKeys = {};
        m_completedBasemapResults = {};
    }
    m_tileCv.notify_all();
}

[[nodiscard]] keel::u64 AsyncChartStreamer::GetCurrentEpoch() const noexcept {
    return m_epoch.load(keel::MemoryOrder::Acquire);
}

void AsyncChartStreamer::SetActiveTileZoom(keel::i32 z) noexcept {
    m_activeTileZoom.store(z, keel::MemoryOrder::Relaxed);
}

void AsyncChartStreamer::RequestSpatialWindow(
    const geo::GeoLayer& layer,
    const guikit::GeoBounds& visibleGeo,
    const guikit::Rect& visibleNormBox,
    keel::f32 tileZoom,
    bool scaleFiltering,
    keel::u64 frameTick
) {
    if (!layer.pager) return;
    const keel::u64 layerUid = layer.uid;
    keel::LockGuard<keel::Mutex> lk(m_windowMutex);
    auto& genPtr = m_layerWindowGenerations.get_or_insert(layerUid);
    if (!genPtr) {
        genPtr = keel::make_arc<WindowGenCounter>();
    }

    // Check if an identical window job is already pending or in-flight
    if (const AsyncWindowJob* cur = m_pendingWindowJobs.get(layerUid)) {
        if (keel::abs(cur->req.tileZoom - tileZoom) < 0.05f &&
            cur->req.scaleFiltering == scaleFiltering &&
            keel::abs(cur->req.viewBounds.nw.lat - visibleGeo.nw.lat) < 1e-6 &&
            keel::abs(cur->req.viewBounds.nw.lon - visibleGeo.nw.lon) < 1e-6 &&
            keel::abs(cur->req.viewBounds.se.lat - visibleGeo.se.lat) < 1e-6 &&
            keel::abs(cur->req.viewBounds.se.lon - visibleGeo.se.lon) < 1e-6) {
            return;
        }
    }
    if (m_inFlightWindowLayerUid == layerUid &&
        keel::abs(m_inFlightWindowZoom - tileZoom) < 0.05f &&
        m_inFlightWindowScaleFilter == scaleFiltering &&
        keel::abs(m_inFlightWindowGeo.nw.lat - visibleGeo.nw.lat) < 1e-6 &&
        keel::abs(m_inFlightWindowGeo.nw.lon - visibleGeo.nw.lon) < 1e-6 &&
        keel::abs(m_inFlightWindowGeo.se.lat - visibleGeo.se.lat) < 1e-6 &&
        keel::abs(m_inFlightWindowGeo.se.lon - visibleGeo.se.lon) < 1e-6) {
        return;
    }

    const keel::u64 nextGen = genPtr->FetchAddRelaxed(1) + 1;
    geo::SpatialWindowRequest req{};
    req.layerUid = layerUid;
    req.srsId = layer.srsId;
    req.layerBounds = layer.bounds;
    req.hasLayerBounds = layer.hasValidBounds;
    req.dominantClass = layer.dominantChartClass;
    req.chartClassMask = layer.chartClassMask;
    req.savedPointCount = layer.pointFeatureCount;
    req.savedLineCount = layer.lineFeatureCount;
    req.savedPolygonCount = layer.polygonFeatureCount;
    req.viewBounds = visibleGeo;
    req.normViewBox = visibleNormBox;
    req.tileZoom = tileZoom;
    req.scaleFiltering = scaleFiltering;
    req.frameTick = frameTick;
    req.requestGeneration = nextGen;

    m_pendingWindowJobs.put(layerUid, AsyncWindowJob{
        .layerUid = layerUid,
        .pager = layer.pager,
        .req = req,
        .epoch = GetCurrentEpoch(),
        .activeGenPtr = genPtr
    });
    m_windowCv.notify_one();
}

void AsyncChartStreamer::DrainCompletedWindows(keel::Array<AsyncWindowResult>& outResults) {
    outResults.clear();
    const keel::u64 curEpoch = GetCurrentEpoch();
    keel::LockGuard<keel::Mutex> lk(m_windowMutex);
    for (AsyncWindowResult& res : m_completedWindowResults) {
        if (res.epoch != curEpoch || !res.snapshot) continue;
        if (const auto* gPtr = m_layerWindowGenerations.get(res.layerUid); gPtr && *gPtr) {
            if (res.generation < (*gPtr)->LoadRelaxed()) {
                continue; // Superseded by a newer window request for this layer
            }
        }
        outResults.push_back(keel::move(res));
    }
    m_completedWindowResults.clear();
}

[[nodiscard]] keel::usize AsyncChartStreamer::GetPendingWindowCount() const {
    keel::LockGuard<keel::Mutex> lk(m_windowMutex);
    return m_pendingWindowJobs.size() + m_inFlightWindowCount + m_completedWindowResults.size();
}

[[nodiscard]] bool AsyncChartStreamer::IsLayerWindowPending(keel::u64 layerUid) const {
    keel::LockGuard<keel::Mutex> lk(m_windowMutex);
    if (m_inFlightWindowLayerUid == layerUid && m_inFlightWindowCount > 0) {
        return true;
    }
    if (m_pendingWindowJobs.contains(layerUid)) {
        return true;
    }
    for (const AsyncWindowResult& res : m_completedWindowResults) {
        if (res.layerUid == layerUid) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool AsyncChartStreamer::IsLayerTilePending(const AsyncLayerTileJobKey& key, keel::u64 styleHash, keel::u64 windowUpdateFrame) const {
    const keel::u64 curEpoch = GetCurrentEpoch();
    keel::LockGuard<keel::Mutex> lk(m_tileMutex);
    if (const auto* inFlight = m_inFlightTileKeys.get(key)) {
        if (inFlight->epoch == curEpoch && inFlight->styleHash == styleHash &&
            inFlight->windowUpdateFrame == windowUpdateFrame) {
            return true;
        }
    }
    for (const AsyncLayerTileJob& job : m_pendingTileJobs) {
        if (job.epoch == curEpoch && job.jobKey == key && job.styleHash == styleHash &&
            job.windowUpdateFrame == windowUpdateFrame) {
            return true;
        }
    }
    for (const AsyncLayerTileResult& res : m_completedTileResults) {
        if (res.epoch == curEpoch && res.jobKey == key && res.styleHash == styleHash &&
            res.windowUpdateFrame == windowUpdateFrame) {
            return true;
        }
    }
    return false;
}

void AsyncChartStreamer::EnqueueLayerTileJob(AsyncLayerTileJob&& job) {
    static constexpr keel::usize kMaxPendingTileJobs = 192u;
    job.epoch = GetCurrentEpoch();
    {
        keel::LockGuard<keel::Mutex> lk(m_tileMutex);
        if (const auto* inFlight = m_inFlightTileKeys.get(job.jobKey)) {
            if (inFlight->epoch == job.epoch && inFlight->styleHash == job.styleHash &&
                inFlight->windowUpdateFrame == job.windowUpdateFrame) {
                return;
            }
        }
        for (AsyncLayerTileJob& existing : m_pendingTileJobs) {
            if (existing.jobKey == job.jobKey) {
                existing = keel::move(job);
                m_tileCv.notify_one();
                return;
            }
        }
        if (m_pendingTileJobs.size() >= kMaxPendingTileJobs) {
            const keel::i32 activeZ = m_activeTileZoom.load(keel::MemoryOrder::Relaxed);
            for (keel::usize i = m_pendingTileJobs.size(); i > 0u; --i) {
                if (keel::abs(m_pendingTileJobs[i - 1u].jobKey.z - activeZ) > 1) {
                    m_pendingTileJobs.swap_remove(i - 1u);
                }
            }
            if (m_pendingTileJobs.size() >= kMaxPendingTileJobs) {
                keel::usize worstIdx = 0u;
                for (keel::usize i = 1u; i < m_pendingTileJobs.size(); ++i) {
                    const bool worstMatchesZ = (m_pendingTileJobs[worstIdx].jobKey.z == activeZ);
                    const bool curMatchesZ = (m_pendingTileJobs[i].jobKey.z == activeZ);
                    if (worstMatchesZ != curMatchesZ) {
                        if (!curMatchesZ) worstIdx = i;
                    } else if (m_pendingTileJobs[i].distSq > m_pendingTileJobs[worstIdx].distSq) {
                        worstIdx = i;
                    }
                }
                const bool newMatchesZ = (job.jobKey.z == activeZ);
                const bool worstMatchesZ = (m_pendingTileJobs[worstIdx].jobKey.z == activeZ);
                if ((newMatchesZ && !worstMatchesZ) ||
                    (newMatchesZ == worstMatchesZ && job.distSq < m_pendingTileJobs[worstIdx].distSq)) {
                    m_pendingTileJobs[worstIdx] = keel::move(job);
                    m_tileCv.notify_one();
                }
                return;
            }
        }
        m_pendingTileJobs.push_back(keel::move(job));
    }
    m_tileCv.notify_one();
}

void AsyncChartStreamer::DrainCompletedLayerTiles(keel::Array<AsyncLayerTileResult>& outResults) {
    outResults.clear();
    const keel::u64 curEpoch = GetCurrentEpoch();
    keel::LockGuard<keel::Mutex> lk(m_tileMutex);
    for (AsyncLayerTileResult& res : m_completedTileResults) {
        if (res.epoch == curEpoch) {
            outResults.push_back(keel::move(res));
        } else {
            GetTilePixelPool().Recycle(res.pixels);
        }
    }
    m_completedTileResults.clear();
}

[[nodiscard]] keel::usize AsyncChartStreamer::GetPendingLayerTileCount() const {
    keel::LockGuard<keel::Mutex> lk(m_tileMutex);
    return m_pendingTileJobs.size() + m_inFlightTileCount + m_completedTileResults.size();
}

void AsyncChartStreamer::EnqueueBasemapTileJob(
    const keel::Arc<geo::IRasterTileProvider>& provider,
    const geo::SlippyTileKey& tileKey,
    keel::f32 distSq
) {
    if (!provider) return;
    const keel::u64 curEpoch = GetCurrentEpoch();
    const keel::StringRef srcId = provider->GetSourceId();
    if (!HasBackgroundWorkerThreads()) {
        auto img = provider->FetchTileBlocking(tileKey);
        if (img && img->IsValid()) {
            keel::LockGuard<keel::Mutex> lk(m_basemapMutex);
            m_completedBasemapResults.push_back(AsyncBasemapResult{
                .sourceId = keel::String::from(srcId),
                .tileKey = tileKey,
                .epoch = curEpoch,
                .image = keel::move(img)
            });
        }
        return;
    }
    {
        static constexpr keel::usize kMaxPendingBasemapJobs = 128u;
        keel::LockGuard<keel::Mutex> lk(m_basemapMutex);
        if (m_inFlightBasemapKeys.contains(tileKey)) {
            return;
        }
        for (AsyncBasemapJob& existing : m_pendingBasemapJobs) {
            if (existing.tileKey == tileKey && existing.sourceId == srcId) {
                existing.distSq = keel::min(existing.distSq, distSq);
                return;
            }
        }
        if (m_pendingBasemapJobs.size() >= kMaxPendingBasemapJobs) {
            const keel::i32 activeZ = m_activeTileZoom.load(keel::MemoryOrder::Relaxed);
            for (keel::usize i = m_pendingBasemapJobs.size(); i > 0u; --i) {
                if (keel::abs(m_pendingBasemapJobs[i - 1u].tileKey.z - activeZ) > 2) {
                    m_pendingBasemapJobs.swap_remove(i - 1u);
                }
            }
            if (m_pendingBasemapJobs.size() >= kMaxPendingBasemapJobs) {
                keel::usize worstIdx = 0u;
                for (keel::usize i = 1u; i < m_pendingBasemapJobs.size(); ++i) {
                    if (m_pendingBasemapJobs[i].distSq > m_pendingBasemapJobs[worstIdx].distSq) {
                        worstIdx = i;
                    }
                }
                if (distSq < m_pendingBasemapJobs[worstIdx].distSq) {
                    m_pendingBasemapJobs[worstIdx] = AsyncBasemapJob{
                        .sourceId = keel::String::from(srcId),
                        .provider = provider,
                        .tileKey = tileKey,
                        .distSq = distSq,
                        .epoch = curEpoch
                    };
                    m_basemapCv.notify_one();
                }
                return;
            }
        }
        m_pendingBasemapJobs.push_back(AsyncBasemapJob{
            .sourceId = keel::String::from(srcId),
            .provider = provider,
            .tileKey = tileKey,
            .distSq = distSq,
            .epoch = curEpoch
        });
    }
    m_basemapCv.notify_one();
}

void AsyncChartStreamer::DrainCompletedBasemapTiles(keel::Array<AsyncBasemapResult>& outResults) {
    outResults.clear();
    const keel::u64 curEpoch = GetCurrentEpoch();
    keel::LockGuard<keel::Mutex> lk(m_basemapMutex);
    for (AsyncBasemapResult& res : m_completedBasemapResults) {
        if (res.epoch == curEpoch) {
            outResults.push_back(keel::move(res));
        }
    }
    m_completedBasemapResults.clear();
}

[[nodiscard]] keel::usize AsyncChartStreamer::GetPendingBasemapCount() const {
    keel::LockGuard<keel::Mutex> lk(m_basemapMutex);
    return m_pendingBasemapJobs.size() + m_inFlightBasemapCount + m_completedBasemapResults.size();
}

bool AsyncChartStreamer::WaitForIdle(keel::u32 timeoutMs) {
    const keel::u64 deadlineMs = keel::monotonic_ms() + keel::as<keel::u64>(timeoutMs);
    for (;;) {
        bool anyActive = false;
        {
            keel::LockGuard<keel::Mutex> lkWindow(m_windowMutex);
            keel::LockGuard<keel::Mutex> lkTile(m_tileMutex);
            keel::LockGuard<keel::Mutex> lkBasemap(m_basemapMutex);
            anyActive = !m_pendingWindowJobs.empty() || m_inFlightWindowCount > 0 ||
                        !m_pendingTileJobs.empty() || m_inFlightTileCount > 0 ||
                        !m_pendingBasemapJobs.empty() || m_inFlightBasemapCount > 0;
        }
        if (!anyActive) return true;
        if (keel::monotonic_ms() >= deadlineMs) {
            return false;
        }
        keel::thread_yield();
    }
}

void AsyncChartStreamer::WindowWorkerLoop() {
    for (;;) {
        AsyncWindowJob job{};
        {
            keel::UniqueLock<keel::Mutex> lk(m_windowMutex);
            m_windowCv.wait(lk, [this]() {
                return m_shutdown.load(keel::MemoryOrder::Acquire) || !m_pendingWindowJobs.empty();
            });
            if (m_shutdown.load(keel::MemoryOrder::Acquire)) {
                break;
            }
            auto it = m_pendingWindowJobs.begin();
            const keel::u64 kUid = (*it).key;
            job = keel::move((*it).value);
            m_pendingWindowJobs.remove(kUid);
            m_inFlightWindowCount = 1;
            m_inFlightWindowLayerUid = job.layerUid;
            m_inFlightWindowGeo = job.req.viewBounds;
            m_inFlightWindowZoom = job.req.tileZoom;
            m_inFlightWindowScaleFilter = job.req.scaleFiltering;
        }

        keel::Arc<geo::SpatialWindowSnapshot> snapshot;
        if (job.pager && job.epoch == GetCurrentEpoch()) {
            snapshot = job.pager->BuildSpatialWindowSnapshot(
                job.req,
                job.activeGenPtr ? &job.activeGenPtr->value : nullptr
            );
        }

        {
            keel::LockGuard<keel::Mutex> lk(m_windowMutex);
            m_inFlightWindowCount = 0;
            m_inFlightWindowLayerUid = 0;
            if (snapshot && snapshot->changed && job.epoch == GetCurrentEpoch()) {
                // Replace any earlier undrained result for the same layer
                for (keel::usize ri = m_completedWindowResults.size(); ri > 0; --ri) {
                    if (m_completedWindowResults[ri - 1].layerUid == job.layerUid) {
                        m_completedWindowResults.swap_remove(ri - 1);
                    }
                }
                m_completedWindowResults.push_back(AsyncWindowResult{
                    .layerUid = job.layerUid,
                    .generation = job.req.requestGeneration,
                    .epoch = job.epoch,
                    .snapshot = keel::move(snapshot)
                });
            }
        }
    }
}

void AsyncChartStreamer::TileWorkerLoop() {
    // Each worker thread constructs, uses, and destroys its OWN offscreen ICanvas strictly on
    // this thread. ThorVG's SwRenderer binds a thread_local SwMpool* on construction, so strict
    // 1:1 thread affinity eliminates cross-thread memory pool races completely.
    TileWorkerScratch slot{};
    slot.canvas = CreateCanvasThreadSafe();
    slot.fillPath.Reserve(256, 256);
    slot.strokePath.Reserve(512, 512);

    keel::u64 localTrimGen = m_trimGeneration.load(keel::MemoryOrder::Acquire);

    for (;;) {
        AsyncLayerTileJob job{};
        bool hasJob = false;
        bool shouldTrimWorker = false;
        {
            keel::UniqueLock<keel::Mutex> lk(m_tileMutex);
            m_tileCv.wait(lk, [this, &localTrimGen]() {
                return m_shutdown.load(keel::MemoryOrder::Acquire) ||
                       !m_pendingTileJobs.empty() ||
                       m_trimGeneration.load(keel::MemoryOrder::Acquire) != localTrimGen;
            });
            if (m_shutdown.load(keel::MemoryOrder::Acquire)) {
                break;
            }
            const keel::u64 curTrimGen = m_trimGeneration.load(keel::MemoryOrder::Acquire);
            if (curTrimGen != localTrimGen) {
                localTrimGen = curTrimGen;
                if (m_pendingTileJobs.empty()) {
                    shouldTrimWorker = true;
                }
            }
            if (!shouldTrimWorker && !m_pendingTileJobs.empty()) {
                const keel::i32 curZ = m_activeTileZoom.load(keel::MemoryOrder::Relaxed);
                // Pick the job matching `curZ` and closest to viewport center (`distSq`)
                keel::usize bestIdx = 0;
                for (keel::usize i = 1; i < m_pendingTileJobs.size(); ++i) {
                    const bool bestMatchesZ = (m_pendingTileJobs[bestIdx].jobKey.z == curZ);
                    const bool curMatchesZ = (m_pendingTileJobs[i].jobKey.z == curZ);
                    if (curMatchesZ != bestMatchesZ) {
                        if (curMatchesZ) bestIdx = i;
                    } else if (m_pendingTileJobs[i].distSq < m_pendingTileJobs[bestIdx].distSq) {
                        bestIdx = i;
                    }
                }
                job = keel::move(m_pendingTileJobs[bestIdx]);
                m_pendingTileJobs.swap_remove(bestIdx);
                m_inFlightTileKeys.put(
                    job.jobKey, InFlightTileStamp{job.styleHash, job.windowUpdateFrame, job.epoch}
                );
                ++m_inFlightTileCount;
                hasJob = true;
            }
        }

        if (shouldTrimWorker) {
            if (slot.canvas) {
                slot.canvas->TrimCaches();
            }
            slot.fillPath = guikit::VectorPath{};
            slot.strokePath = guikit::VectorPath{};
            slot.clippedRing = {};
            slot.scratchA = {};
            slot.scratchB = {};
            keel::memory_trim();
            continue;
        }

        if (!hasJob) {
            continue;
        }

        AsyncLayerTileResult res{};
        bool valid = false;
        const keel::i32 activeZ = m_activeTileZoom.load(keel::MemoryOrder::Relaxed);
        const bool obsoleteZoom = (keel::abs(job.jobKey.z - activeZ) > 1);
        if (!obsoleteZoom && job.epoch == GetCurrentEpoch()) {
            res.jobKey = job.jobKey;
            res.topologyHash = job.topologyHash;
            res.styleHash = job.styleHash;
            res.windowUpdateFrame = job.windowUpdateFrame;
            res.epoch = job.epoch;
            res.queryBox = job.queryBox;

            bool aborted = false;
            for (AsyncTileCandidate& cand : job.candidates) {
                if (job.epoch != GetCurrentEpoch()) {
                    aborted = true;
                    break;
                }
                if (!cand.payload && job.pager) {
                    cand.payload = job.pager->GetOrLoadGeometryPayload(
                        cand.sqliteRowId, cand.srsId, 0
                    );
                    if (cand.payload) {
                        res.newlyLoadedPayloads.push_back(AsyncLoadedFeaturePayload{
                            .layerUid = job.jobKey.layerUid,
                            .featureIndex = cand.featureIndex,
                            .featureUid = cand.featureUid,
                            .payload = cand.payload
                        });
                    }
                }
            }

            if (!slot.canvas) {
                slot.canvas = CreateCanvasThreadSafe();
            }
            if (!aborted && slot.canvas && job.epoch == GetCurrentEpoch()) {
                RasterizeCandidatesToPixels(
                    *slot.canvas,
                    slot.fillPath,
                    slot.strokePath,
                    slot.clippedRing,
                    slot.scratchA,
                    slot.scratchB,
                    job.candidates,
                    job.cell,
                    job.crs,
                    job.jobKey.tileSizePx,
                    job.queryBox,
                    job.lodSimplification,
                    job.lodPixelTolerance,
                    res.pixels,
                    res.hasAnyGeometry
                );
            }

            if (!aborted && job.epoch == GetCurrentEpoch()) {
                valid = true;
            }
        }

        {
            keel::LockGuard<keel::Mutex> lk(m_tileMutex);
            if (const auto* inFlight = m_inFlightTileKeys.get(job.jobKey);
                inFlight && inFlight->epoch == job.epoch) {
                m_inFlightTileKeys.remove(job.jobKey);
            }
            if (m_inFlightTileCount > 0) --m_inFlightTileCount;
            if (valid && res.epoch == GetCurrentEpoch()) {
                if (m_completedTileResults.size() >= 256u) {
                    GetTilePixelPool().Recycle(m_completedTileResults[0].pixels);
                    m_completedTileResults.swap_remove(0u);
                }
                m_completedTileResults.push_back(keel::move(res));
            } else {
                GetTilePixelPool().Recycle(res.pixels);
            }
        }
    }

    DestroyCanvasThreadSafe(slot.canvas);
}

void AsyncChartStreamer::BasemapWorkerLoop() {
    for (;;) {
        AsyncBasemapJob job{};
        {
            keel::UniqueLock<keel::Mutex> lk(m_basemapMutex);
            m_basemapCv.wait(lk, [this]() {
                return m_shutdown.load(keel::MemoryOrder::Acquire) || !m_pendingBasemapJobs.empty();
            });
            if (m_shutdown.load(keel::MemoryOrder::Acquire)) {
                break;
            }

            const keel::i32 curZ = m_activeTileZoom.load(keel::MemoryOrder::Relaxed);
            keel::usize bestIdx = 0;
            for (keel::usize i = 1; i < m_pendingBasemapJobs.size(); ++i) {
                const bool bestMatchesZ = (m_pendingBasemapJobs[bestIdx].tileKey.z == curZ);
                const bool curMatchesZ = (m_pendingBasemapJobs[i].tileKey.z == curZ);
                if (curMatchesZ != bestMatchesZ) {
                    if (curMatchesZ) bestIdx = i;
                } else if (m_pendingBasemapJobs[i].distSq < m_pendingBasemapJobs[bestIdx].distSq) {
                    bestIdx = i;
                }
            }
            job = keel::move(m_pendingBasemapJobs[bestIdx]);
            m_pendingBasemapJobs.swap_remove(bestIdx);
            m_inFlightBasemapKeys.insert(job.tileKey);
            ++m_inFlightBasemapCount;
        }

        keel::Arc<const geo::RasterTileImage> img;
        const keel::i32 activeZ = m_activeTileZoom.load(keel::MemoryOrder::Relaxed);
        if (job.provider && job.epoch == GetCurrentEpoch() && keel::abs(job.tileKey.z - activeZ) <= 2) {
            img = job.provider->FetchTileBlocking(job.tileKey);
        }

        {
            keel::LockGuard<keel::Mutex> lk(m_basemapMutex);
            m_inFlightBasemapKeys.remove(job.tileKey);
            if (m_inFlightBasemapCount > 0) --m_inFlightBasemapCount;
            if (img && img->IsValid() && job.epoch == GetCurrentEpoch()) {
                m_completedBasemapResults.push_back(AsyncBasemapResult{
                    .sourceId = keel::move(job.sourceId),
                    .tileKey = job.tileKey,
                    .epoch = job.epoch,
                    .image = keel::move(img)
                });
            }
        }
    }
}

AsyncChartStreamer& GetAsyncStreamer() {
    static AsyncChartStreamer s_streamer;
    return s_streamer;
}

namespace {

[[nodiscard]] keel::Arc<const geo::DecodedGeometryPayload> GetOrCacheInMemoryFeaturePayload(
    const geo::GeoFeature& feat
) {
    if (feat.sharedGeometry) {
        return feat.sharedGeometry;
    }
    if (!feat.geometryLoaded) {
        return {};
    }
    feat.sharedGeometry = feat.BuildDecodedGeometryPayload();
    return feat.sharedGeometry;
}


} // namespace

PerLayerTileCacheManager::PerLayerTileCacheManager()
    : m_tileCanvas(CreateCanvasThreadSafe()) {
    m_fillPath.Reserve(256, 256);
    m_strokePath.Reserve(512, 512);
}

PerLayerTileCacheManager::~PerLayerTileCacheManager() {
    DestroyCanvasThreadSafe(m_tileCanvas);
}

void PerLayerTileCacheManager::Clear() {
    for (auto [uid, store] : m_layerStores) {
        ClearStoreTiles(store);
    }
    m_layerStores.clear();
    m_pixelLayerTileCount = 0u;
    for (auto [k, comp] : m_compositeTiles) {
        GetTilePixelPool().Recycle(comp.pixels);
    }
    m_compositeTiles.clear();
    m_basemapTiles.clear();
    GetTilePixelPool().Recycle(m_upscaledBasemapScratch);
}

void PerLayerTileCacheManager::ReleaseMemory() noexcept {
    Clear();
    m_layerStores = {};
    m_compositeTiles = {};
    m_basemapTiles = {};
    m_upscaledBasemapScratch = {};
    m_queryIndices = {};
    m_clippedRing = {};
    m_scratchA = {};
    m_scratchB = {};
    m_candidates = {};
    m_syncAsyncCandidates = {};
    m_fillPath = guikit::VectorPath{};
    m_strokePath = guikit::VectorPath{};
    if (m_tileCanvas) {
        m_tileCanvas->TrimCaches();
    }
}

void PerLayerTileCacheManager::SyncConfig(const GisRenderConfig& cfg, guikit::MapCrs crs, keel::StringRef basemapSourceId) {
    const keel::u32 newTileSize = ResolveTileSizePx(cfg.tileSizePx);
    if (newTileSize != m_tileSizePx || crs != m_activeCrs) {
        GetAsyncStreamer().ClearAndBumpEpoch();
        Clear();
        keel::usize newNumTilePx = 0;
        (void)keel::checked_mul<keel::usize>(newTileSize, newTileSize, newNumTilePx);
        GetTilePixelPool().Clear(newNumTilePx);
        m_tileSizePx = newTileSize;
        m_activeCrs = crs;
    }
    if (m_activeBasemapSourceId != basemapSourceId) {
        m_activeBasemapSourceId.assign(basemapSourceId);
        m_basemapTiles.clear();
        ++m_basemapGeneration;
        for (auto [k, comp] : m_compositeTiles) {
            comp.isComplete = false;
            comp.compositeSignature = 0ULL;
        }
    }
    const keel::usize tileAreaScale = (m_tileSizePx > 256u)
        ? keel::as<keel::usize>((m_tileSizePx / 256u) * (m_tileSizePx / 256u))
        : 1u;
    const keel::usize rawMaxLayer = keel::clamp<keel::usize>(cfg.maxLayerTiles, 16u, 2048u);
    const keel::usize rawMaxComp = keel::clamp<keel::usize>(cfg.maxCompositeTiles, 16u, 2048u);
    m_maxLayerTiles = keel::max<keel::usize>(16u, rawMaxLayer / tileAreaScale);
    m_maxCompositeTiles = keel::max<keel::usize>(16u, rawMaxComp / tileAreaScale);
}

void PerLayerTileCacheManager::GarbageCollectLayers(keel::ArrayRef<geo::GeoLayer> layers) {
    keel::Array<keel::u64> staleUids;
    for (auto [uid, store] : m_layerStores) {
        bool found = false;
        for (const geo::GeoLayer& lyr : layers) {
            if (lyr.uid == uid) {
                found = true;
                break;
            }
        }
        if (!found) staleUids.push_back(uid);
    }
    for (keel::u64 uid : staleUids) {
        if (LayerTileStore* st = m_layerStores.get(uid)) {
            ClearStoreTiles(*st);
        }
        m_layerStores.remove(uid);
    }
}

[[nodiscard]] PerLayerTileCacheManager::LayerTileStore& PerLayerTileCacheManager::SyncLayerStore(
    const geo::GeoLayer& layer,
    guikit::MapCrs crs
) {
    keel::u64 topoHash = keel::kFnv1aOffsetBasis;
    HashCombineVal(topoHash, crs);
    HashCombineVal(topoHash, layer.uid);
    const keel::usize fCount = layer.isWindowStreamed
        ? keel::max(layer.estimatedTotalFeatures, keel::as<keel::usize>(1u))
        : layer.features.size();
    HashCombineVal(topoHash, fCount);

    LayerTileStore& store = m_layerStores.get_or_insert(layer.uid);
    if (store.activeTopologyHash != topoHash) {
        ClearStoreTiles(store);
        store.activeTopologyHash = topoHash;
        ++store.lastWindowUpdateFrame;
        for (auto [tileKey, comp] : m_compositeTiles) {
            comp.isComplete = false;
            comp.compositeSignature = 0ULL;
        }
    }
    return store;
}

keel::usize PerLayerTileCacheManager::InvalidateTilesForDirtiedBoxes(
    keel::u64 layerUid,
    keel::ArrayRef<guikit::Rect> dirtiedNormBoxes,
    keel::u64 frameTick) noexcept {
    LayerTileStore* storePtr = m_layerStores.get(layerUid);
    if (storePtr && frameTick > 0) {
        storePtr->lastWindowUpdateFrame = frameTick;
    }
    keel::usize invalidatedCount = 0;
    if (storePtr) {
        LayerTileStore& store = *storePtr;
        keel::Array<TileCoordKey> toErase;
        for (auto [tileKey, tile] : store.tiles) {
            bool shouldInvalidate = !tile.hasAnyGeometry;
            if (!shouldInvalidate) {
                for (const guikit::Rect& dirtyBox : dirtiedNormBoxes) {
                    if (tile.normQueryBox.Intersects(dirtyBox)) {
                        shouldInvalidate = true;
                        break;
                    }
                }
            }
            if (shouldInvalidate) {
                if (CompositeTile* comp = m_compositeTiles.get(tileKey)) {
                    comp->isComplete = false;
                    comp->compositeSignature = 0ULL;
                }
                toErase.push_back(tileKey);
            }
        }
        for (const TileCoordKey& tk : toErase) {
            if (CachedVectorTile* t = store.tiles.get(tk)) {
                ReleaseTilePixels(*t);
            }
            store.tiles.remove(tk);
            ++invalidatedCount;
        }
    }
    for (auto [tileKey, comp] : m_compositeTiles) {
        if (!comp.isComplete) continue;
        if (dirtiedNormBoxes.empty() && frameTick > 0) {
            comp.isComplete = false;
            comp.compositeSignature = 0ULL;
            ++invalidatedCount;
            continue;
        }
        for (const guikit::Rect& dirtyBox : dirtiedNormBoxes) {
            if (comp.normQueryBox.w <= 0.0f || comp.normQueryBox.Intersects(dirtyBox)) {
                comp.isComplete = false;
                comp.compositeSignature = 0ULL;
                ++invalidatedCount;
                break;
            }
        }
    }
    return invalidatedCount;
}

[[nodiscard]] const CachedVectorTile* PerLayerTileCacheManager::FindTile(
    LayerTileStore& store,
    const TileCoordKey& key,
    keel::u64 frameTick
) noexcept {
    CachedVectorTile* tile = store.tiles.get(key);
    if (!tile) return nullptr;
    tile->lastUsedFrame = frameTick;
    return tile;
}

void PerLayerTileCacheManager::MarkTileEmpty(
    LayerTileStore& store,
    const TileCoordKey& key,
    const guikit::Rect& normQueryBox,
    keel::u64 styleHash,
    keel::u64 frameTick
) {
    if (store.tiles.size() >= kMaxMetadataTilesPerLayer && !store.tiles.contains(key)) {
        EvictOldestLayerTiles(store, kMaxMetadataTilesPerLayer - 24u, frameTick);
    }
    CachedVectorTile& tile = store.tiles.get_or_insert(key);
    ReleaseTilePixels(tile);
    tile.normQueryBox = normQueryBox;
    tile.styleHash = styleHash;
    tile.lastUsedFrame = frameTick;
    tile.lastRasterizedFrame = frameTick;
    tile.hasAnyGeometry = false;
}

bool PerLayerTileCacheManager::StoreAsyncLayerTile(AsyncLayerTileResult&& res, keel::u64 frameTick) {
    if (res.jobKey.tileSizePx != m_tileSizePx) {
        GetTilePixelPool().Recycle(res.pixels);
        return false;
    }
    LayerTileStore* storePtr = m_layerStores.get(res.jobKey.layerUid);
    if (!storePtr) {
        GetTilePixelPool().Recycle(res.pixels);
        return false;
    }
    LayerTileStore& store = *storePtr;
    if (store.activeTopologyHash != res.topologyHash ||
        res.windowUpdateFrame < store.lastWindowUpdateFrame) {
        GetTilePixelPool().Recycle(res.pixels);
        return false;
    }

    const TileCoordKey key{res.jobKey.z, res.jobKey.x, res.jobKey.y};
    EnforceGlobalLayerPixelBudget(frameTick);
    if (store.tiles.size() >= kMaxMetadataTilesPerLayer && !store.tiles.contains(key)) {
        EvictOldestLayerTiles(store, kMaxMetadataTilesPerLayer - 24u, frameTick);
    }

    CachedVectorTile& tile = store.tiles.get_or_insert(key);
    ReleaseTilePixels(tile);
    if (res.hasAnyGeometry && !res.pixels.empty()) {
        tile.pixels = keel::move(res.pixels);
        if (tile.pixels.capacity() > 0u) {
            ++m_pixelLayerTileCount;
        }
    } else {
        GetTilePixelPool().Recycle(res.pixels);
    }
    tile.normQueryBox = res.queryBox;
    tile.styleHash = res.styleHash;
    tile.lastUsedFrame = frameTick;
    tile.lastRasterizedFrame = frameTick;
    tile.hasAnyGeometry = res.hasAnyGeometry && !tile.pixels.empty();

    if (CompositeTile* comp = m_compositeTiles.get(key)) {
        comp->isComplete = false;
        comp->compositeSignature = 0ULL;
    }
    return true;
}

[[nodiscard]] bool PerLayerTileCacheManager::HasBasemapTile(const TileCoordKey& key) const noexcept {
    return m_basemapTiles.contains(key);
}

[[nodiscard]] const geo::RasterTileImage* PerLayerTileCacheManager::FindBasemapTile(const TileCoordKey& key, keel::u64 frameTick) noexcept {
    if (auto* entry = m_basemapTiles.get(key); entry && entry->image && entry->image->IsValid()) {
        if (frameTick != 0ULL) {
            entry->lastUsedFrame = frameTick;
        }
        return entry->image.get();
    }
    return nullptr;
}

bool PerLayerTileCacheManager::StoreBasemapTile(
    keel::StringRef sourceId,
    const geo::SlippyTileKey& tileKey,
    keel::Arc<const geo::RasterTileImage> img,
    keel::u64 frameTick
) {
    if (!img || !img->IsValid() || tileKey.tileSizePx != m_tileSizePx || sourceId != m_activeBasemapSourceId) {
        return false;
    }
    const TileCoordKey key{tileKey.z, tileKey.x, tileKey.y};
    const keel::usize maxBasemapTiles = keel::min<keel::usize>(m_maxCompositeTiles, 32u);
    while (m_basemapTiles.size() >= maxBasemapTiles && !m_basemapTiles.contains(key) && !m_basemapTiles.empty()) {
        bool found = false;
        TileCoordKey victimKey{};
        keel::u64 oldestFrame = keel::Limits<keel::u64>::kMax;
        for (auto [k, v] : m_basemapTiles) {
            if (v.lastUsedFrame < frameTick && v.lastUsedFrame < oldestFrame) {
                oldestFrame = v.lastUsedFrame;
                victimKey = k;
                found = true;
            }
        }
        if (!found) {
            break;
        }
        m_basemapTiles.remove(victimKey);
    }
    m_basemapTiles.put(key, CachedBasemapTile{.image = keel::move(img), .lastUsedFrame = frameTick});
    ++m_basemapGeneration;

    if (CompositeTile* comp = m_compositeTiles.get(key)) {
        comp->isComplete = false;
        comp->compositeSignature = 0ULL;
    }
    return true;
}

[[nodiscard]] CompositeTile* PerLayerTileCacheManager::FindCompositeTile(
    const TileCoordKey& key,
    keel::u64 frameTick
) noexcept {
    CompositeTile* comp = m_compositeTiles.get(key);
    if (!comp) return nullptr;
    comp->lastUsedFrame = frameTick;
    return comp;
}

void PerLayerTileCacheManager::PinVisibleBasemapTiles(
    keel::ArrayRef<guikit::OsmTileCell> visibleTiles,
    keel::u64 frameTick
) noexcept {
    for (const guikit::OsmTileCell& cell : visibleTiles) {
        const TileCoordKey key{cell.id.z, cell.id.x, cell.id.y};
        if (CachedBasemapTile* bm = m_basemapTiles.get(key)) {
            bm->lastUsedFrame = frameTick;
        }
    }
}

void PerLayerTileCacheManager::PinVisibleTiles(
    keel::ArrayRef<guikit::OsmTileCell> visibleTiles,
    keel::ArrayRef<LayerTileStore*> stores,
    keel::u64 frameTick
) noexcept {
    for (const guikit::OsmTileCell& cell : visibleTiles) {
        const TileCoordKey key{cell.id.z, cell.id.x, cell.id.y};
        if (CompositeTile* comp = m_compositeTiles.get(key)) {
            comp->lastUsedFrame = frameTick;
        }
        if (CachedBasemapTile* bm = m_basemapTiles.get(key)) {
            bm->lastUsedFrame = frameTick;
        }
        for (LayerTileStore* store : stores) {
            if (!store) continue;
            if (CachedVectorTile* lt = store->tiles.get(key)) {
                lt->lastUsedFrame = frameTick;
            }
        }
    }
}

void PerLayerTileCacheManager::EnforceCompositeBudget(keel::u64 protectedFrameTick) noexcept {
    if (m_compositeTiles.size() > m_maxCompositeTiles) {
        EvictOldestCompositeTiles(m_maxCompositeTiles, protectedFrameTick);
    }
}

CompositeTile& PerLayerTileCacheManager::BuildCompositeTile(
    const TileCoordKey& key,
    const guikit::Rect& normQueryBox,
    keel::u64 compositeSignature,
    keel::u32 bgPx,
    keel::ArrayRef<CompositeLayerRef> layerRefs,
    keel::u64 frameTick,
    bool useBasemapOverlay,
    keel::u8 basemapOpacityByte) {
    if (m_compositeTiles.size() >= m_maxCompositeTiles && !m_compositeTiles.contains(key)) {
        EvictOldestCompositeTiles(
            m_maxCompositeTiles - keel::min<keel::usize>(24u, m_maxCompositeTiles / 4u),
            frameTick
        );
    }
    keel::usize numTilePx = 0;
    if (!keel::checked_mul<keel::usize>(m_tileSizePx, m_tileSizePx, numTilePx)) {
        CompositeTile& comp = m_compositeTiles.get_or_insert(key);
        comp.pixels.clear();
        comp.isComplete = false;
        return comp;
    }
    CompositeTile& comp = m_compositeTiles.get_or_insert(key);
    GetTilePixelPool().AcquireFilled(comp.pixels, numTilePx, bgPx);
    comp.normQueryBox = normQueryBox;
    comp.compositeSignature = compositeSignature;
    comp.lastUsedFrame = frameTick;
    comp.lastUpdatedFrame = frameTick;
    comp.isComplete = true;

    if (useBasemapOverlay && basemapOpacityByte > 0) {
        const bool exactBasemap = BlendBasemapTileOrAncestor(comp.pixels.data(), key, basemapOpacityByte);
        if (!exactBasemap) {
            comp.isComplete = false;
        }
    }

    for (const CompositeLayerRef& ref : layerRefs) {
        if (!ref.store || ref.opacityByte == 0) continue;
        const CachedVectorTile* lt = FindTile(*ref.store, key, frameTick);
        if (lt && lt->hasAnyGeometry && lt->pixels.size() == numTilePx) {
            BlendPremulTileOntoOpaqueComposite(comp.pixels.data(), lt->pixels.data(), ref.opacityByte, m_tileSizePx);
        }
    }
    return comp;
}

// Seeds an uncached [z/x/y] composite tile from parent [z-1]/[z-2] or child [z+1] tiles
// so zooming in or out immediately displays a full-stack chart preview with zero blanking.
bool PerLayerTileCacheManager::SeedFallbackFromHierarchy(
    const TileCoordKey& key,
    keel::u32 bgPx,
    keel::u64 frameTick
) {
    keel::usize numTilePx = 0;
    if (!keel::checked_mul<keel::usize>(m_tileSizePx, m_tileSizePx, numTilePx)) {
        return false;
    }
    if (CompositeTile* existing = m_compositeTiles.get(key); existing && !existing->pixels.empty()) {
        existing->lastUsedFrame = frameTick;
        return true;
    }

    // 1. Check Parent [z-1] or Grandparent [z-2] and pin its LRU frameTick BEFORE any cache eviction
    keel::i32 ancestorShift = 0;
    TileCoordKey foundParentKey{};
    for (keel::i32 shift = 1; shift <= 2; ++shift) {
        if (key.z - shift < 0) break;
        const TileCoordKey parentKey{key.z - shift, key.x >> shift, key.y >> shift};
        if (CompositeTile* pTile = m_compositeTiles.get(parentKey); pTile && pTile->pixels.size() == numTilePx) {
            pTile->lastUsedFrame = frameTick;
            ancestorShift = shift;
            foundParentKey = parentKey;
            break;
        }
    }

    // 2. Check Children [z+1, 2x+dx, 2y+dy] and pin their LRU frameTick BEFORE any cache eviction
    bool hasChild[2][2]{{false, false}, {false, false}};
    keel::i32 foundChildren = 0;
    if (key.z < 22) {
        for (keel::i32 dy = 0; dy < 2; ++dy) {
            for (keel::i32 dx = 0; dx < 2; ++dx) {
                const TileCoordKey childKey{key.z + 1, (key.x << 1) + dx, (key.y << 1) + dy};
                if (CompositeTile* cTile = m_compositeTiles.get(childKey); cTile && cTile->pixels.size() == numTilePx) {
                    cTile->lastUsedFrame = frameTick;
                    hasChild[dy][dx] = true;
                    ++foundChildren;
                }
            }
        }
    }

    if (ancestorShift == 0 && foundChildren == 0) {
        return false;
    }

    // Perform eviction and map insertion BEFORE taking any pointers into m_compositeTiles!
    if (m_compositeTiles.size() >= m_maxCompositeTiles && !m_compositeTiles.contains(key)) {
        EvictOldestCompositeTiles(
            m_maxCompositeTiles - keel::min<keel::usize>(24u, m_maxCompositeTiles / 4u),
            frameTick
        );
    }

    CompositeTile& comp = m_compositeTiles.get_or_insert(key);
    GetTilePixelPool().AcquireFilled(comp.pixels, numTilePx, bgPx);
    comp.lastUsedFrame = frameTick;
    comp.isComplete = false;

    const CompositeTile* ancestorTile = nullptr;
    if (ancestorShift > 0) {
        if (const CompositeTile* pTile = m_compositeTiles.get(foundParentKey);
            pTile && pTile->pixels.size() == numTilePx) {
            ancestorTile = pTile;
        }
    }

    if (ancestorTile) {
        const keel::u32 subSpan = m_tileSizePx >> ancestorShift;
        const keel::u32 qx = keel::as<keel::u32>(key.x & ((1 << ancestorShift) - 1)) * subSpan;
        const keel::u32 qy = keel::as<keel::u32>(key.y & ((1 << ancestorShift) - 1)) * subSpan;
        for (keel::u32 y = 0; y < m_tileSizePx; ++y) {
            const keel::u32 sy = keel::min(m_tileSizePx - 1u, qy + (y >> ancestorShift));
            const keel::u32* srcRow = ancestorTile->pixels.data() + keel::as<keel::usize>(sy) * m_tileSizePx;
            keel::u32* dstRow = comp.pixels.data() + keel::as<keel::usize>(y) * m_tileSizePx;
            for (keel::u32 x = 0; x < m_tileSizePx; ++x) {
                const keel::u32 sx = keel::min(m_tileSizePx - 1u, qx + (x >> ancestorShift));
                dstRow[x] = srcRow[sx];
            }
        }
    }

    if (foundChildren > 0 && (foundChildren == 4 || ancestorTile != nullptr)) {
        const keel::u32 kHalf = m_tileSizePx >> 1;
        for (keel::i32 dy = 0; dy < 2; ++dy) {
            for (keel::i32 dx = 0; dx < 2; ++dx) {
                if (!hasChild[dy][dx]) continue;
                const TileCoordKey childKey{key.z + 1, (key.x << 1) + dx, (key.y << 1) + dy};
                const CompositeTile* child = m_compositeTiles.get(childKey);
                if (!child || child->pixels.size() != numTilePx) {
                    continue;
                }
                const keel::u32 dstBaseX = keel::as<keel::u32>(dx) * kHalf;
                const keel::u32 dstBaseY = keel::as<keel::u32>(dy) * kHalf;
                for (keel::u32 y = 0; y < kHalf; ++y) {
                    const keel::u32* srcRow = child->pixels.data() + keel::as<keel::usize>(y << 1) * m_tileSizePx;
                    keel::u32* dstRow = comp.pixels.data() + keel::as<keel::usize>(dstBaseY + y) * m_tileSizePx + dstBaseX;
                    for (keel::u32 x = 0; x < kHalf; ++x) {
                        dstRow[x] = srcRow[x << 1];
                    }
                }
            }
        }
    }

    return true;
}

const CachedVectorTile* PerLayerTileCacheManager::RasterizeOrDispatchLayerTile(
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
    keel::usize* outPagedInCount,
    bool hasDeadline,
    keel::u64 deadlineUs,
    keel::usize minPagedBeforeCheck,
    bool* outAbortedOrQueued) {
    const keel::f32 tileSizeF = keel::as<keel::f32>(m_tileSizePx);

    const CellNorm3857Bounds cb = ComputeCellNorm3857Bounds(cell, crs);
    const keel::f32 u0 = cb.u0;
    const keel::f32 u1 = cb.u1;
    const keel::f32 v0 = cb.v0;
    const keel::f32 v1 = cb.v1;
    const keel::f32 spanU = cb.spanU;
    const keel::f32 spanV = cb.spanV;
    const keel::f32 invSpanU = tileSizeF / spanU;
    const keel::f32 invSpanV = tileSizeF / spanV;

    const keel::f32 marginU = spanU * (kTileGuardBandPx / tileSizeF);
    const keel::f32 marginV = spanV * (kTileGuardBandPx / tileSizeF);
    const guikit::Rect queryBox(
        u0 - marginU,
        v0 - marginV,
        (u1 - u0) + 2.0f * marginU,
        (v1 - v0) + 2.0f * marginV
    );

    if (!layer.normBounds.Intersects(queryBox)) {
        MarkTileEmpty(store, key, queryBox, styleHash, frameTick);
        return FindTile(store, key, frameTick);
    }

    const bool layerWindowPending =
        allowAsyncDispatch && layer.isWindowStreamed && GetAsyncStreamer().IsLayerWindowPending(layer.uid);
    if (layer.features.empty()) {
        if (layerWindowPending) {
            if (outAbortedOrQueued) *outAbortedOrQueued = true;
            return nullptr;
        }
        MarkTileEmpty(store, key, queryBox, styleHash, frameTick);
        return FindTile(store, key, frameTick);
    }

    const AsyncLayerTileJobKey asyncKey{
        .layerUid = layer.uid,
        .z = key.z,
        .x = key.x,
        .y = key.y,
        .tileSizePx = m_tileSizePx
    };
    if (allowAsyncDispatch &&
        GetAsyncStreamer().IsLayerTilePending(asyncKey, styleHash, store.lastWindowUpdateFrame)) {
        if (outAbortedOrQueued) *outAbortedOrQueued = true;
        return nullptr;
    }

    m_candidates.clear();
    m_queryIndices.clear();
    layer.spatialIndex.query_box(
        keel::RTreeBox<keel::f32>::from_xywh(queryBox.x, queryBox.y, queryBox.w, queryBox.h),
        [&](keel::u32 fIdx) { m_queryIndices.push_back(fIdx); }
    );
    for (keel::u32 fIdx : m_queryIndices) {
        if (fIdx >= layer.features.size()) continue;
        const geo::GeoFeature& feat = layer.features[fIdx];

        if (!feat.HasPolygons() && !feat.HasLines()) continue;
        if (ShouldSkipFeatureAtZoom(feat, cell.id.z, cfg)) continue;

        const keel::f32 featWpx = feat.normBounds.w * invSpanU;
        const keel::f32 featHpx = feat.normBounds.h * invSpanV;
        const keel::f32 maxDimPx = keel::max(featWpx, featHpx);

        const keel::f32 minFeaturePx = (feat.chartClass == geo::ChartFeatureClass::DepthArea ||
                                    feat.chartClass == geo::ChartFeatureClass::LandArea)
            ? 0.20f
            : ((cfg.lodSimplification || cfg.scaleFiltering) ? 1.35f : 0.65f);
        if (maxDimPx < minFeaturePx) {
            continue;
        }

        if (cfg.lodSimplification || cfg.scaleFiltering) {
            if (cfg.scaleFiltering && cell.id.z <= 11) {
                if (feat.chartClass == geo::ChartFeatureClass::DepthContour) {
                    const keel::f32 minCntPx = (cell.id.z <= 10) ? 4.5f : 2.8f;
                    if (maxDimPx < minCntPx) continue;
                } else if (feat.chartClass == geo::ChartFeatureClass::BuiltUpOrStructure && maxDimPx < 2.8f) {
                    continue;
                }
            }
        }

        const keel::i32 rank = CartographicPainterRank(feat);
        keel::f32 depthKey = 0.0f;
        if (feat.chartClass == geo::ChartFeatureClass::DepthArea) {
            const keel::f32 d = feat.minDepthMeters;
            depthKey = keel::is_nan(d) ? -100.0f : -d;
        }

        m_candidates.push_back({
            .feat = &feat,
            .featIdx = fIdx,
            .painterRank = rank,
            .sortDepth = depthKey,
            .maxDimPx = maxDimPx
        });
    }

    if (m_candidates.empty()) {
        if (layerWindowPending) {
            if (outAbortedOrQueued) *outAbortedOrQueued = true;
            return nullptr;
        }
        MarkTileEmpty(store, key, queryBox, styleHash, frameTick);
        return FindTile(store, key, frameTick);
    }

    keel::sort(m_candidates,
        [](const TileCandidate& a, const TileCandidate& b) noexcept {
            if (a.painterRank != b.painterRank) return a.painterRank < b.painterRank;
            if (a.sortDepth != b.sortDepth) return a.sortDepth < b.sortDepth;
            return a.featIdx < b.featIdx;
        });

    keel::usize missingDiskCount = 0;
    keel::usize totalCandidateVertices = 0;
    for (const TileCandidate& cand : m_candidates) {
        const geo::GeoFeature& feat = *cand.feat;
        totalCandidateVertices += feat.VertexCount();
        if (!feat.geometryLoaded) {
            if (!feat.TryEnsureGeometryLoadedNonBlocking(frameTick)) {
                ++missingDiskCount;
            }
        } else {
            feat.TouchAccess(frameTick);
        }
    }

    const bool isLightweightInRamTile =
        (missingDiskCount == 0) &&
        (m_candidates.size() <= 48u) &&
        (totalCandidateVertices <= 3200u);

    (void)canRunSyncFastPath;
    if (allowAsyncDispatch && !isLightweightInRamTile) {
        AsyncLayerTileJob job{};
        job.jobKey = asyncKey;
        job.topologyHash = store.activeTopologyHash;
        job.styleHash = styleHash;
        job.windowUpdateFrame = store.lastWindowUpdateFrame;
        job.cell = cell;
        job.crs = crs;
        job.queryBox = queryBox;
        job.lodSimplification = cfg.lodSimplification;
        job.lodPixelTolerance = cfg.lodPixelTolerance;
        job.distSq = distSqToCenter;
        job.pager = layer.pager;
        job.candidates.reserve(m_candidates.size());

        for (const TileCandidate& cand : m_candidates) {
            const geo::GeoFeature& feat = *cand.feat;
            job.candidates.push_back(AsyncTileCandidate{
                .featureIndex = cand.featIdx,
                .featureUid = feat.uid,
                .sqliteRowId = feat.sqliteRowId,
                .srsId = feat.srsId,
                .chartClass = feat.chartClass,
                .maxDimPx = cand.maxDimPx,
                .style = ResolveMarineFeatureStyle(layer, feat, cfg, /*bakeFillOpacity=*/false),
                .payload = GetOrCacheInMemoryFeaturePayload(feat)
            });
        }

        GetAsyncStreamer().EnqueueLayerTileJob(keel::move(job));
        if (outAbortedOrQueued) *outAbortedOrQueued = true;
        return nullptr;
    }

    // Synchronous path (used for lightweight already-in-RAM tiles or when asyncBackgroundStreaming is disabled)
    keel::usize newlyPagedInTile = 0;
    m_syncAsyncCandidates.clear();
    m_syncAsyncCandidates.reserve(m_candidates.size());
    for (const TileCandidate& cand : m_candidates) {
        const geo::GeoFeature& feat = *cand.feat;
        if (!feat.geometryLoaded) {
            if (hasDeadline && newlyPagedInTile >= minPagedBeforeCheck &&
                (newlyPagedInTile & 7u) == 0u) {
                if (keel::monotonic_us() >= deadlineUs) {
                    if (outAbortedOrQueued) *outAbortedOrQueued = true;
                    return nullptr;
                }
            }
            if (feat.EnsureGeometryLoaded(frameTick)) {
                ++newlyPagedInTile;
                if (outPagedInCount) {
                    ++(*outPagedInCount);
                }
            }
        }
        if (feat.geometryLoaded) {
            m_syncAsyncCandidates.push_back(AsyncTileCandidate{
                .featureIndex = cand.featIdx,
                .featureUid = feat.uid,
                .sqliteRowId = feat.sqliteRowId,
                .srsId = feat.srsId,
                .chartClass = feat.chartClass,
                .maxDimPx = cand.maxDimPx,
                .style = ResolveMarineFeatureStyle(layer, feat, cfg, /*bakeFillOpacity=*/false),
                .payload = GetOrCacheInMemoryFeaturePayload(feat)
            });
        }
    }

    if (!m_tileCanvas) {
        m_tileCanvas = CreateCanvasThreadSafe();
    }

    EnforceGlobalLayerPixelBudget(frameTick);
    if (store.tiles.size() >= kMaxMetadataTilesPerLayer && !store.tiles.contains(key)) {
        EvictOldestLayerTiles(store, kMaxMetadataTilesPerLayer - 64u, frameTick);
    }

    CachedVectorTile& tile = store.tiles.get_or_insert(key);
    ReleaseTilePixels(tile);
    tile.normQueryBox = queryBox;
    tile.styleHash = styleHash;
    tile.lastUsedFrame = frameTick;
    tile.lastRasterizedFrame = frameTick;
    tile.hasAnyGeometry = false;

    if (m_tileCanvas) {
        RasterizeCandidatesToPixels(
            *m_tileCanvas,
            m_fillPath,
            m_strokePath,
            m_clippedRing,
            m_scratchA,
            m_scratchB,
            m_syncAsyncCandidates,
            cell,
            crs,
            m_tileSizePx,
            queryBox,
            cfg.lodSimplification,
            cfg.lodPixelTolerance,
            tile.pixels,
            tile.hasAnyGeometry
        );
    }

    if (tile.hasAnyGeometry && !tile.pixels.empty()) {
        if (tile.pixels.capacity() > 0u) {
            ++m_pixelLayerTileCount;
        }
    } else {
        GetTilePixelPool().Recycle(tile.pixels);
        tile.hasAnyGeometry = false;
    }

    return &tile;
}

void PerLayerTileCacheManager::EnforceGlobalLayerPixelBudget(
    keel::u64 protectedFrameTick,
    keel::u64 activeCompositeSignature) noexcept {
    if (m_pixelLayerTileCount <= m_maxLayerTiles) {
        return;
    }
    const keel::usize targetCount = m_maxLayerTiles - keel::min<keel::usize>(16u, m_maxLayerTiles / 8u);

    // Pass 1: Prefer evicting Tier-1 pixel buffers whose [z/x/y] already has a complete CompositeTile.
    // Offscreen tiles (lastUsedFrame < protectedFrameTick) are evicted first; current-frame tiles are
    // only reclaimed here if their CompositeTile is already complete AND matches `activeCompositeSignature`
    // (so Fast-path 1 blits directly from the CompositeTile with zero re-rasterization or flicker).
    while (m_pixelLayerTileCount > targetCount) {
        LayerTileStore* victimStore = nullptr;
        TileCoordKey victimKey{};
        keel::u64 oldestFrame = keel::Limits<keel::u64>::kMax;
        for (auto [uid, store] : m_layerStores) {
            for (auto [k, v] : store.tiles) {
                if (v.pixels.capacity() == 0u) {
                    continue;
                }
                const CompositeTile* comp = m_compositeTiles.get(k);
                if (!comp || !comp->isComplete || comp->pixels.empty()) {
                    continue;
                }
                if (v.lastUsedFrame >= protectedFrameTick) {
                    if (activeCompositeSignature == 0ULL || comp->compositeSignature != activeCompositeSignature) {
                        continue;
                    }
                }
                if (v.lastUsedFrame < oldestFrame) {
                    oldestFrame = v.lastUsedFrame;
                    victimStore = &store;
                    victimKey = k;
                }
            }
        }
        if (!victimStore) {
            break;
        }
        if (CachedVectorTile* t = victimStore->tiles.get(victimKey)) {
            ReleaseTilePixels(*t);
        }
        victimStore->tiles.remove(victimKey);
    }

    // Pass 2: Fallback global LRU eviction strictly restricted to offscreen/older Tier-1 tiles
    // (never evicts an in-progress visible cell's Tier-1 tiles while sibling layers are still loading).
    while (m_pixelLayerTileCount > targetCount) {
        LayerTileStore* victimStore = nullptr;
        TileCoordKey victimKey{};
        keel::u64 oldestFrame = keel::Limits<keel::u64>::kMax;
        for (auto [uid, store] : m_layerStores) {
            for (auto [k, v] : store.tiles) {
                if (v.pixels.capacity() > 0u &&
                    v.lastUsedFrame < protectedFrameTick &&
                    v.lastUsedFrame < oldestFrame) {
                    oldestFrame = v.lastUsedFrame;
                    victimStore = &store;
                    victimKey = k;
                }
            }
        }
        if (!victimStore) {
            break;
        }
        if (CachedVectorTile* t = victimStore->tiles.get(victimKey)) {
            ReleaseTilePixels(*t);
        }
        victimStore->tiles.remove(victimKey);
    }
}

void PerLayerTileCacheManager::ReleaseTilePixels(CachedVectorTile& tile) noexcept {
    if (tile.pixels.capacity() > 0u) {
        if (m_pixelLayerTileCount > 0u) {
            --m_pixelLayerTileCount;
        }
        GetTilePixelPool().Recycle(tile.pixels);
    } else {
        tile.pixels.clear();
    }
}

void PerLayerTileCacheManager::ClearStoreTiles(LayerTileStore& store) noexcept {
    for (auto [k, tile] : store.tiles) {
        ReleaseTilePixels(tile);
    }
    store.tiles = {};
}

bool PerLayerTileCacheManager::BlendBasemapTileOrAncestor(
    keel::u32* dstPixels,
    const TileCoordKey& key,
    keel::u8 opacityByte
) const {
    keel::usize numTilePx = 0;
    if (!keel::checked_mul<keel::usize>(m_tileSizePx, m_tileSizePx, numTilePx)) {
        return false;
    }
    if (const CachedBasemapTile* exactPtr = m_basemapTiles.get(key);
        exactPtr && exactPtr->image && exactPtr->image->rgbaPixels.size() == numTilePx) {
        BlendStraightRgbaBasemapOntoComposite(
            dstPixels, exactPtr->image->rgbaPixels.data(), opacityByte, m_tileSizePx
        );
        return true;
    }

    // Check parent [z-1], [z-2], [z-3] for progressive basemap mipmap fallback while exact z downloads
    for (keel::i32 shift = 1; shift <= 3; ++shift) {
        if (key.z - shift < 0) break;
        const TileCoordKey parentKey{key.z - shift, key.x >> shift, key.y >> shift};
        if (const CachedBasemapTile* pPtr = m_basemapTiles.get(parentKey);
            pPtr && pPtr->image && pPtr->image->rgbaPixels.size() == numTilePx) {
            const keel::u32* srcData = pPtr->image->rgbaPixels.data();
            const keel::u32 subSpan = keel::max(1u, m_tileSizePx >> shift);
            const keel::u32 qx = keel::as<keel::u32>(key.x & ((1 << shift) - 1)) * subSpan;
            const keel::u32 qy = keel::as<keel::u32>(key.y & ((1 << shift) - 1)) * subSpan;

            m_upscaledBasemapScratch.resize(numTilePx);
            for (keel::u32 y = 0; y < m_tileSizePx; ++y) {
                const keel::u32 sy = keel::min(m_tileSizePx - 1u, qy + (y >> shift));
                const keel::u32* srcRow = srcData + keel::as<keel::usize>(sy) * m_tileSizePx;
                keel::u32* dstRow = m_upscaledBasemapScratch.data() + keel::as<keel::usize>(y) * m_tileSizePx;
                for (keel::u32 x = 0; x < m_tileSizePx; ++x) {
                    const keel::u32 sx = keel::min(m_tileSizePx - 1u, qx + (x >> shift));
                    dstRow[x] = srcRow[sx];
                }
            }
            BlendStraightRgbaBasemapOntoComposite(
                dstPixels, m_upscaledBasemapScratch.data(), opacityByte, m_tileSizePx
            );
            return false; // Ancestor preview used; exact z tile is still pending
        }
    }
    return false;
}

void PerLayerTileCacheManager::EvictOldestLayerTiles(LayerTileStore& store, keel::usize targetCount, keel::u64 protectedFrameTick) noexcept {
    while (store.tiles.size() > targetCount && !store.tiles.empty()) {
        bool found = false;
        TileCoordKey oldestKey{};
        keel::u64 oldestFrame = keel::Limits<keel::u64>::kMax;
        for (auto [k, v] : store.tiles) {
            if (v.lastUsedFrame < protectedFrameTick && v.lastUsedFrame < oldestFrame) {
                oldestFrame = v.lastUsedFrame;
                oldestKey = k;
                found = true;
            }
        }
        if (!found) {
            break;
        }
        if (CachedVectorTile* t = store.tiles.get(oldestKey)) {
            ReleaseTilePixels(*t);
        }
        store.tiles.remove(oldestKey);
    }
}

void PerLayerTileCacheManager::EvictOldestCompositeTiles(
    keel::usize targetCount,
    keel::u64 protectedFrameTick
) noexcept {
    while (m_compositeTiles.size() > targetCount && !m_compositeTiles.empty()) {
        bool found = false;
        TileCoordKey oldestKey{};
        keel::u64 oldestFrame = keel::Limits<keel::u64>::kMax;
        for (auto [k, v] : m_compositeTiles) {
            if (v.lastUsedFrame < protectedFrameTick && v.lastUsedFrame < oldestFrame) {
                oldestFrame = v.lastUsedFrame;
                oldestKey = k;
                found = true;
            }
        }
        if (!found) {
            break;
        }
        if (CompositeTile* victim = m_compositeTiles.get(oldestKey)) {
            GetTilePixelPool().Recycle(victim->pixels);
        }
        m_compositeTiles.remove(oldestKey);
    }
}

PerLayerTileCacheManager& GetTileCache() {
    static PerLayerTileCacheManager s_cache;
    return s_cache;
}

[[nodiscard]] keel::Arc<geo::IRasterTileProvider> ResolveActiveBasemapProvider(
    const GisRenderConfig& cfg
) {
    keel::Arc<geo::IRasterTileProvider> resolved;
    if (cfg.customBasemapProvider) {
        resolved = cfg.customBasemapProvider;
    } else if (cfg.showOsmBasemap) {
        if (auto defaultProv = GisRenderEngine::GetDefaultBasemapProvider()) {
            resolved = keel::move(defaultProv);
        } else {
            static keel::Arc<geo::IRasterTileProvider> s_defaultOsmProvider = geo::CreateOsmRasterTileProvider();
            resolved = s_defaultOsmProvider;
        }
    }
    if (resolved) {
        keel::LockGuard<keel::Mutex> lk(GetDefaultBasemapMutex());
        GetLastActiveBasemapStorage() = resolved;
    }
    return resolved;
}


} // namespace guikit::geo::detail
