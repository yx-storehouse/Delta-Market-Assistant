#include "application/runtime/observation/observation_adapter.h"

#include <QDebug>
#include <cstdio>
#include <limits>

using namespace relink::runtime;
using namespace relink::runtime::observation;

namespace {
int failures = 0;
int assertions = 0;
void check(bool condition, const char* expression) {
    ++assertions;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", expression); ++failures; }
}
#define CHECK(expr) check((expr), #expr)

ObservationDemand demand(const QString& id = QStringLiteral("demand-1")) {
    ObservationDemand d;
    d.demandId = id;
    d.context = {QStringLiteral("run-1"), QStringLiteral("session-1"), QStringLiteral("clock-1"), QStringLiteral("step-1"), 0, 0};
    d.mode = ObservationMode::OneShot;
    d.purpose = ObservationPurpose::PageCheck;
    d.createdMonoMs = 100;
    d.notBeforeMonoMs = 100;
    d.deadlineMonoMs = 500;
    d.maxFrameAgeMs = 100;
    d.frameBudget = 1;
    d.minIntervalMs = 0;
    d.roiSpecs = {RoiSpec{QStringLiteral("page"), QStringLiteral("client_physical_px"), 0, 0, 2, 2, 1}};
    return d;
}
}

int main() {
    {
        InMemoryReplaySource source;
        source.enqueueSyntheticFrame();
        ObservationAdapter adapter(&source);
        QString error;
        CHECK(adapter.start(demand(), 100, &error));
        CHECK(error.isEmpty());
        const CaptureReply reply = adapter.acquire(100);
        CHECK(reply.status == CaptureStatus::Captured);
        CHECK(adapter.snapshot().requestCount == 1);
        const FrameValidationResult result = adapter.consumePending(101);
        CHECK(result.accepted);
        CHECK(result.errorCode.isEmpty());
        CHECK(result.lowerBoundMonoMs == 100);
        CHECK(adapter.state() == ObservationState::Satisfied);
        CHECK(adapter.snapshot().acceptedFrameCount == 1);
        CHECK(adapter.snapshot().imageFileWriteCount == 0);
        CHECK(source.imageFileWriteCount() == 0);
        CHECK(adapter.releaseFrame(result.frame.leaseId, result.frame.slotGeneration));
        CHECK(!adapter.snapshot().frameInUse);
    }

    {
        InMemoryReplaySource source;
        InMemoryReplaySource::SyntheticFrameSpec spec;
        spec.sourceMonoMs = 105;
        spec.sourceUncertaintyMs = 10;
        source.enqueueSyntheticFrame(spec);
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 100));
        CHECK(adapter.acquire(100).status == CaptureStatus::Captured);
        const FrameValidationResult result = adapter.consumePending(105);
        CHECK(!result.accepted);
        CHECK(result.errorCode == QStringLiteral("E_STALE_OR_UNPROVEN_FRAME"));
        CHECK(result.lowerBoundMonoMs == 95);
        CHECK(adapter.state() == ObservationState::Expired);
    }

    {
        InMemoryReplaySource source;
        InMemoryReplaySource::SyntheticFrameSpec spec;
        spec.freshnessBasis = FreshnessBasis::Unproven;
        source.enqueueSyntheticFrame(spec);
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 100));
        CHECK(adapter.acquire(100).status == CaptureStatus::Captured);
        const FrameValidationResult result = adapter.consumePending(100);
        CHECK(!result.accepted);
        CHECK(result.errorCode == QStringLiteral("E_STALE_OR_UNPROVEN_FRAME"));
        CHECK(adapter.snapshot().imageFileWriteCount == 0);
    }

    {
        InMemoryReplaySource source;
        FrameEnvelope frame;
        frame.frameId = QStringLiteral("frame-context-mismatch");
        frame.demandId = QStringLiteral("demand-1");
        frame.captureSessionId = QStringLiteral("session-old");
        frame.context = {QStringLiteral("run-1"), QStringLiteral("session-old"), QStringLiteral("clock-1"), QStringLiteral("step-1"), 0, 0};
        frame.sourceKind = QStringLiteral("replay");
        frame.windowRef = QStringLiteral("window:synthetic");
        frame.width = 4; frame.height = 4; frame.dpiX = 96; frame.dpiY = 96;
        frame.strideBytes = 16; frame.validBytes = 64; frame.pixels = QByteArray(64, '\0');
        frame.captureStartMonoMs = 100; frame.captureEndMonoMs = 101;
        frame.freshnessBasis = FreshnessBasis::SourceTimestamp; frame.sourceMonoMs = 100; frame.sourceUncertaintyMs = 0;
        frame.leaseId = QStringLiteral("lease:old"); frame.slotGeneration = 1;
        source.enqueueFrame(frame);
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 100));
        CHECK(adapter.acquire(100).status == CaptureStatus::Captured);
        const FrameValidationResult result = adapter.consumePending(100);
        CHECK(!result.accepted);
        CHECK(result.errorCode == QStringLiteral("E_CONTEXT_MISMATCH"));
        CHECK(adapter.state() == ObservationState::Expired);
    }

    {
        InMemoryReplaySource source;
        source.enqueueSyntheticFrame();
        source.enqueueSyntheticFrame();
        ObservationDemand d = demand(QStringLiteral("watch-1"));
        d.mode = ObservationMode::BoundedWatch;
        d.frameBudget = 2;
        d.minIntervalMs = 50;
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(d, 100));
        CHECK(adapter.watchTick(100));
        const FrameValidationResult first = adapter.consumePending(100);
        CHECK(first.accepted);
        CHECK(!adapter.watchTick(120));
        CHECK(!adapter.watchTick(140));
        CHECK(adapter.releaseFrame(first.frame.leaseId, first.frame.slotGeneration));
        CHECK(adapter.watchTick(150));
        const FrameValidationResult second = adapter.consumePending(150);
        CHECK(second.accepted);
        CHECK(adapter.snapshot().requestCount == 2);
        CHECK(!adapter.watchTick(200));
        CHECK(adapter.releaseFrame(second.frame.leaseId, second.frame.slotGeneration));
        CHECK(adapter.snapshot().imageFileWriteCount == 0);
    }

    {
        InMemoryReplaySource source;
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 100));
        QString error;
        const CaptureRequest request = adapter.beginAcquire(100, &error);
        CHECK(request.requestNumber == 1);
        CHECK(adapter.snapshot().captureInFlight);
        adapter.cancel(QStringLiteral("pause"));
        CHECK(adapter.state() == ObservationState::Cancelled);
        CHECK(source.cancelCallCount() == 1);
        FrameEnvelope late;
        late.frameId = QStringLiteral("late");
        CaptureReply lateReply;
        lateReply.status = CaptureStatus::Captured;
        lateReply.frame = late;
        lateReply.requestId = request.requestId;
        lateReply.requestNumber = request.requestNumber;
        CHECK(adapter.snapshot().resourcesDraining);
        CHECK(adapter.completeCapture(lateReply, 101).status == CaptureStatus::Cancelled);
        CHECK(adapter.snapshot().resourcesDrained);
        CHECK(adapter.snapshot().requestCount == 1);
        CHECK(adapter.snapshot().imageFileWriteCount == 0);
    }

    {
        ObservationAdapter adapter(nullptr);
        ObservationDemand invalid = demand();
        invalid.deadlineMonoMs = 99;
        QString error;
        CHECK(!adapter.start(invalid, 100, &error));
        CHECK(error == QStringLiteral("E_INVALID_DEADLINE"));
        CHECK(adapter.state() == ObservationState::Rejected);
    }

    {
        InMemoryReplaySource source;
        InMemoryReplaySource::SyntheticFrameSpec spec;
        spec.width = 4; spec.height = 4;
        source.enqueueSyntheticFrame(spec);
        ObservationDemand invalidRoi = demand();
        invalidRoi.roiSpecs[0].x = 3;
        invalidRoi.roiSpecs[0].width = 2;
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(invalidRoi, 100));
        CHECK(adapter.acquire(100).status == CaptureStatus::Captured);
        CHECK(adapter.consumePending(100).errorCode == QStringLiteral("E_ROI_OUT_OF_BOUNDS"));
    }


    // Deadlines are independent of budget, pending/reader state and in-flight
    // captures. Cancellation requests do not pretend resources are drained.
    {
        InMemoryReplaySource source;
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 100));
        CHECK(!adapter.watchTick(501));
        CHECK(adapter.state() == ObservationState::Expired);
        CHECK(adapter.snapshot().requestCount == 0);
        CHECK(source.cancelCallCount() == 1);
        CHECK(adapter.snapshot().resourcesDrained);
    }
    {
        InMemoryReplaySource source;
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 100));
        CaptureRequest r = adapter.beginAcquire(100);
        CHECK(r.requestNumber == 1);
        CHECK(adapter.deadlineTick(500));
        CHECK(adapter.state() == ObservationState::Expired);
        CHECK(source.cancelCallCount() == 1);
        CHECK(adapter.snapshot().captureInFlight);
        CHECK(adapter.snapshot().resourcesDraining);
        CaptureReply late = source.capture(r);
        CHECK(adapter.completeCapture(late, 500).status == CaptureStatus::Cancelled);
        CHECK(adapter.snapshot().resourcesDrained);
        CHECK(!adapter.watchTick(501));
        CHECK(source.cancelCallCount() == 1);
    }
    {
        InMemoryReplaySource source;
        source.enqueueSyntheticFrame();
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 100));
        CHECK(adapter.acquire(100).status == CaptureStatus::Captured);
        CHECK(adapter.snapshot().pendingFrame);
        CHECK(adapter.consumePending(501).errorCode == QStringLiteral("E_OBSERVATION_DEADLINE"));
        CHECK(!adapter.snapshot().pendingFrame);
        CHECK(adapter.snapshot().resourcesDrained);
        CHECK(adapter.lastAcceptedFrame().pixels.isEmpty());
        CHECK(source.cancelCallCount() == 1);
        CHECK(!adapter.consumePending(501).accepted);
    }
    {
        InMemoryReplaySource source;
        source.enqueueSyntheticFrame();
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 100));
        CHECK(adapter.acquire(100).status == CaptureStatus::Captured);
        adapter.cancel();
        CHECK(!adapter.snapshot().pendingFrame);
        CHECK(adapter.snapshot().resourcesDrained);
        CHECK(!adapter.consumePending(100).accepted);
        CHECK(adapter.state() == ObservationState::Cancelled);
    }
    {
        InMemoryReplaySource source;
        source.enqueueSyntheticFrame();
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 100));
        CHECK(adapter.acquire(100).status == CaptureStatus::Captured);
        const auto accepted = adapter.consumePending(100);
        CHECK(accepted.accepted);
        adapter.close();
        CHECK(adapter.state() != ObservationState::Closed);
        CHECK(adapter.snapshot().resourcesDraining);
        CHECK(!adapter.snapshot().resourcesDrained);
        CHECK(!adapter.releaseFrame(QStringLiteral("not-current"), accepted.frame.slotGeneration));
        CHECK(adapter.snapshot().frameInUse);
        CHECK(adapter.releaseFrame(accepted.frame.leaseId, accepted.frame.slotGeneration));
        CHECK(adapter.state() == ObservationState::Closed);
        CHECK(adapter.snapshot().resourcesDrained);
        CHECK(adapter.lastAcceptedFrame().pixels.isEmpty());
    }
    {
        InMemoryReplaySource source;
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 100));
        const auto r = adapter.beginAcquire(100);
        adapter.close();
        CHECK(adapter.state() == ObservationState::Cancelled);
        CHECK(adapter.snapshot().resourcesDraining);
        CHECK(adapter.completeCapture(source.capture(r), 100).status == CaptureStatus::Cancelled);
        CHECK(adapter.state() == ObservationState::Closed);
        CHECK(adapter.snapshot().resourcesDrained);
    }

    // The final bounded-watch request remains usable until deadline; budget
    // exhaustion cannot discard work that was already issued.
    {
        InMemoryReplaySource source;
        source.enqueueSyntheticFrame();
        ObservationDemand d = demand();
        d.mode = ObservationMode::BoundedWatch;
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(d, 100));
        const auto r = adapter.beginAcquire(100);
        CHECK(!adapter.watchTick(101));
        CHECK(adapter.state() == ObservationState::Active);
        CHECK(adapter.snapshot().captureInFlight);
        CHECK(adapter.completeCapture(source.capture(r), 101).status == CaptureStatus::Captured);
        const auto accepted = adapter.consumePending(101);
        CHECK(accepted.accepted);
        CHECK(adapter.releaseFrame(accepted.frame.leaseId, accepted.frame.slotGeneration));
        CHECK(!adapter.watchTick(102));
        CHECK(adapter.state() == ObservationState::Active);
        CHECK(adapter.deadlineTick(500));
        CHECK(adapter.snapshot().requestCount == 1);
    }
    // A duplicated old completion must not steal a later request's token.
    {
        InMemoryReplaySource source;
        source.enqueueEmpty();
        source.enqueueSyntheticFrame();
        auto d = demand(); d.mode = ObservationMode::BoundedWatch; d.frameBudget = 2;
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(d, 100));
        const auto first = adapter.beginAcquire(100);
        const auto oldReply = source.capture(first);
        CHECK(adapter.completeCapture(oldReply, 100).status == CaptureStatus::Empty);
        const auto second = adapter.beginAcquire(101);
        CHECK(second.requestNumber == 2);
        CHECK(second.requestId != first.requestId);
        CHECK(adapter.completeCapture(oldReply, 101).errorCode == QStringLiteral("E_CAPTURE_REQUEST_MISMATCH"));
        CHECK(adapter.snapshot().captureInFlight);
        CaptureReply missingId; missingId.requestNumber = 2;
        CHECK(adapter.completeCapture(missingId, 101).errorCode == QStringLiteral("E_CAPTURE_REQUEST_MISMATCH"));
        CHECK(adapter.snapshot().captureInFlight);
        auto correct = source.capture(second);
        CHECK(adapter.completeCapture(correct, 101).status == CaptureStatus::Captured);
        CHECK(adapter.consumePending(101).accepted);
        CHECK(adapter.snapshot().acceptedFrameCount == 1);
    }

    // All runtime clock inputs are monotonic safe integers, including input
    // to callback and consumption paths; failed inputs do not consume resources.
    for (const qint64 badNow : {qint64(-1), qint64(9007199254740992LL), std::numeric_limits<qint64>::max()}) {
        InMemoryReplaySource source;
        ObservationAdapter adapter(&source);
        QString error;
        CHECK(!adapter.start(demand(), badNow, &error));
        CHECK(error == QStringLiteral("E_CLOCK_RANGE"));
        CHECK(source.captureCallCount() == 0);
    }
    {
        InMemoryReplaySource source;
        ObservationAdapter validator(&source);
        auto d = demand(); d.deadlineMonoMs = 100;
        CHECK(!validator.validateDemand(d).valid);
        d = demand(); d.deadlineMonoMs = 9007199254740992LL;
        CHECK(!validator.validateDemand(d).valid);
        d = demand(); d.context.cancelEpoch = 9007199254740992LL;
        CHECK(!validator.validateDemand(d).valid);
        d = demand(); d.roiSpecs[0].transformVersion = 9007199254740992LL;
        CHECK(!validator.validateDemand(d).valid);
        d = demand(); d.mode = static_cast<ObservationMode>(9);
        CHECK(!validator.validateDemand(d).valid);
    }
    {
        InMemoryReplaySource source;
        source.enqueueSyntheticFrame();
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 100));
        QString error;
        CHECK(adapter.beginAcquire(99, &error).requestNumber == 0);
        CHECK(error == QStringLiteral("E_CLOCK_BACKWARD"));
        CHECK(adapter.snapshot().requestCount == 0);
        const auto r = adapter.beginAcquire(101);
        const auto reply = source.capture(r);
        CHECK(adapter.completeCapture(reply, 100).errorCode == QStringLiteral("E_CLOCK_BACKWARD"));
        CHECK(adapter.snapshot().captureInFlight);
        CHECK(adapter.completeCapture(reply, 101).status == CaptureStatus::Captured);
        CHECK(adapter.consumePending(100).errorCode == QStringLiteral("E_CLOCK_BACKWARD"));
        CHECK(adapter.snapshot().pendingFrame);
        CHECK(adapter.consumePending(9007199254740992LL).errorCode == QStringLiteral("E_CLOCK_RANGE"));
        CHECK(adapter.snapshot().pendingFrame);
        CHECK(adapter.consumePending(101).accepted);
    }
    {
        InMemoryReplaySource source;
        ObservationAdapter adapter(&source);
        QString error;
        CHECK(!adapter.start(demand(), 501, &error));
        CHECK(error == QStringLiteral("E_OBSERVATION_DEADLINE"));
        CHECK(adapter.state() == ObservationState::Expired);
        CHECK(source.cancelCallCount() == 1);
        CHECK(source.captureCallCount() == 0);
    }
    // At exactly deadline, result-first is accepted; deadline-first is not.
    {
        InMemoryReplaySource source;
        source.enqueueSyntheticFrame();
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 500));
        CHECK(adapter.acquire(500).status == CaptureStatus::Captured);
        CHECK(adapter.consumePending(500).accepted);
        CHECK(!adapter.deadlineTick(500));
    }
    for (int variant = 0; variant < 4; ++variant) {
        InMemoryReplaySource source;
        source.enqueueSyntheticFrame();
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 100));
        const auto r = adapter.beginAcquire(100);
        auto reply = source.capture(r);
        if (variant == 0) { reply.frame.sourceMonoMs = 110; reply.frame.sourceUncertaintyMs = 10; }
        if (variant == 1) reply.frame.captureEndMonoMs = 110;
        if (variant == 2) reply.frame.sourceKind = QStringLiteral("wgc");
        if (variant == 3) reply.frame.captureStartMonoMs = 9007199254740992LL;
        CHECK(adapter.completeCapture(reply, 100).status == CaptureStatus::Captured);
        const auto result = adapter.consumePending(100);
        CHECK(!result.accepted);
        CHECK(result.frame.pixels.isEmpty());
        if (variant < 2) CHECK(result.errorCode == QStringLiteral("E_STALE_OR_UNPROVEN_FRAME"));
        if (variant == 2) CHECK(result.errorCode == QStringLiteral("E_SOURCE_KIND"));
        if (variant == 3) CHECK(result.errorCode == QStringLiteral("E_CAPTURE_TIME"));
        CHECK(adapter.snapshot().imageFileWriteCount == 0);
    }
    for (int dimension : {-1, 0, 8193, std::numeric_limits<int>::max()}) {
        InMemoryReplaySource source;
        InMemoryReplaySource::SyntheticFrameSpec spec; spec.width = dimension;
        CHECK(source.enqueueSyntheticFrame(spec));
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 100));
        const auto reply = adapter.acquire(100);
        CHECK(reply.status == CaptureStatus::Failed);
        CHECK(reply.errorCode == QStringLiteral("E_FRAME_SHAPE"));
        CHECK(reply.frame.pixels.isEmpty());
        CHECK(source.queuedPixelBytes() == 0);
    }
    {
        InMemoryReplaySource source;
        InMemoryReplaySource::SyntheticFrameSpec spec; spec.width = 8192; spec.height = 8192;
        CHECK(source.enqueueSyntheticFrame(spec));
        ObservationAdapter adapter(&source);
        CHECK(adapter.start(demand(), 100));
        CHECK(adapter.acquire(100).errorCode == QStringLiteral("E_SHM_BOUNDS"));
        CHECK(source.queuedPixelBytes() == 0);
    }
    {
        InMemoryReplaySource source;
        for (int i = 0; i < 600; ++i) CHECK(source.enqueueEmpty());
        CHECK(!source.enqueueEmpty());
        CHECK(source.queuedReplyCount() == 600);
        CHECK(source.rejectedEnqueueCount() == 1);
        CHECK(source.queuedPixelBytes() == 0);
    }
    {
        InMemoryReplaySource source;
        CHECK(!source.enqueueFailure(QString(201, 'x')));
        InMemoryReplaySource::SyntheticFrameSpec spec; spec.windowRef = QString(201, 'x');
        CHECK(!source.enqueueSyntheticFrame(spec));
        CHECK(source.queuedReplyCount() == 0);
        auto d = demand();
        for (int i = 0; i < 605; ++i) {
            CaptureRequest request;
            request.demand = d; request.requestId = QStringLiteral("request:%1").arg(i);
            request.requestNumber = 1; request.requestedMonoMs = 100;
            source.capture(request);
        }
        CHECK(source.captureCallCount() == 605);
        CHECK(source.requests().size() == 600);
        for (int i = 0; i < 605; ++i) source.cancel(QStringLiteral("cancel:%1").arg(i), d.context);
        CHECK(source.retainedCancellationCount() == 600);
        CaptureRequest request;
        request.demand = d; request.requestId = QStringLiteral("after-cancel-bound");
        request.requestNumber = 1; request.requestedMonoMs = 100;
        CHECK(source.capture(request).status == CaptureStatus::Cancelled);
        CHECK(source.imageFileWriteCount() == 0);
    }

    std::printf("OBSERVATION_ADAPTER_TESTS=%s; assertions=%d; failures=%d; image_file_writes=0\n",
                failures == 0 ? "PASS" : "FAIL", assertions, failures);
    return failures == 0 ? 0 : 1;
}
