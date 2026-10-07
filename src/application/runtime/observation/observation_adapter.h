#pragma once

#include "application/runtime/runtime.h"

#include <QByteArray>
#include <QHash>
#include <QQueue>
#include <QString>
#include <QVector>

#include <limits>

namespace relink::runtime::observation {

enum class ObservationMode {
    OneShot,
    BoundedWatch,
};

enum class ObservationPurpose {
    PageCheck,
    ListingFields,
    Receipt,
    Revalidate,
    Countdown,
};

enum class FreshnessBasis {
    SourceTimestamp,
    PostBarrierCapture,
    Unproven,
};

enum class ObservationState {
    Created,
    Active,
    Satisfied,
    Expired,
    Cancelled,
    Rejected,
    Closed,
};

QString toString(ObservationMode value);
QString toString(ObservationPurpose value);
QString toString(FreshnessBasis value);
QString toString(ObservationState value);

struct RoiSpec {
    QString roiId;
    QString coordinateSpace = QStringLiteral("client_physical_px");
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;
    qint64 transformVersion = 1;
};

struct ObservationDemand {
    QString demandId;
    RuntimeContext context;
    ObservationMode mode = ObservationMode::OneShot;
    ObservationPurpose purpose = ObservationPurpose::PageCheck;
    qint64 createdMonoMs = 0;
    qint64 notBeforeMonoMs = 0;
    qint64 deadlineMonoMs = 0;
    qint64 maxFrameAgeMs = 1000;
    int frameBudget = 1;
    qint64 minIntervalMs = 0;
    QVector<RoiSpec> roiSpecs;
    QString persistence = QStringLiteral("none");
};

struct ObservationDemandValidation {
    bool valid = false;
    QString errorCode;
};

struct CaptureRequest {
    ObservationDemand demand;
    QString requestId;
    int requestNumber = 0;
    qint64 requestedMonoMs = 0;
};

struct FrameEnvelope {
    QString frameId;
    QString demandId;
    QString captureSessionId;
    RuntimeContext context;
    QString sourceKind = QStringLiteral("replay");
    QString windowRef = QStringLiteral("window:synthetic");
    int width = 0;
    int height = 0;
    int dpiX = 96;
    int dpiY = 96;
    QString pixelFormat = QStringLiteral("BGRA8");
    int strideBytes = 0;
    qint64 validBytes = 0;
    qint64 captureStartMonoMs = 0;
    qint64 captureEndMonoMs = 0;
    FreshnessBasis freshnessBasis = FreshnessBasis::Unproven;
    qint64 sourceMonoMs = -1;
    qint64 sourceUncertaintyMs = -1;
    QString leaseId;
    int slotIndex = 0;
    qint64 slotGeneration = 0;
    QByteArray pixels;
};

struct FrameValidationResult {
    bool accepted = false;
    QString errorCode;
    qint64 lowerBoundMonoMs = -1;
    FrameEnvelope frame;
};

enum class CaptureStatus {
    Captured,
    Empty,
    Failed,
    Cancelled,
};

struct CaptureReply {
    CaptureStatus status = CaptureStatus::Failed;
    QString errorCode;
    FrameEnvelope frame;
    // The coordinator-issued request number is part of the async completion
    // identity. A late response for an older request must never complete a
    // newer acquire.
    int requestNumber = 0;
    QString requestId;
};

struct ObservationSnapshot {
    ObservationState state = ObservationState::Created;
    int requestCount = 0;
    int acceptedFrameCount = 0;
    int rejectionCount = 0;
    int imageFileWriteCount = 0;
    bool captureInFlight = false;
    bool pendingFrame = false;
    bool frameInUse = false;
    bool resourcesDraining = false;
    bool resourcesDrained = true;
    QString lastError;
    QString cancelReason;
};

// Boundary for future Windows Graphics Capture/DXGI implementations. The
// implementation must return pixels and metadata in memory; the adapter never
// supplies a file path and never falls back to an image file transport.
class IObservationSource {
public:
    virtual ~IObservationSource() = default;
    virtual CaptureReply capture(const CaptureRequest& request) = 0;
    virtual void cancel(const QString& demandId, const RuntimeContext& context) = 0;
    virtual QString sourceKind() const = 0;
};

// OCR/vision is intentionally a second boundary. It receives an accepted
// in-memory frame and cannot ask the capture source to retry implicitly.
class IObservationRecognizer {
public:
    virtual ~IObservationRecognizer() = default;
    virtual QString recognize(const FrameEnvelope& frame, QString* errorCode = nullptr) = 0;
};

class ObservationAdapter final {
public:
    explicit ObservationAdapter(IObservationSource* source);

    ObservationDemandValidation validateDemand(const ObservationDemand& demand) const;
    bool start(const ObservationDemand& demand, qint64 nowMonoMs, QString* errorCode = nullptr);

    // beginAcquire/completeCapture support an asynchronous Windows backend.
    // acquire is the synchronous convenience path used by the replay source.
    bool canAcquire(qint64 nowMonoMs) const;
    CaptureRequest beginAcquire(qint64 nowMonoMs, QString* errorCode = nullptr);
    CaptureReply completeCapture(const CaptureReply& reply, qint64 nowMonoMs);
    CaptureReply acquire(qint64 nowMonoMs);
    bool watchTick(qint64 nowMonoMs);
    // An explicitly arbitrated deadline event expires at equality. Results
    // arbitrated first at that same millisecond may still be consumed.
    bool deadlineTick(qint64 nowMonoMs);

    FrameValidationResult consumePending(qint64 nowMonoMs);
    FrameValidationResult submitFrame(const QString& requestId, const FrameEnvelope& frame, qint64 nowMonoMs);
    bool releaseFrame(const QString& leaseId, qint64 slotGeneration);
    void discardPending(const QString& reason = QStringLiteral("discarded"));

    void cancel(const QString& reason = QStringLiteral("cancelled"));
    void close();

    const ObservationDemand& demand() const { return m_demand; }
    const ObservationSnapshot& snapshot() const { return m_snapshot; }
    ObservationState state() const { return m_snapshot.state; }
    const FrameEnvelope& lastAcceptedFrame() const { return m_lastAcceptedFrame; }

private:
    bool observeNow(qint64 nowMonoMs);
    bool expireIfDue(qint64 nowMonoMs);
    void cancelCaptureForTerminalState();
    void updateResourceDrainState();
    bool contextMatches(const RuntimeContext& lhs, const RuntimeContext& rhs) const;
    QString validateFrameShape(const FrameEnvelope& frame) const;
    FrameValidationResult validateAndAccept(const FrameEnvelope& frame, qint64 nowMonoMs);
    void rememberError(const QString& errorCode);

    IObservationSource* m_source = nullptr;
    ObservationDemand m_demand;
    ObservationSnapshot m_snapshot;
    FrameEnvelope m_pendingFrame;
    FrameEnvelope m_lastAcceptedFrame;
    bool m_hasDemand = false;
    bool m_hasPendingFrame = false;
    bool m_hasLastAcceptedFrame = false;
    bool m_captureInFlight = false;
    bool m_captureCancellationSent = false;
    bool m_closeRequested = false;
    int m_requestNumber = 0;
    int m_activeRequestNumber = 0;
    QString m_activeRequestId;
    QString m_expectedSourceKind;
    qint64 m_lastObservedNowMonoMs = -1;
    qint64 m_lastAcquireMonoMs = std::numeric_limits<qint64>::min();
};

// Deterministic replay source. Frames and failures are queued in memory and
// consumed exactly once per Acquire; no implicit retry and no filesystem I/O.
class InMemoryReplaySource final : public IObservationSource {
public:
    struct SyntheticFrameSpec {
        int width = 4;
        int height = 4;
        int dpiX = 96;
        int dpiY = 96;
        FreshnessBasis freshnessBasis = FreshnessBasis::SourceTimestamp;
        qint64 sourceMonoMs = -1;
        qint64 sourceUncertaintyMs = 0;
        QString sourceKind = QStringLiteral("replay");
        QString windowRef = QStringLiteral("window:synthetic");
        QByteArray pixels;
        QString frameId;
    };

    bool enqueueFrame(const FrameEnvelope& frame);
    void enqueueSyntheticFrame();
    bool enqueueSyntheticFrame(const SyntheticFrameSpec& spec);
    bool enqueueEmpty(const QString& errorCode = QStringLiteral("E_EMPTY_FRAME"));
    bool enqueueFailure(const QString& errorCode);

    CaptureReply capture(const CaptureRequest& request) override;
    void cancel(const QString& demandId, const RuntimeContext& context) override;
    QString sourceKind() const override { return QStringLiteral("replay"); }

    int captureCallCount() const { return m_captureCallCount; }
    int cancelCallCount() const { return m_cancelCallCount; }
    int imageFileWriteCount() const { return 0; }
    int queuedReplyCount() const { return m_queue.size(); }
    qint64 queuedPixelBytes() const { return m_queuedPixelBytes; }
    int rejectedEnqueueCount() const { return m_rejectedEnqueueCount; }
    int retainedCancellationCount() const { return m_cancelled.size(); }
    const QVector<CaptureRequest>& requests() const { return m_requests; }

private:
    static constexpr int kMaxQueuedReplies = 600;
    struct QueuedReply {
        CaptureReply reply;
        bool synthetic = false;
    };
    CaptureReply materialize(const QueuedReply& queued, const CaptureRequest& request);
    bool enqueueReply(const QueuedReply& queued);

    QQueue<QueuedReply> m_queue;
    QVector<CaptureRequest> m_requests;
    QHash<QString, RuntimeContext> m_cancelled;
    bool m_cancelRegistryFull = false;
    qint64 m_queuedPixelBytes = 0;
    int m_rejectedEnqueueCount = 0;
    int m_captureCallCount = 0;
    int m_cancelCallCount = 0;
    qint64 m_nextFrameNumber = 1;
    qint64 m_nextLeaseNumber = 1;
};

} // namespace relink::runtime::observation
