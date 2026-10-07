#include "observation_adapter.h"

#include <QtGlobal>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <utility>

namespace relink::runtime::observation {
namespace {

constexpr qint64 kMaxPixelBytes = 134217728;
constexpr int kMaxDimension = 8192;
constexpr int kMaxStride = 32768;
constexpr qint64 kMaxSafeInteger = 9007199254740991;
constexpr int kMaxMetadataLength = 200;

bool safeInteger(qint64 value) { return value >= 0 && value <= kMaxSafeInteger; }
bool validId(const QString& value) { return !value.isEmpty() && value.size() <= kMaxMetadataLength; }
bool supportedSource(const QString& value) {
    return value == QStringLiteral("replay") || value == QStringLiteral("wgc") || value == QStringLiteral("dxgi");
}

QString contextError(const RuntimeContext& c) {
    if (!validId(c.runId) || !validId(c.sessionId) || !validId(c.clockDomainId) || !validId(c.stepId)) {
        return QStringLiteral("E_CONTEXT_INVALID");
    }
    if (!safeInteger(c.cancelEpoch) || !safeInteger(c.viewportGeneration)) {
        return QStringLiteral("E_CONTEXT_INVALID");
    }
    return {};
}

bool safeMultiply(qint64 a, qint64 b, qint64* result) {
    if (a < 0 || b < 0 || a > std::numeric_limits<qint64>::max() / std::max<qint64>(1, b)) return false;
    *result = a * b;
    return true;
}

QString idOr(const QString& id, const QString& fallback) {
    return id.isEmpty() ? fallback : id;
}

} // namespace

QString toString(ObservationMode value) {
    return value == ObservationMode::OneShot ? QStringLiteral("one_shot") : QStringLiteral("bounded_watch");
}

QString toString(ObservationPurpose value) {
    switch (value) {
    case ObservationPurpose::PageCheck: return QStringLiteral("page_check");
    case ObservationPurpose::ListingFields: return QStringLiteral("listing_fields");
    case ObservationPurpose::Receipt: return QStringLiteral("receipt");
    case ObservationPurpose::Revalidate: return QStringLiteral("revalidate");
    case ObservationPurpose::Countdown: return QStringLiteral("countdown");
    }
    return QStringLiteral("unknown");
}

QString toString(FreshnessBasis value) {
    switch (value) {
    case FreshnessBasis::SourceTimestamp: return QStringLiteral("source_timestamp");
    case FreshnessBasis::PostBarrierCapture: return QStringLiteral("post_barrier_capture");
    case FreshnessBasis::Unproven: return QStringLiteral("unproven");
    }
    return QStringLiteral("unknown");
}

QString toString(ObservationState value) {
    switch (value) {
    case ObservationState::Created: return QStringLiteral("Created");
    case ObservationState::Active: return QStringLiteral("Active");
    case ObservationState::Satisfied: return QStringLiteral("Satisfied");
    case ObservationState::Expired: return QStringLiteral("Expired");
    case ObservationState::Cancelled: return QStringLiteral("Cancelled");
    case ObservationState::Rejected: return QStringLiteral("Rejected");
    case ObservationState::Closed: return QStringLiteral("Closed");
    }
    return QStringLiteral("Unknown");
}

ObservationAdapter::ObservationAdapter(IObservationSource* source)
    : m_source(source) {}

ObservationDemandValidation ObservationAdapter::validateDemand(const ObservationDemand& demand) const {
    ObservationDemandValidation result;
    if (!validId(demand.demandId)) { result.errorCode = QStringLiteral("E_DEMAND_ID"); return result; }
    if (const QString error = contextError(demand.context); !error.isEmpty()) {
        result.errorCode = error; return result;
    }
    if (!safeInteger(demand.createdMonoMs) || !safeInteger(demand.notBeforeMonoMs)
        || !safeInteger(demand.deadlineMonoMs) || demand.notBeforeMonoMs < demand.createdMonoMs
        || demand.deadlineMonoMs <= demand.createdMonoMs || demand.deadlineMonoMs < demand.notBeforeMonoMs) {
        result.errorCode = QStringLiteral("E_INVALID_DEADLINE"); return result;
    }
    if ((demand.mode != ObservationMode::OneShot && demand.mode != ObservationMode::BoundedWatch)
        || toString(demand.purpose) == QStringLiteral("unknown")) {
        result.errorCode = QStringLiteral("E_DEMAND_MODE_OR_PURPOSE"); return result;
    }
    if (demand.maxFrameAgeMs < 1 || demand.maxFrameAgeMs > 60000) {
        result.errorCode = QStringLiteral("E_MAX_FRAME_AGE"); return result;
    }
    if (demand.frameBudget < 1 || demand.frameBudget > 600) {
        result.errorCode = QStringLiteral("E_FRAME_BUDGET"); return result;
    }
    if (demand.minIntervalMs < 0 || demand.minIntervalMs > 60000) {
        result.errorCode = QStringLiteral("E_MIN_INTERVAL"); return result;
    }
    if (demand.mode == ObservationMode::OneShot && (demand.frameBudget != 1 || demand.minIntervalMs != 0)) {
        result.errorCode = QStringLiteral("E_ONE_SHOT_BUDGET"); return result;
    }
    if (demand.mode == ObservationMode::BoundedWatch && demand.deadlineMonoMs <= demand.createdMonoMs) {
        result.errorCode = QStringLiteral("E_INVALID_DEADLINE"); return result;
    }
    if (demand.persistence != QStringLiteral("none")) {
        result.errorCode = QStringLiteral("E_PERSISTENCE_NOT_ALLOWED"); return result;
    }
    if (demand.roiSpecs.isEmpty() || demand.roiSpecs.size() > 64) {
        result.errorCode = QStringLiteral("E_ROI_SPECS"); return result;
    }
    for (const RoiSpec& roi : demand.roiSpecs) {
        if (!validId(roi.roiId) || roi.coordinateSpace != QStringLiteral("client_physical_px")
            || roi.x < 0 || roi.y < 0 || roi.width < 1 || roi.width > kMaxDimension
            || roi.height < 1 || roi.height > kMaxDimension || roi.x > kMaxDimension - roi.width
            || roi.y > kMaxDimension - roi.height || (roi.transformVersion < 1 || !safeInteger(roi.transformVersion))) {
            result.errorCode = QStringLiteral("E_ROI_INVALID"); return result;
        }
    }
    result.valid = true;
    return result;
}

bool ObservationAdapter::start(const ObservationDemand& demand, qint64 nowMonoMs, QString* errorCode) {
    if (errorCode) errorCode->clear();
    auto reject = [&](const QString& error) {
        m_snapshot.state = ObservationState::Rejected;
        rememberError(error);
        if (errorCode) *errorCode = error;
        return false;
    };
    if (m_snapshot.state != ObservationState::Created) {
        if (errorCode) *errorCode = QStringLiteral("E_ALREADY_STARTED");
        return false;
    }
    const ObservationDemandValidation validation = validateDemand(demand);
    if (!validation.valid) return reject(validation.errorCode);
    if (!safeInteger(nowMonoMs)) return reject(QStringLiteral("E_CLOCK_RANGE"));
    if (nowMonoMs < demand.createdMonoMs) return reject(QStringLiteral("E_CLOCK_BEFORE_CREATED"));
    if (!m_source || !supportedSource(m_source->sourceKind())) return reject(QStringLiteral("E_SOURCE_UNAVAILABLE"));
    m_demand = demand;
    m_expectedSourceKind = m_source->sourceKind();
    m_hasDemand = true;
    m_snapshot = {};
    m_snapshot.state = ObservationState::Active;
    m_lastObservedNowMonoMs = nowMonoMs;
    if (expireIfDue(nowMonoMs)) {
        if (errorCode) *errorCode = m_snapshot.lastError;
        return false;
    }
    return true;
}

bool ObservationAdapter::contextMatches(const RuntimeContext& lhs, const RuntimeContext& rhs) const {
    return lhs.runId == rhs.runId && lhs.sessionId == rhs.sessionId
        && lhs.clockDomainId == rhs.clockDomainId && lhs.stepId == rhs.stepId
        && lhs.cancelEpoch == rhs.cancelEpoch && lhs.viewportGeneration == rhs.viewportGeneration;
}

bool ObservationAdapter::observeNow(qint64 nowMonoMs) {
    if (!safeInteger(nowMonoMs)) { rememberError(QStringLiteral("E_CLOCK_RANGE")); return false; }
    if (nowMonoMs < m_lastObservedNowMonoMs) { rememberError(QStringLiteral("E_CLOCK_BACKWARD")); return false; }
    m_lastObservedNowMonoMs = nowMonoMs;
    return true;
}

void ObservationAdapter::updateResourceDrainState() {
    m_snapshot.captureInFlight = m_captureInFlight;
    m_snapshot.pendingFrame = m_hasPendingFrame;
    m_snapshot.resourcesDrained = !m_captureInFlight && !m_hasPendingFrame && !m_snapshot.frameInUse;
    m_snapshot.resourcesDraining = m_snapshot.state != ObservationState::Active && !m_snapshot.resourcesDrained;
    if (m_closeRequested && m_snapshot.resourcesDrained) m_snapshot.state = ObservationState::Closed;
}

void ObservationAdapter::cancelCaptureForTerminalState() {
    // A cancellation request is not proof that an OS callback or reader has
    // stopped. Keep the in-flight token/lease until its matching callback/release.
    if (m_hasDemand && m_source && !m_captureCancellationSent) {
        m_source->cancel(m_demand.demandId, m_demand.context);
        m_captureCancellationSent = true;
    }
    m_pendingFrame = {};
    m_hasPendingFrame = false;
    updateResourceDrainState();
}

bool ObservationAdapter::expireIfDue(qint64 nowMonoMs) {
    if (!m_hasDemand || m_snapshot.state != ObservationState::Active || nowMonoMs <= m_demand.deadlineMonoMs) return false;
    m_snapshot.state = ObservationState::Expired;
    rememberError(QStringLiteral("E_OBSERVATION_DEADLINE"));
    cancelCaptureForTerminalState();
    return true;
}

bool ObservationAdapter::deadlineTick(qint64 nowMonoMs) {
    if (!observeNow(nowMonoMs) || !m_hasDemand || m_snapshot.state != ObservationState::Active
        || nowMonoMs < m_demand.deadlineMonoMs) return false;
    m_snapshot.state = ObservationState::Expired;
    rememberError(QStringLiteral("E_OBSERVATION_DEADLINE"));
    cancelCaptureForTerminalState();
    return true;
}

bool ObservationAdapter::canAcquire(qint64 nowMonoMs) const {
    if (!safeInteger(nowMonoMs) || nowMonoMs < m_lastObservedNowMonoMs) return false;
    if (!m_hasDemand || m_snapshot.state != ObservationState::Active || m_source == nullptr) return false;
    if (m_captureInFlight || m_hasPendingFrame || m_snapshot.frameInUse) return false;
    if (m_snapshot.requestCount >= m_demand.frameBudget) return false;
    if (nowMonoMs < m_demand.notBeforeMonoMs || nowMonoMs > m_demand.deadlineMonoMs) return false;
    if (m_snapshot.requestCount > 0 && nowMonoMs - m_lastAcquireMonoMs < m_demand.minIntervalMs) return false;
    return true;
}

CaptureRequest ObservationAdapter::beginAcquire(qint64 nowMonoMs, QString* errorCode) {
    if (errorCode) errorCode->clear();
    CaptureRequest request;
    auto fail = [&](const QString& error) {
        rememberError(error);
        if (errorCode) *errorCode = error;
        return request;
    };
    if (!m_hasDemand) return fail(QStringLiteral("E_NO_DEMAND"));
    if (!observeNow(nowMonoMs)) return fail(m_snapshot.lastError);
    if (expireIfDue(nowMonoMs)) return fail(m_snapshot.lastError);
    if (!canAcquire(nowMonoMs)) {
        QString error = QStringLiteral("E_ACQUIRE_NOT_DUE");
        if (m_snapshot.state != ObservationState::Active) error = QStringLiteral("E_OBSERVATION_NOT_ACTIVE");
        else if (m_captureInFlight) error = QStringLiteral("E_CAPTURE_IN_FLIGHT");
        else if (m_snapshot.requestCount >= m_demand.frameBudget) error = QStringLiteral("E_FRAME_BUDGET_EXHAUSTED");
        else if (m_snapshot.frameInUse || m_hasPendingFrame) error = QStringLiteral("E_FRAME_NOT_RELEASED");
        return fail(error);
    }
    request.demand = m_demand;
    request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.requestNumber = ++m_requestNumber;
    request.requestedMonoMs = nowMonoMs;
    ++m_snapshot.requestCount;
    m_captureInFlight = true;
    m_activeRequestNumber = request.requestNumber;
    m_activeRequestId = request.requestId;
    m_lastAcquireMonoMs = nowMonoMs;
    updateResourceDrainState();
    return request;
}

CaptureReply ObservationAdapter::completeCapture(const CaptureReply& reply, qint64 nowMonoMs) {
    CaptureReply result = reply;
    auto fail = [&](const QString& error, CaptureStatus status = CaptureStatus::Failed) {
        result.status = status;
        result.errorCode = error;
        result.frame = {}; // do not retain rejected/late pixel storage
        rememberError(error);
        return result;
    };
    if (!observeNow(nowMonoMs)) return fail(m_snapshot.lastError);
    expireIfDue(nowMonoMs);
    if (!m_captureInFlight) return fail(QStringLiteral("E_NO_CAPTURE_REQUEST"));
    if (reply.requestId != m_activeRequestId || reply.requestNumber != m_activeRequestNumber) {
        return fail(QStringLiteral("E_CAPTURE_REQUEST_MISMATCH"));
    }
    m_captureInFlight = false;
    m_activeRequestId.clear();
    m_activeRequestNumber = 0;
    updateResourceDrainState();
    if (m_snapshot.state != ObservationState::Active) {
        return fail(QStringLiteral("E_OBSERVATION_CANCELLED"), CaptureStatus::Cancelled);
    }
    if (reply.status != CaptureStatus::Captured) {
        result.errorCode = idOr(reply.errorCode, reply.status == CaptureStatus::Empty
            ? QStringLiteral("E_EMPTY_FRAME") : QStringLiteral("E_CAPTURE_FAILED"));
        result.frame = {};
        rememberError(result.errorCode);
        if (m_demand.mode == ObservationMode::OneShot) {
            m_snapshot.state = ObservationState::Expired;
            updateResourceDrainState();
        }
        return result;
    }
    m_pendingFrame = reply.frame;
    m_hasPendingFrame = true;
    updateResourceDrainState();
    return result;
}

CaptureReply ObservationAdapter::acquire(qint64 nowMonoMs) {
    QString error;
    const CaptureRequest request = beginAcquire(nowMonoMs, &error);
    if (request.requestNumber == 0) {
        CaptureReply failure;
        failure.errorCode = error;
        return failure;
    }
    return completeCapture(m_source->capture(request), nowMonoMs);
}

bool ObservationAdapter::watchTick(qint64 nowMonoMs) {
    if (!observeNow(nowMonoMs) || expireIfDue(nowMonoMs) || !canAcquire(nowMonoMs)) return false;
    const int before = m_snapshot.requestCount;
    acquire(nowMonoMs);
    return m_snapshot.requestCount > before;
}

QString ObservationAdapter::validateFrameShape(const FrameEnvelope& frame) const {
    if (!validId(frame.frameId) || !validId(frame.demandId) || !validId(frame.captureSessionId)
        || !validId(frame.leaseId)) return QStringLiteral("E_FRAME_METADATA");
    if (!supportedSource(frame.sourceKind) || frame.sourceKind != m_expectedSourceKind) return QStringLiteral("E_SOURCE_KIND");
    if (!validId(frame.windowRef)) return QStringLiteral("E_WINDOW_REF");
    if (frame.width < 1 || frame.width > kMaxDimension || frame.height < 1 || frame.height > kMaxDimension
        || frame.dpiX < 48 || frame.dpiX > 768 || frame.dpiY < 48 || frame.dpiY > 768
        || frame.pixelFormat != QStringLiteral("BGRA8")) return QStringLiteral("E_FRAME_SHAPE");
    const qint64 minStride = static_cast<qint64>(frame.width) * 4;
    if (frame.strideBytes < minStride || frame.strideBytes > kMaxStride) return QStringLiteral("E_SHM_BOUNDS");
    qint64 validBytes = 0;
    if (!safeMultiply(frame.strideBytes, frame.height, &validBytes) || validBytes > kMaxPixelBytes
        || frame.validBytes != validBytes || frame.pixels.size() != validBytes) return QStringLiteral("E_SHM_BOUNDS");
    if (!safeInteger(frame.captureStartMonoMs) || !safeInteger(frame.captureEndMonoMs) || frame.captureEndMonoMs < frame.captureStartMonoMs) {
        return QStringLiteral("E_CAPTURE_TIME");
    }
    if (frame.slotIndex < 0 || frame.slotIndex > 1 || (frame.slotGeneration < 1 || !safeInteger(frame.slotGeneration))) {
        return QStringLiteral("E_LEASE_INVALID");
    }
    if (frame.freshnessBasis == FreshnessBasis::SourceTimestamp) {
        if (!safeInteger(frame.sourceMonoMs) || frame.sourceUncertaintyMs < 0 || frame.sourceUncertaintyMs > 60000) {
            return QStringLiteral("E_FRESHNESS_METADATA");
        }
    } else if ((frame.freshnessBasis != FreshnessBasis::PostBarrierCapture && frame.freshnessBasis != FreshnessBasis::Unproven)
               || frame.sourceMonoMs != -1 || frame.sourceUncertaintyMs != -1) {
        return QStringLiteral("E_FRESHNESS_METADATA");
    }
    return {};
}

FrameValidationResult ObservationAdapter::validateAndAccept(const FrameEnvelope& frame, qint64 nowMonoMs) {
    FrameValidationResult result;
    if (!m_hasDemand || m_snapshot.state != ObservationState::Active) {
        result.errorCode = QStringLiteral("E_OBSERVATION_NOT_ACTIVE");
    } else if (frame.demandId != m_demand.demandId || !contextMatches(frame.context, m_demand.context)
               || frame.captureSessionId != m_demand.context.sessionId) {
        result.errorCode = QStringLiteral("E_CONTEXT_MISMATCH");
    } else if (const QString shapeError = validateFrameShape(frame); !shapeError.isEmpty()) {
        result.errorCode = shapeError;
    } else if (nowMonoMs > m_demand.deadlineMonoMs) {
        result.errorCode = QStringLiteral("E_OBSERVATION_DEADLINE");
    } else if (frame.captureEndMonoMs > nowMonoMs || (frame.freshnessBasis == FreshnessBasis::SourceTimestamp && frame.sourceMonoMs > nowMonoMs)) {
        // A callback cannot publish a frame whose API capture interval has not
        // ended in the coordinator's clock domain.  Treat it as an unproven
        // future frame rather than allowing the source timestamp to hide it.
        result.errorCode = QStringLiteral("E_STALE_OR_UNPROVEN_FRAME");
    } else {
        qint64 lowerBound = -1;
        if (frame.freshnessBasis == FreshnessBasis::SourceTimestamp) {
            if (frame.sourceMonoMs < frame.sourceUncertaintyMs) {
                result.errorCode = QStringLiteral("E_STALE_OR_UNPROVEN_FRAME");
            } else {
                lowerBound = frame.sourceMonoMs - frame.sourceUncertaintyMs;
            }
        } else if (frame.freshnessBasis == FreshnessBasis::PostBarrierCapture) {
            lowerBound = frame.captureStartMonoMs;
        } else {
            result.errorCode = QStringLiteral("E_STALE_OR_UNPROVEN_FRAME");
        }
        if (result.errorCode.isEmpty()) {
            result.lowerBoundMonoMs = lowerBound;
            if (lowerBound < m_demand.notBeforeMonoMs || lowerBound > nowMonoMs
                || nowMonoMs - lowerBound > m_demand.maxFrameAgeMs) {
                result.errorCode = QStringLiteral("E_STALE_OR_UNPROVEN_FRAME");
            }
        }
        if (result.errorCode.isEmpty()) {
            for (const RoiSpec& roi : m_demand.roiSpecs) {
                if (roi.x < 0 || roi.y < 0 || roi.x > frame.width - roi.width
                    || roi.y > frame.height - roi.height) {
                    result.errorCode = QStringLiteral("E_ROI_OUT_OF_BOUNDS");
                    break;
                }
            }
        }
    }
    if (!result.errorCode.isEmpty()) {
        ++m_snapshot.rejectionCount;
        rememberError(result.errorCode);
        return result;
    }
    result.accepted = true;
    result.frame = frame;
    m_snapshot.lastError.clear();
    ++m_snapshot.acceptedFrameCount;
    m_snapshot.frameInUse = true;
    m_hasLastAcceptedFrame = true;
    m_lastAcceptedFrame = frame;
    return result;
}

FrameValidationResult ObservationAdapter::consumePending(qint64 nowMonoMs) {
    FrameValidationResult result;
    if (!observeNow(nowMonoMs)) { result.errorCode = m_snapshot.lastError; return result; }
    if (expireIfDue(nowMonoMs)) { result.errorCode = m_snapshot.lastError; return result; }
    if (!m_hasPendingFrame) {
        result.errorCode = QStringLiteral("E_NO_PENDING_FRAME");
        rememberError(result.errorCode);
        return result;
    }
    FrameEnvelope frame = std::move(m_pendingFrame);
    m_pendingFrame = {};
    m_hasPendingFrame = false;
    result = validateAndAccept(frame, nowMonoMs);
    if (m_demand.mode == ObservationMode::OneShot) {
        m_snapshot.state = result.accepted ? ObservationState::Satisfied : ObservationState::Expired;
        updateResourceDrainState();
    }
    updateResourceDrainState();
    return result;
}

FrameValidationResult ObservationAdapter::submitFrame(const QString& requestId, const FrameEnvelope& frame, qint64 nowMonoMs) {
    CaptureReply reply;
    reply.status = CaptureStatus::Captured;
    reply.frame = frame;
    reply.requestId = requestId;
    reply.requestNumber = m_activeRequestNumber;
    const CaptureReply completed = completeCapture(reply, nowMonoMs);
    if (completed.status != CaptureStatus::Captured) {
        FrameValidationResult result;
        result.errorCode = completed.errorCode;
        return result;
    }
    return consumePending(nowMonoMs);
}

bool ObservationAdapter::releaseFrame(const QString& leaseId, qint64 slotGeneration) {
    if (!m_snapshot.frameInUse || !m_hasLastAcceptedFrame
        || m_lastAcceptedFrame.leaseId != leaseId || m_lastAcceptedFrame.slotGeneration != slotGeneration) {
        rememberError(QStringLiteral("E_LEASE_UNKNOWN"));
        return false;
    }
    m_snapshot.frameInUse = false;
    m_hasLastAcceptedFrame = false;
    m_lastAcceptedFrame = {};
    updateResourceDrainState();
    return true;
}

void ObservationAdapter::discardPending(const QString& reason) {
    if (!m_hasPendingFrame) return;
    m_pendingFrame = {};
    m_hasPendingFrame = false;
    rememberError(reason.left(kMaxMetadataLength));
    ++m_snapshot.rejectionCount;
    if (m_snapshot.state == ObservationState::Active && m_demand.mode == ObservationMode::OneShot) {
        m_snapshot.state = ObservationState::Expired;
        updateResourceDrainState();
    }
    updateResourceDrainState();
}

void ObservationAdapter::cancel(const QString& reason) {
    if (!m_hasDemand || m_snapshot.state != ObservationState::Active) return;
    m_snapshot.state = ObservationState::Cancelled;
    m_snapshot.cancelReason = reason.left(kMaxMetadataLength);
    rememberError(QStringLiteral("E_OBSERVATION_CANCELLED"));
    cancelCaptureForTerminalState();
}

void ObservationAdapter::close() {
    if (m_snapshot.state == ObservationState::Closed) return;
    m_closeRequested = true;
    if (m_snapshot.state == ObservationState::Active || m_snapshot.state == ObservationState::Created) {
        m_snapshot.state = ObservationState::Cancelled;
    }
    cancelCaptureForTerminalState();
    updateResourceDrainState();
}

void ObservationAdapter::rememberError(const QString& errorCode) {
    m_snapshot.lastError = errorCode.left(kMaxMetadataLength);
}

bool InMemoryReplaySource::enqueueFrame(const FrameEnvelope& frame) {
    QueuedReply queued;
    queued.reply.status = CaptureStatus::Captured;
    queued.reply.frame = frame;
    return enqueueReply(queued);
}

void InMemoryReplaySource::enqueueSyntheticFrame() {
    enqueueSyntheticFrame(SyntheticFrameSpec{});
}

bool InMemoryReplaySource::enqueueSyntheticFrame(const SyntheticFrameSpec& spec) {
    QueuedReply queued;
    queued.reply.status = CaptureStatus::Captured;
    queued.reply.frame.width = spec.width;
    queued.reply.frame.height = spec.height;
    queued.reply.frame.dpiX = spec.dpiX;
    queued.reply.frame.dpiY = spec.dpiY;
    queued.reply.frame.freshnessBasis = spec.freshnessBasis;
    queued.reply.frame.sourceMonoMs = spec.sourceMonoMs;
    queued.reply.frame.sourceUncertaintyMs = spec.sourceUncertaintyMs;
    queued.reply.frame.sourceKind = spec.sourceKind;
    queued.reply.frame.windowRef = spec.windowRef;
    queued.reply.frame.pixels = spec.pixels;
    queued.reply.frame.frameId = spec.frameId;
    queued.synthetic = true;
    return enqueueReply(queued);
}

bool InMemoryReplaySource::enqueueEmpty(const QString& errorCode) {
    QueuedReply queued;
    queued.reply.status = CaptureStatus::Empty;
    queued.reply.errorCode = errorCode;
    return enqueueReply(queued);
}

bool InMemoryReplaySource::enqueueFailure(const QString& errorCode) {
    QueuedReply queued;
    queued.reply.status = CaptureStatus::Failed;
    queued.reply.errorCode = errorCode;
    return enqueueReply(queued);
}

bool InMemoryReplaySource::enqueueReply(const QueuedReply& queued) {
    const FrameEnvelope& f = queued.reply.frame;
    const auto bounded = [](const QString& text) { return text.size() <= kMaxMetadataLength; };
    if (m_queue.size() >= kMaxQueuedReplies || f.pixels.size() > kMaxPixelBytes - m_queuedPixelBytes
        || !bounded(queued.reply.errorCode) || !bounded(f.frameId) || !bounded(f.demandId)
        || !bounded(f.captureSessionId) || !bounded(f.context.runId) || !bounded(f.context.sessionId)
        || !bounded(f.context.clockDomainId) || !bounded(f.context.stepId) || !bounded(f.windowRef)
        || !bounded(f.leaseId) || !bounded(f.sourceKind) || !bounded(f.pixelFormat)) {
        ++m_rejectedEnqueueCount;
        return false;
    }
    m_queue.enqueue(queued);
    m_queuedPixelBytes += f.pixels.size();
    return true;
}

CaptureReply InMemoryReplaySource::materialize(const QueuedReply& queued, const CaptureRequest& request) {
    CaptureReply reply = queued.reply;
    reply.requestId = request.requestId;
    reply.requestNumber = request.requestNumber;
    if (reply.status != CaptureStatus::Captured) return reply;
    FrameEnvelope& frame = reply.frame;
    if (queued.synthetic) {
        // Validate shape before width*4, any multiplication or allocation.
        if (frame.width < 1 || frame.width > kMaxDimension || frame.height < 1 || frame.height > kMaxDimension) {
            reply.status = CaptureStatus::Failed;
            reply.errorCode = QStringLiteral("E_FRAME_SHAPE");
            reply.frame = {};
            return reply;
        }
        const qint64 stride = static_cast<qint64>(frame.width) * 4;
        const qint64 bytes = stride * frame.height;
        if (bytes > kMaxPixelBytes || (!frame.pixels.isEmpty() && frame.pixels.size() != bytes)) {
            reply.status = CaptureStatus::Failed;
            reply.errorCode = QStringLiteral("E_SHM_BOUNDS");
            reply.frame = {};
            return reply;
        }
        frame.demandId = request.demand.demandId;
        frame.captureSessionId = request.demand.context.sessionId;
        frame.context = request.demand.context;
        frame.captureStartMonoMs = request.requestedMonoMs;
        frame.captureEndMonoMs = request.requestedMonoMs;
        frame.frameId = idOr(frame.frameId, QStringLiteral("frame:replay:%1").arg(m_nextFrameNumber++));
        frame.leaseId = QStringLiteral("lease:replay:%1").arg(m_nextLeaseNumber);
        frame.slotIndex = static_cast<int>((m_nextLeaseNumber - 1) % 2);
        frame.slotGeneration = m_nextLeaseNumber++;
        frame.strideBytes = static_cast<int>(stride);
        frame.validBytes = bytes;
        if (frame.pixels.isEmpty()) {
            frame.pixels.resize(static_cast<int>(bytes));
            for (qsizetype i = 0; i < frame.pixels.size(); ++i) frame.pixels[i] = static_cast<char>((i + request.requestNumber) % 251);
        }
        if (frame.freshnessBasis == FreshnessBasis::SourceTimestamp && frame.sourceMonoMs == -1) {
            frame.sourceMonoMs = request.requestedMonoMs;
        }
        if (frame.freshnessBasis != FreshnessBasis::SourceTimestamp) {
            frame.sourceMonoMs = -1;
            frame.sourceUncertaintyMs = -1;
        }
    }
    return reply;
}

CaptureReply InMemoryReplaySource::capture(const CaptureRequest& request) {
    ++m_captureCallCount;
    CaptureReply reply;
    reply.requestId = request.requestId;
    reply.requestNumber = request.requestNumber;
    ObservationAdapter validator(this);
    if (!validId(request.requestId) || request.requestNumber < 1 || request.requestNumber > 600
        || !validator.validateDemand(request.demand).valid || !safeInteger(request.requestedMonoMs)
        || request.requestedMonoMs < request.demand.notBeforeMonoMs
        || request.requestedMonoMs > request.demand.deadlineMonoMs) {
        reply.errorCode = QStringLiteral("E_CAPTURE_REQUEST_INVALID");
        return reply;
    }
    if (m_requests.size() == kMaxQueuedReplies) m_requests.removeFirst();
    m_requests.push_back(request);
    if (m_cancelRegistryFull || m_cancelled.contains(request.demand.demandId)) {
        reply.status = CaptureStatus::Cancelled;
        reply.errorCode = QStringLiteral("E_OBSERVATION_CANCELLED");
        return reply;
    }
    if (m_queue.isEmpty()) {
        reply.status = CaptureStatus::Empty;
        reply.errorCode = QStringLiteral("E_EMPTY_FRAME");
        return reply;
    }
    const QueuedReply queued = m_queue.dequeue();
    m_queuedPixelBytes -= queued.reply.frame.pixels.size();
    return materialize(queued, request);
}

void InMemoryReplaySource::cancel(const QString& demandId, const RuntimeContext& context) {
    // Keep cancellation identity bounded without evicting an old cancellation
    // and accidentally allowing an old demand to be replayed again.
    if (!validId(demandId) || !contextError(context).isEmpty()) return;
    if (!m_cancelled.contains(demandId) && m_cancelled.size() >= kMaxQueuedReplies) m_cancelRegistryFull = true;
    else m_cancelled.insert(demandId, context);
    ++m_cancelCallCount;
}

} // namespace relink::runtime::observation
