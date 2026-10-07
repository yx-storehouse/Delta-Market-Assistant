#include "application/runtime/observation/observation_adapter.h"
#include "application/vision/lease_pool.h"
#include "application/vision/worker_protocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <array>
#include <iostream>
#include <limits>

using namespace relink::runtime;
using namespace relink::runtime::observation;
using namespace relink::vision;

namespace {
int assertions = 0;
int failures = 0;
void check(bool condition, const char* label)
{
    ++assertions;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << label << '\n';
    }
}
#define CHECK(expression) check((expression), #expression)

QJsonObject contextJson(const RuntimeContext& context)
{
    return {{"run_id", context.runId}, {"session_id", context.sessionId},
            {"clock_domain_id", context.clockDomainId}, {"step_id", context.stepId},
            {"cancel_epoch", context.cancelEpoch}, {"viewport_generation", context.viewportGeneration}};
}

ObservationDemand makeDemand(ObservationMode mode = ObservationMode::OneShot)
{
    ObservationDemand demand;
    demand.demandId = QStringLiteral("pipeline-demand");
    demand.context = {QStringLiteral("pipeline-run"), QStringLiteral("session-1"),
                      QStringLiteral("pipeline-clock"), QStringLiteral("price-step"), 3, 7};
    demand.mode = mode;
    demand.purpose = ObservationPurpose::ListingFields;
    demand.createdMonoMs = 100;
    demand.notBeforeMonoMs = 100;
    demand.deadlineMonoMs = 500;
    demand.maxFrameAgeMs = 100;
    demand.frameBudget = mode == ObservationMode::OneShot ? 1 : 2;
    demand.minIntervalMs = mode == ObservationMode::OneShot ? 0 : 50;
    demand.roiSpecs = {{QStringLiteral("price"), QStringLiteral("client_physical_px"), 0, 0, 100, 50, 7}};
    return demand;
}

InMemoryReplaySource::SyntheticFrameSpec syntheticFrame()
{
    InMemoryReplaySource::SyntheticFrameSpec frame;
    frame.width = 100;
    frame.height = 50;
    return frame;
}

// This harness connects the production metadata/state machines. Its two byte
// arrays emulate shared-memory contents; descriptor names are protocol text
// only. No process, OS mapping, recognizer model or image-file API is invoked.
class Pipeline final {
public:
    explicit Pipeline(const QDir& fixtures)
        : adapter(&source), directory(fixtures), pool(20000),
          coordinator(QStringLiteral("session-1"), WorkerRole::Coordinator),
          worker(QStringLiteral("session-1"), WorkerRole::Worker)
    {
        CHECK(transfer(coordinator, worker, fixture("01_hello.valid.json")));
        CHECK(transfer(worker, coordinator, fixture("02_ready.valid.json")));
        CHECK(coordinator.state() == SessionState::Ready && worker.state() == SessionState::Ready);
    }

    WorkerEnvelope fixture(const char* name)
    {
        QFile file(directory.filePath(QString::fromLatin1(name)));
        CHECK(file.open(QIODevice::ReadOnly));
        const auto object = QJsonDocument::fromJson(file.readAll()).object();
        CHECK(!object.isEmpty());
        WorkerEnvelope envelope;
        envelope.protocolVersion = object.value("protocol_version").toInt();
        envelope.sessionId = object.value("session_id").toString();
        envelope.kind = object.value("kind").toString();
        envelope.body = object.value("body").toObject();
        envelope.messageId = QStringLiteral("pipeline-message-%1").arg(++messageNumber);
        return envelope;
    }

    bool transfer(WorkerProtocol& sender, WorkerProtocol& receiver, const WorkerEnvelope& message)
    {
        ProtocolError error;
        QByteArray wire;
        if (!sender.send(message, &wire, &error)) {
            std::cerr << "send " << message.kind.toStdString() << ": " << error.code.toStdString()
                      << ' ' << error.message.toStdString() << '\n';
            return false;
        }
        NdjsonFramer framer;
        const auto head = framer.feed(wire.left(7));
        const auto middle = framer.feed(wire.mid(7, wire.size() - 8));
        const auto tail = framer.feed(wire.right(1));
        if (!head.ok() || !middle.ok() || !tail.ok() || !head.messages.isEmpty()
            || !middle.messages.isEmpty() || tail.messages.size() != 1 || !framer.finish().ok()) return false;
        const auto& decoded = tail.messages.first();
        if (decoded.body != message.body || decoded.messageId != message.messageId) return false;
        if (!receiver.receive(decoded, &error)) {
            std::cerr << "receive " << message.kind.toStdString() << ": " << error.code.toStdString()
                      << ' ' << error.message.toStdString() << '\n';
            return false;
        }
        return true;
    }

    FrameValidationResult capture(qint64 now)
    {
        CHECK(adapter.acquire(now).status == CaptureStatus::Captured);
        return adapter.consumePending(now);
    }

    bool dispatch(const FrameValidationResult& accepted, const QString& request)
    {
        // A rejected capture cannot allocate a lease or enter the protocol.
        if (!accepted.accepted) return false;
        const auto allocation = pool.beginWrite(accepted.frame.validBytes);
        if (!allocation.ok) return false;
        lease = allocation.lease;
        sourceFrame = accepted.frame;
        requestId = request;
        slotBytes[lease.slotIndex] = sourceFrame.pixels;
        CHECK(slotBytes[lease.slotIndex].size() == lease.byteLength);
        CHECK(slotBytes[lease.slotIndex] == adapter.lastAcceptedFrame().pixels);
        if (!pool.publish(lease) || !pool.acquireForWorker(lease)) return false;

        auto recognize = fixture("03_recognize.valid.json");
        auto frame = recognize.body.value("frame").toObject();
        frame.insert("frame_id", sourceFrame.frameId);
        frame.insert("demand_id", sourceFrame.demandId);
        frame.insert("capture_session_id", sourceFrame.captureSessionId);
        frame.insert("context", contextJson(sourceFrame.context));
        frame.insert("source_kind", sourceFrame.sourceKind);
        frame.insert("window_ref", sourceFrame.windowRef);
        frame.insert("width", sourceFrame.width);
        frame.insert("height", sourceFrame.height);
        frame.insert("dpi_x", sourceFrame.dpiX);
        frame.insert("dpi_y", sourceFrame.dpiY);
        frame.insert("pixel_format", sourceFrame.pixelFormat);
        frame.insert("stride_bytes", sourceFrame.strideBytes);
        frame.insert("valid_bytes", sourceFrame.validBytes);
        frame.insert("capture_start_mono_ms", sourceFrame.captureStartMonoMs);
        frame.insert("capture_end_mono_ms", sourceFrame.captureEndMonoMs);
        frame.insert("freshness_basis", relink::runtime::observation::toString(sourceFrame.freshnessBasis));
        frame.insert("source_mono_ms", sourceFrame.sourceMonoMs < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(sourceFrame.sourceMonoMs));
        frame.insert("source_uncertainty_ms", sourceFrame.sourceUncertaintyMs < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(sourceFrame.sourceUncertaintyMs));
        frame.insert("descriptor", QJsonObject{{"transport", "win32_named_shared_memory"},
            {"mapping_name", QStringLiteral("Local\\RelinkVision_session-1_%1").arg(lease.slotIndex)},
            {"lease_id", lease.leaseId}, {"slot_index", lease.slotIndex}, {"slot_generation", lease.generation},
            {"offset_bytes", 0}, {"byte_length", lease.byteLength}, {"capacity_bytes", pool.slotCapacityBytes()}});
        QJsonArray rois;
        for (const auto& roi : adapter.demand().roiSpecs)
            rois.append(QJsonObject{{"roi_id", roi.roiId}, {"coordinate_space", roi.coordinateSpace},
                {"x", roi.x}, {"y", roi.y}, {"width", roi.width}, {"height", roi.height},
                {"transform_version", roi.transformVersion}});
        recognize.body.insert("request_id", requestId);
        recognize.body.insert("demand_id", sourceFrame.demandId);
        recognize.body.insert("context", contextJson(sourceFrame.context));
        recognize.body.insert("frame", frame);
        recognize.body.insert("roi_specs", rois);
        recognize.body.insert("relative_budget_ms", adapter.demand().deadlineMonoMs - sourceFrame.captureEndMonoMs);
        return transfer(coordinator, worker, recognize);
    }

    WorkerEnvelope result()
    {
        auto message = fixture("04_result.valid.json");
        message.body.insert("request_id", requestId);
        message.body.insert("demand_id", sourceFrame.demandId);
        message.body.insert("frame_id", sourceFrame.frameId);
        message.body.insert("lease_id", lease.leaseId);
        message.body.insert("context", contextJson(sourceFrame.context));
        return message;
    }

    WorkerEnvelope releaseMessage()
    {
        auto message = fixture("05_release_frame.valid.json");
        message.body.insert("request_id", requestId);
        message.body.insert("lease_id", lease.leaseId);
        message.body.insert("slot_index", lease.slotIndex);
        message.body.insert("slot_generation", lease.generation);
        return message;
    }

    void release()
    {
        const auto message = releaseMessage();
        CHECK(transfer(worker, coordinator, message));
        CHECK(coordinator.requestReleased(requestId) && worker.requestReleased(requestId));
        CHECK(pool.release(lease));
        slotBytes[lease.slotIndex].clear();
        CHECK(adapter.releaseFrame(sourceFrame.leaseId, sourceFrame.slotGeneration));
        CHECK(coordinator.inFlightCount() == 0 && worker.inFlightCount() == 0);
        CHECK(pool.freeCount() == 2);
        CHECK(!adapter.snapshot().frameInUse && adapter.snapshot().resourcesDrained);
        CHECK(adapter.lastAcceptedFrame().pixels.isEmpty());
        CHECK(slotBytes[0].isEmpty() && slotBytes[1].isEmpty());
    }

    void shutdown()
    {
        adapter.close();
        CHECK(adapter.state() == ObservationState::Closed && adapter.snapshot().resourcesDrained);
        CHECK(transfer(coordinator, worker, fixture("09_shutdown.valid.json")));
        CHECK(transfer(worker, coordinator, fixture("10_bye.valid.json")));
        CHECK(coordinator.state() == SessionState::Closed && worker.state() == SessionState::Closed);
        CHECK(source.imageFileWriteCount() == 0 && adapter.snapshot().imageFileWriteCount == 0);
    }

    InMemoryReplaySource source;
    ObservationAdapter adapter;
    QDir directory;
    LeasePool pool;
    WorkerProtocol coordinator;
    WorkerProtocol worker;
    std::array<QByteArray, 2> slotBytes;
    LeaseHandle lease;
    FrameEnvelope sourceFrame;
    QString requestId;
    int messageNumber = 0;
};

void successAndCorrelation(const QDir& directory)
{
    Pipeline pipeline(directory);
    CHECK(pipeline.source.enqueueSyntheticFrame(syntheticFrame()));
    CHECK(pipeline.adapter.start(makeDemand(), 100));
    const auto frame = pipeline.capture(100);
    CHECK(frame.accepted && frame.lowerBoundMonoMs == 100);
    CHECK(pipeline.dispatch(frame, QStringLiteral("recognize-first")));
    CHECK(pipeline.coordinator.inFlightCount() == 1 && pipeline.worker.inFlightCount() == 1);
    CHECK(pipeline.adapter.snapshot().resourcesDraining && pipeline.pool.freeCount() == 1);
    CHECK(pipeline.pool.current(pipeline.lease.slotIndex).state == LeaseState::InUse);
    ProtocolError error;
    auto wrong = pipeline.result();
    auto context = wrong.body.value("context").toObject();
    context.insert("viewport_generation", frame.frame.context.viewportGeneration + 1);
    wrong.body.insert("context", context);
    CHECK(!pipeline.coordinator.receive(wrong, &error) && error.code == QStringLiteral("E_CONTEXT_MISMATCH"));
    CHECK(!pipeline.coordinator.requestTerminal(pipeline.requestId));
    CHECK(pipeline.adapter.snapshot().frameInUse && pipeline.pool.freeCount() == 1);
    wrong = pipeline.result();
    wrong.body.insert("frame_id", QStringLiteral("stale-frame"));
    CHECK(!pipeline.coordinator.receive(wrong, &error) && error.code == QStringLiteral("E_CONTEXT_MISMATCH"));
    CHECK(!pipeline.coordinator.receive(pipeline.releaseMessage(), &error));
    CHECK(!pipeline.coordinator.requestReleased(pipeline.requestId));
    CHECK(pipeline.transfer(pipeline.worker, pipeline.coordinator, pipeline.result()));
    CHECK(pipeline.coordinator.requestTerminal(pipeline.requestId) && pipeline.worker.requestTerminal(pipeline.requestId));
    auto wrongRelease = pipeline.releaseMessage();
    wrongRelease.body.insert("slot_generation", pipeline.lease.generation + 1);
    CHECK(!pipeline.coordinator.receive(wrongRelease, &error) && error.code == QStringLiteral("E_LEASE_UNKNOWN"));
    auto wrongLease = pipeline.lease;
    ++wrongLease.generation;
    CHECK(!pipeline.pool.release(wrongLease));
    CHECK(!pipeline.adapter.releaseFrame(frame.frame.leaseId, frame.frame.slotGeneration + 1));
    CHECK(pipeline.pool.freeCount() == 1 && pipeline.adapter.snapshot().frameInUse);
    pipeline.release();
    pipeline.shutdown();
}

void cancellation(const QDir& directory)
{
    Pipeline pipeline(directory);
    CHECK(pipeline.source.enqueueSyntheticFrame(syntheticFrame()));
    CHECK(pipeline.adapter.start(makeDemand(ObservationMode::BoundedWatch), 100));
    const auto frame = pipeline.capture(100);
    CHECK(pipeline.dispatch(frame, QStringLiteral("recognize-cancelled")));
    pipeline.adapter.cancel(QStringLiteral("pause"));
    CHECK(pipeline.pool.cancel(pipeline.lease));
    auto cancel = pipeline.fixture("06_cancel.valid.json");
    cancel.body.insert("target_id", pipeline.requestId);
    CHECK(pipeline.transfer(pipeline.coordinator, pipeline.worker, cancel));
    auto acknowledged = pipeline.fixture("07_cancelled.valid.json");
    acknowledged.body.insert("target_id", pipeline.requestId);
    CHECK(pipeline.transfer(pipeline.worker, pipeline.coordinator, acknowledged));
    CHECK(pipeline.coordinator.requestCancelled(pipeline.requestId) && pipeline.worker.requestCancelled(pipeline.requestId));
    CHECK(pipeline.adapter.state() == ObservationState::Cancelled);
    CHECK(pipeline.source.cancelCallCount() == 1);
    CHECK(pipeline.adapter.snapshot().resourcesDraining && !pipeline.adapter.snapshot().resourcesDrained);
    CHECK(pipeline.pool.current(pipeline.lease.slotIndex).state == LeaseState::AwaitRelease);
    CHECK(pipeline.coordinator.inFlightCount() == 1 && pipeline.pool.freeCount() == 1);
    ProtocolError error;
    CHECK(!pipeline.coordinator.receive(pipeline.result(), &error) && error.code == QStringLiteral("E_CANCELLED"));
    CHECK(!pipeline.worker.send(pipeline.result(), nullptr, &error) && error.code == QStringLiteral("E_CANCELLED"));
    CHECK(!pipeline.coordinator.requestTerminal(pipeline.requestId));
    CHECK(!pipeline.adapter.watchTick(150));
    CHECK(pipeline.source.captureCallCount() == 1);
    pipeline.release();
    CHECK(pipeline.adapter.state() == ObservationState::Cancelled);
    pipeline.shutdown();
}

void rejectedCaptureNeverDispatched(const QDir& directory)
{
    Pipeline pipeline(directory);
    auto stale = syntheticFrame();
    stale.sourceMonoMs = 99;
    CHECK(pipeline.source.enqueueSyntheticFrame(stale));
    CHECK(pipeline.adapter.start(makeDemand(), 100));
    const int history = pipeline.coordinator.messageHistorySize();
    const auto rejected = pipeline.capture(100);
    CHECK(!rejected.accepted && rejected.errorCode == QStringLiteral("E_STALE_OR_UNPROVEN_FRAME"));
    CHECK(!pipeline.dispatch(rejected, QStringLiteral("stale-request")));
    CHECK(pipeline.coordinator.messageHistorySize() == history);
    CHECK(!pipeline.worker.hasRequest(QStringLiteral("stale-request")) && pipeline.worker.inFlightCount() == 0);
    CHECK(pipeline.pool.freeCount() == 2 && pipeline.pool.releasedHistorySize() == 0);
    CHECK(pipeline.slotBytes[0].isEmpty() && pipeline.slotBytes[1].isEmpty());
    CHECK(pipeline.adapter.snapshot().acceptedFrameCount == 0 && pipeline.adapter.snapshot().resourcesDrained);
    pipeline.shutdown();
}

void oversizedBeforeAllocation(const QDir& directory)
{
    for (int dimension : {std::numeric_limits<int>::max(), 8192}) {
        Pipeline pipeline(directory);
        auto oversized = syntheticFrame();
        oversized.width = dimension;
        oversized.height = dimension; // 8192 squared BGRA8 is above the 128 MiB budget.
        CHECK(oversized.pixels.isEmpty());
        CHECK(pipeline.source.enqueueSyntheticFrame(oversized)); // Metadata only; materialization validates before allocation.
        CHECK(pipeline.source.queuedPixelBytes() == 0);
        CHECK(pipeline.adapter.start(makeDemand(), 100));
        const auto reply = pipeline.adapter.acquire(100);
        CHECK(reply.status == CaptureStatus::Failed);
        CHECK(reply.errorCode == (dimension == 8192 ? QStringLiteral("E_SHM_BOUNDS") : QStringLiteral("E_FRAME_SHAPE")));
        CHECK(reply.frame.pixels.isEmpty());
        CHECK(!pipeline.dispatch(pipeline.adapter.consumePending(100), QStringLiteral("oversized-request")));
        CHECK(pipeline.source.queuedReplyCount() == 0 && pipeline.source.queuedPixelBytes() == 0);
        CHECK(pipeline.source.captureCallCount() == 1);
        CHECK(!pipeline.pool.beginWrite(pipeline.pool.slotCapacityBytes() + 1).ok);
        CHECK(pipeline.pool.freeCount() == 2 && pipeline.pool.releasedHistorySize() == 0);
        CHECK(pipeline.slotBytes[0].isEmpty() && pipeline.slotBytes[1].isEmpty());
        CHECK(pipeline.coordinator.inFlightCount() == 0 && pipeline.worker.inFlightCount() == 0);
        CHECK(pipeline.coordinator.messageHistorySize() == 2);
        pipeline.shutdown();
    }
}

void boundedWatchFinalFrameDrain(const QDir& directory)
{
    Pipeline pipeline(directory);
    CHECK(pipeline.source.enqueueSyntheticFrame(syntheticFrame()));
    CHECK(pipeline.source.enqueueSyntheticFrame(syntheticFrame()));
    CHECK(pipeline.adapter.start(makeDemand(ObservationMode::BoundedWatch), 100));
    auto frame = pipeline.capture(100);
    CHECK(pipeline.dispatch(frame, QStringLiteral("watch-request-1")));
    CHECK(!pipeline.adapter.watchTick(150)); // A due tick cannot overwrite an in-use frame.
    CHECK(pipeline.source.captureCallCount() == 1);
    CHECK(pipeline.transfer(pipeline.worker, pipeline.coordinator, pipeline.result()));
    const LeaseHandle firstLease = pipeline.lease;
    pipeline.release();
    CHECK(pipeline.adapter.watchTick(150));
    frame = pipeline.adapter.consumePending(150);
    CHECK(frame.accepted);
    CHECK(pipeline.dispatch(frame, QStringLiteral("watch-request-2")));
    CHECK(pipeline.lease.slotIndex == firstLease.slotIndex && pipeline.lease.generation > firstLease.generation);
    CHECK(pipeline.lease.leaseId != firstLease.leaseId);
    CHECK(pipeline.pool.release(firstLease)); // Old exact release is idempotent, not a current-slot release.
    CHECK(pipeline.pool.current(pipeline.lease.slotIndex).state == LeaseState::InUse);
    CHECK(pipeline.pool.freeCount() == 1);
    CHECK(!pipeline.adapter.watchTick(200));
    CHECK(pipeline.source.captureCallCount() == 2 && pipeline.adapter.snapshot().requestCount == 2);
    pipeline.adapter.close();
    CHECK(pipeline.adapter.snapshot().resourcesDraining && pipeline.adapter.state() != ObservationState::Closed);
    CHECK(pipeline.transfer(pipeline.worker, pipeline.coordinator, pipeline.result()));
    pipeline.release();
    CHECK(pipeline.adapter.state() == ObservationState::Closed && pipeline.adapter.snapshot().resourcesDrained);
    CHECK(pipeline.source.queuedReplyCount() == 0 && pipeline.source.queuedPixelBytes() == 0);
    pipeline.shutdown();
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QDir directory(argc > 1 ? QString::fromLocal8Bit(argv[1])
        : QStringLiteral("docs/business_rebuild/implementation/runtime/fixtures/messages"));
    successAndCorrelation(directory);
    cancellation(directory);
    rejectedCaptureNeverDispatched(directory);
    oversizedBeforeAllocation(directory);
    boundedWatchFinalFrameDrain(directory);
    std::cout << "VISION_PIPELINE_TESTS=" << (failures == 0 ? "PASS" : "FAIL")
              << "; assertions=" << assertions << "; failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
