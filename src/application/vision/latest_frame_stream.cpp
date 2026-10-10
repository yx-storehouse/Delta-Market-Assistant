#include "latest_frame_stream.h"
#include <QUuid>
#include <algorithm>
#include <chrono>

namespace relink::vision {
using namespace runtime::observation;
namespace {
CaptureReply failure(const QString& error, CaptureStatus status = CaptureStatus::Failed) {
    CaptureReply r; r.status = status; r.errorCode = error; return r;
}
bool validPixels(const FrameEnvelope& f) {
    return f.sourceKind == "dxgi" && !f.frameId.isEmpty() && f.frameId.size() <= 200
        && f.freshnessBasis == FreshnessBasis::SourceTimestamp
        && f.sourceMonoMs >= 0 && f.sourceUncertaintyMs >= 0 && f.sourceUncertaintyMs <= 1000
        && f.captureStartMonoMs >= 0 && f.captureEndMonoMs >= f.captureStartMonoMs
        && f.sourceMonoMs <= f.captureEndMonoMs && f.pixelFormat == "BGRA8"
        && f.width > 0 && f.width <= 8192 && f.height > 0 && f.height <= 8192
        && f.strideBytes >= f.width * 4 && f.strideBytes <= 32768
        && f.validBytes == qint64(f.strideBytes) * f.height && f.validBytes <= 134217728
        && f.pixels.size() == f.validBytes && !f.windowRef.isEmpty();
}
}

LatestFramePump::LatestFramePump(Pull pull, Cancel cancel, Clock clock)
    : m_pull(std::move(pull)), m_cancel(std::move(cancel)), m_clock(std::move(clock)) {}
LatestFramePump::~LatestFramePump() { stop(); }
void LatestFramePump::start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_started || m_stopped) return;
    m_started = true; m_idleDeadline = m_clock() + 4000;
    m_thread = std::thread([this] { run(); });
}
void LatestFramePump::stop() {
    bool cancel = false;
    { std::lock_guard<std::mutex> lock(m_mutex); m_stopped = true; m_hasLatest = false; m_latest = {};
      cancel = !m_cancelIssued; m_cancelIssued = true; }
    m_changed.notify_all();
    if (cancel && m_cancel) m_cancel();
    if (m_thread.joinable()) m_thread.join();
}
void LatestFramePump::wake() { m_changed.notify_all(); }
void LatestFramePump::run() {
    try {
        while (true) {
            { std::lock_guard<std::mutex> lock(m_mutex);
              if (m_stopped) break;
              if (m_clock() > m_idleDeadline) { m_error = "E_FRAME_STREAM_IDLE"; break; } }
            auto r = m_pull();
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopped) break;
            if (r.status != CaptureStatus::Captured) {
                m_error = r.errorCode.isEmpty() ? QStringLiteral("E_FRAME_STREAM_CAPTURE") : r.errorCode; break;
            }
            if (!validPixels(r.frame) || r.frame.frameId == m_lastFrameId
                || r.frame.sourceMonoMs <= m_lastSource || r.frame.captureEndMonoMs > m_clock()) {
                m_error = "E_FRAME_STREAM_INVALID_FRAME"; break;
            }
            m_lastFrameId = r.frame.frameId; m_lastSource = r.frame.sourceMonoMs;
            if (m_hasLatest) ++m_replaced;
            m_latest = std::move(r.frame); m_hasLatest = true; ++m_produced;
            m_changed.notify_all();
        }
    } catch (...) { std::lock_guard<std::mutex> lock(m_mutex); m_error = "E_FRAME_STREAM_EXCEPTION"; }
    { std::lock_guard<std::mutex> lock(m_mutex); m_stopped = true; m_hasLatest = false; m_latest = {}; }
    m_changed.notify_all();
}
CaptureReply LatestFramePump::take(qint64 barrier, qint64 deadline, qint64 maxAge,
                                  const std::atomic<bool>& cancelled) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_started || barrier < 0 || deadline < barrier || maxAge < 1 || maxAge > 60000)
        return failure("E_FRAME_STREAM_REQUEST");
    m_idleDeadline = std::max(m_idleDeadline, deadline + 4000);
    while (true) {
        if (cancelled.load()) return failure("E_CAPTURE_CANCELLED", CaptureStatus::Cancelled);
        if (m_stopped) return failure(m_error.isEmpty() ? QStringLiteral("E_FRAME_STREAM_STOPPED") : m_error);
        const auto now = m_clock();
        if (now > deadline) return failure("E_CAPTURE_DEADLINE");
        if (m_hasLatest) {
            const auto lower = m_latest.sourceMonoMs - m_latest.sourceUncertaintyMs;
            if (lower >= barrier && lower <= now && now - lower <= maxAge) {
                CaptureReply r; r.status = CaptureStatus::Captured; r.frame = std::move(m_latest);
                m_latest = {}; m_hasLatest = false; ++m_consumed; return r;
            }
            m_latest = {}; m_hasLatest = false; ++m_stale;
        }
        m_changed.wait_for(lock, std::chrono::milliseconds(10));
    }
}
QJsonObject LatestFramePump::metrics() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return {{"mode","latest_frame_stream"},{"started",m_started},{"stopped",m_stopped},
        {"pending_slot_capacity",1},{"pending_frames",m_hasLatest ? 1 : 0},
        {"produced",double(m_produced)},{"replaced_without_queue",double(m_replaced)},
        {"consumed",double(m_consumed)},{"rejected_pre_barrier_or_stale",double(m_stale)},
        {"image_file_writes",0},{"error",m_error},{"request_source_barrier_unchanged",true}};
}

struct DxgiFrameStream::State {
    TargetWindow target;
    std::mutex mutex;
    std::shared_ptr<DxgiCaptureResources> resources = std::make_shared<DxgiCaptureResources>();
    std::shared_ptr<DxgiObservationSource> active;
    ObservationDemand activeDemand;
    QJsonObject resourceMetrics;
    std::unique_ptr<LatestFramePump> pump;
    std::atomic<bool> stopping{false};
    CaptureReply pull() {
        if (stopping.load()) return failure("E_CAPTURE_CANCELLED",CaptureStatus::Cancelled);
        ObservationDemand d;
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        d.demandId = "stream:" + id;
        d.context = {"collection-stream",id,captureClockDomain(),"acquire",0,1};
        d.createdMonoMs = d.notBeforeMonoMs = captureClockMs(); d.deadlineMonoMs = d.createdMonoMs + 1500;
        d.maxFrameAgeMs = 1000;
        d.roiSpecs = {{"client","client_physical_px",0,0,target.clientRect.width(),target.clientRect.height(),1}};
        auto source = std::make_shared<DxgiObservationSource>(target,d,resources);
        { std::lock_guard<std::mutex> lock(mutex); active = source; activeDemand = d;
          if (stopping.load()) source->cancel(d.demandId,d.context); }
        auto r = source->capture({d,"stream-request:" + id,1,captureClockMs()});
        { std::lock_guard<std::mutex> lock(mutex); active.reset(); resourceMetrics = resources->metrics(); }
        return r;
    }
    void cancel() {
        stopping.store(true);
        std::lock_guard<std::mutex> lock(mutex);
        if (active) active->cancel(activeDemand.demandId,activeDemand.context);
    }
};
DxgiFrameStream::DxgiFrameStream() = default;
DxgiFrameStream::~DxgiFrameStream() { stop(); }
QString DxgiFrameStream::ensure(const TargetWindow& target) {
    if (m_state) {
        if (!sameCaptureResourceTarget(m_state->target,target)) { stop(); return "E_FRAME_STREAM_TARGET_CHANGED"; }
        const auto status = m_state->pump->metrics();
        return status["stopped"].toBool() ? QStringLiteral("E_FRAME_STREAM_STOPPED") : QString();
    }
    if (const auto e = validateTargetWindow(target); !e.isEmpty()) return e;
    m_state = std::make_unique<State>(); m_state->target = target;
    auto* s = m_state.get();
    s->pump = std::make_unique<LatestFramePump>([s] { return s->pull(); },[s] { s->cancel(); });
    s->pump->start(); return {};
}
void DxgiFrameStream::stop() { if (m_state) { if(m_state->pump)m_state->pump->stop(); m_state.reset(); } }
LatestFramePump* DxgiFrameStream::pump() { return m_state ? m_state->pump.get() : nullptr; }
QJsonObject DxgiFrameStream::metrics() const {
    if (!m_state) return {{"mode","latest_frame_stream"},{"started",false},{"stopped",true}};
    auto r = m_state->pump->metrics();
    { std::lock_guard<std::mutex> lock(m_state->mutex); r["resource_owner"] = m_state->resourceMetrics; }
    return r;
}

LatestFrameObservationSource::LatestFrameObservationSource(LatestFramePump& pump, TargetWindow target,
        ObservationDemand demand, std::function<QString(const TargetWindow&)> validate)
    : m_pump(pump),m_target(std::move(target)),m_demand(std::move(demand)),m_validate(std::move(validate)) {}
void LatestFrameObservationSource::cancel(const QString& id,const runtime::RuntimeContext& context) {
    auto d = m_demand; d.context = context;
    if (id == m_demand.demandId && sameObservationDemand(d,m_demand)) { m_cancelled.store(true); m_pump.wake(); }
}
CaptureReply LatestFrameObservationSource::capture(const CaptureRequest& request) {
    auto fail = [&](const QString& e) { auto r = failure(e); r.requestId = request.requestId; r.requestNumber = request.requestNumber; return r; };
    if (m_busy.exchange(true)) return fail("E_CAPTURE_BUSY");
    struct Reset { std::atomic<bool>& b; ~Reset(){ b.store(false); } } reset{m_busy};
    ObservationAdapter validator(this);
    if (!validator.validateDemand(request.demand).valid || !sameObservationDemand(request.demand,m_demand)
        || request.requestId.isEmpty() || request.requestId.size()>200 || request.requestNumber<=m_lastRequest
        || request.requestNumber>m_demand.frameBudget || request.requestedMonoMs<m_demand.notBeforeMonoMs
        || request.requestedMonoMs>m_demand.deadlineMonoMs || m_demand.context.clockDomainId!=captureClockDomain())
        return fail("E_CAPTURE_REQUEST_INVALID");
    if(m_cancelled.load()) {
        auto r=fail("E_CAPTURE_CANCELLED");r.status=CaptureStatus::Cancelled;return r;
    }
    const auto now=captureClockMs();
    if(now<request.requestedMonoMs ||now>m_demand.deadlineMonoMs)return fail("E_CAPTURE_DEADLINE");
    for(const auto& roi:m_demand.roiSpecs)
        if(qint64(roi.x)+roi.width>m_target.clientRect.width() ||qint64(roi.y)+roi.height>m_target.clientRect.height())
            return fail("E_ROI_OUTSIDE_FRAME");
    if (const auto e=m_validate(m_target);!e.isEmpty()) return fail(e);
    m_lastRequest=request.requestNumber;
    auto r=m_pump.take(std::max(request.requestedMonoMs,m_demand.notBeforeMonoMs),m_demand.deadlineMonoMs,m_demand.maxFrameAgeMs,m_cancelled);
    r.requestId=request.requestId;r.requestNumber=request.requestNumber;
    if(r.status!=CaptureStatus::Captured)return r;
    if(const auto e=m_validate(m_target);!e.isEmpty())return fail(e);
    const auto expected=QStringLiteral("window:%1:%2:%3").arg(m_target.pid).arg(m_target.hwnd).arg(m_target.processCreated);
    if(r.frame.windowRef!=expected ||r.frame.width!=m_target.clientRect.width() ||r.frame.height!=m_target.clientRect.height()
        ||r.frame.dpiX!=m_target.dpi ||r.frame.dpiY!=m_target.dpi)return fail("E_FRAME_STREAM_BINDING");
    r.frame.demandId=m_demand.demandId;r.frame.context=m_demand.context;
    r.frame.captureSessionId=m_demand.context.sessionId;
    r.frame.leaseId=r.frame.frameId+":delivery:"+request.requestId;
    r.frame.slotIndex=(request.requestNumber-1)%2;r.frame.slotGeneration=request.requestNumber;
    return r;
}
}
