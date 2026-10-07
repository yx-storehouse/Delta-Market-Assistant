#include "application/vision/worker_protocol.h"
#include "application/vision/lease_pool.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <iostream>

using namespace relink::vision;

namespace {
int assertions = 0, failures = 0;
void check(bool ok, const QString& label)
{
    ++assertions;
    if (!ok) ++failures;
    std::cout << (ok ? "PASS " : "FAIL ") << label.toStdString() << '\n';
}

WorkerEnvelope read(const QString& path)
{
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), QStringLiteral("fixture_open_") + QFileInfo(path).fileName());
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    WorkerEnvelope envelope;
    envelope.protocolVersion=object.value(QStringLiteral("protocol_version")).toInt();
    envelope.sessionId=object.value(QStringLiteral("session_id")).toString();
    envelope.messageId=object.value(QStringLiteral("message_id")).toString();
    envelope.kind=object.value(QStringLiteral("kind")).toString();
    envelope.body=object.value(QStringLiteral("body")).toObject();
    return envelope;
}

void fixtures(const QDir& directory)
{
    const auto paths = directory.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    check(paths.size() == 18, QStringLiteral("documented_worker_fixture_count"));
    for (const auto& name : paths) {
        const auto envelope = read(directory.filePath(name));
        ProtocolError error;
        const bool valid = WorkerProtocol::validateEnvelope(envelope, {}, &error);
        const bool expected = name.contains(QStringLiteral(".valid."));
        check(valid == expected, QStringLiteral("schema_fixture_") + name);
        if (valid) {
            WorkerEnvelope parsed;
            const auto bytes=WorkerProtocol::encodeLine(envelope,&error);
            check(!bytes.isEmpty() && bytes.endsWith('\n'), QStringLiteral("encode_")+name);
            check(WorkerProtocol::decodeLine(bytes.chopped(1),&parsed,&error)
                      && parsed.body==envelope.body && parsed.kind==envelope.kind,
                  QStringLiteral("roundtrip_")+name);
        }
    }
}

void framing(const WorkerEnvelope& hello)
{
    const QByteArray line=WorkerProtocol::encodeLine(hello);
    for (int split=0;split<line.size();++split) {
        NdjsonFramer framer;
        const auto a=framer.feed(line.left(split));
        const auto b=framer.feed(line.mid(split));
        check(a.ok() && b.ok() && a.messages.size()+b.messages.size()==1 && framer.finish().ok(),
              QStringLiteral("every_chunk_boundary_%1").arg(split));
    }
    NdjsonFramer partial;
    check(partial.feed(line.chopped(1)).ok() && !partial.finish().ok(),QStringLiteral("partial_final_line_rejected"));
    NdjsonFramer blank;
    check(!blank.feed("\n").ok(),QStringLiteral("blank_line_rejected"));
    NdjsonFramer bom;
    check(bom.feed(QByteArray("\xEF",1)).ok() && !bom.feed(QByteArray("\xBB\xBF",2)+line).ok(),QStringLiteral("split_bom_rejected"));
    NdjsonFramer huge;
    check(!huge.feed(QByteArray(1048577,'x')).ok() && huge.failed(),QStringLiteral("unbounded_partial_line_rejected"));
    NdjsonFramer invalidUtf8;
    check(!invalidUtf8.feed(QByteArray("\xFF\n",2)).ok(),QStringLiteral("invalid_utf8_rejected"));
    WorkerEnvelope ignored;
    ProtocolError error;
    check(!WorkerProtocol::decodeLine(line.chopped(1)+" {}",&ignored,&error),QStringLiteral("two_json_objects_rejected"));
    check(!WorkerProtocol::decodeLine("[]",&ignored,&error),QStringLiteral("non_object_rejected"));
}

void lifecycle(const QDir& directory)
{
    auto hello=read(directory.filePath(QStringLiteral("01_hello.valid.json")));
    auto ready=read(directory.filePath(QStringLiteral("02_ready.valid.json")));
    auto recognize=read(directory.filePath(QStringLiteral("03_recognize.valid.json")));
    auto result=read(directory.filePath(QStringLiteral("04_result.valid.json")));
    auto release=read(directory.filePath(QStringLiteral("05_release_frame.valid.json")));
    auto cancel=read(directory.filePath(QStringLiteral("06_cancel.valid.json")));
    auto cancelled=read(directory.filePath(QStringLiteral("07_cancelled.valid.json")));
    auto shutdown=read(directory.filePath(QStringLiteral("09_shutdown.valid.json")));
    auto bye=read(directory.filePath(QStringLiteral("10_bye.valid.json")));
    WorkerProtocol coordinator(hello.sessionId,WorkerRole::Coordinator);
    WorkerProtocol worker(hello.sessionId,WorkerRole::Worker);
    ProtocolError error;
    check(!coordinator.send(recognize,nullptr,&error) && error.code==QStringLiteral("E_HANDSHAKE_REQUIRED")
              && !coordinator.hasRequest(QStringLiteral("request-1")),QStringLiteral("recognize_before_handshake_no_mutation"));
    check(!coordinator.receive(ready,&error),QStringLiteral("ready_without_hello_rejected"));
    check(coordinator.send(hello,nullptr,&error) && worker.receive(hello,&error),QStringLiteral("hello_both_roles"));
    check(worker.send(ready,nullptr,&error) && coordinator.receive(ready,&error),QStringLiteral("ready_both_roles"));
    check(coordinator.send(recognize,nullptr,&error) && worker.receive(recognize,&error),QStringLiteral("recognize_both_roles"));
    check(coordinator.inFlightCount()==1 && worker.inFlightCount()==1,QStringLiteral("one_inflight_both_roles"));
    auto wrong=result; wrong.body.insert(QStringLiteral("frame_id"),QStringLiteral("other-frame"));
    check(!coordinator.receive(wrong,&error) && error.code==QStringLiteral("E_CONTEXT_MISMATCH"),QStringLiteral("frame_correlation_rejected"));
    wrong=result; wrong.body.insert(QStringLiteral("provider"),QStringLiteral("other-provider"));
    check(!coordinator.receive(wrong,&error) && error.code==QStringLiteral("E_MODEL_MISMATCH"),QStringLiteral("model_correlation_rejected"));
    check(!coordinator.receive(release,&error) && !coordinator.requestReleased(QStringLiteral("request-1")),QStringLiteral("release_before_terminal_rejected"));
    check(worker.send(result,nullptr,&error) && coordinator.receive(result,&error),QStringLiteral("result_both_roles"));
    wrong=result; wrong.messageId=QStringLiteral("result-second");
    check(!coordinator.receive(wrong,&error) && error.code==QStringLiteral("E_MESSAGE_INVALID"),QStringLiteral("second_terminal_rejected"));
    wrong=release; wrong.body.insert(QStringLiteral("slot_generation"),2);
    check(!coordinator.receive(wrong,&error) && error.code==QStringLiteral("E_LEASE_UNKNOWN"),QStringLiteral("wrong_release_generation_rejected"));
    check(worker.send(release,nullptr,&error) && coordinator.receive(release,&error),QStringLiteral("release_both_roles"));
    check(coordinator.receive(release,&error) && coordinator.inFlightCount()==0,QStringLiteral("duplicate_release_idempotent"));
    check(coordinator.send(shutdown,nullptr,&error) && worker.receive(shutdown,&error)
              && worker.send(bye,nullptr,&error) && coordinator.receive(bye,&error),QStringLiteral("drained_shutdown_both_roles"));

    WorkerProtocol c(hello.sessionId,WorkerRole::Coordinator);
    check(c.send(hello) && c.receive(ready) && c.send(recognize),QStringLiteral("cancel_setup"));
    check(c.send(cancel) && c.receive(cancelled) && c.requestCancelled(QStringLiteral("request-1")),QStringLiteral("cancel_ack_not_release"));
    check(c.inFlightCount()==1 && !c.requestReleased(QStringLiteral("request-1")),QStringLiteral("cancel_retains_lease"));
    check(!c.receive(result,&error) && error.code==QStringLiteral("E_CANCELLED"),QStringLiteral("late_result_after_cancel_rejected"));
    check(c.receive(release,&error) && c.inFlightCount()==0,QStringLiteral("release_after_cancel_drains"));
    auto extra=result; extra.body.insert(QStringLiteral("image_path"),QStringLiteral("x.png"));
    check(!WorkerProtocol::validateEnvelope(extra,{},&error),QStringLiteral("unexpected_result_image_path_rejected"));
    auto tokens=result.body.value(QStringLiteral("tokens")).toArray();
    while(tokens.size()<=2000) tokens.append(tokens.first());
    extra=result; extra.body.insert(QStringLiteral("tokens"),tokens);
    check(!WorkerProtocol::validateEnvelope(extra,{},&error),QStringLiteral("tokens_bounded"));
}

void protocolHardening(const QDir& directory)
{
    const auto hello=read(directory.filePath(QStringLiteral("01_hello.valid.json")));
    const auto ready=read(directory.filePath(QStringLiteral("02_ready.valid.json")));
    const auto recognize=read(directory.filePath(QStringLiteral("03_recognize.valid.json")));
    const auto result=read(directory.filePath(QStringLiteral("04_result.valid.json")));
    const auto release=read(directory.filePath(QStringLiteral("05_release_frame.valid.json")));
    const auto cancel=read(directory.filePath(QStringLiteral("06_cancel.valid.json")));
    const auto cancelled=read(directory.filePath(QStringLiteral("07_cancelled.valid.json")));
    const auto workerError=read(directory.filePath(QStringLiteral("08_error.valid.json")));
    const auto shutdown=read(directory.filePath(QStringLiteral("09_shutdown.valid.json")));
    const auto bye=read(directory.filePath(QStringLiteral("10_bye.valid.json")));
    ProtocolError error;
    WorkerProtocol c(hello.sessionId,WorkerRole::Coordinator);
    check(c.send(hello) && c.receive(ready),QStringLiteral("hardening_handshake"));
    auto malformed=recognize;
    auto frame=malformed.body.value(QStringLiteral("frame")).toObject();
    frame.insert(QStringLiteral("stride_bytes"),399);
    malformed.body.insert(QStringLiteral("frame"),frame);
    check(!c.send(malformed,nullptr,&error) && error.code==QStringLiteral("E_SHM_BOUNDS")
              && c.inFlightCount()==0 && c.messageHistorySize()==2,QStringLiteral("undersized_bgra_stride_no_mutation"));
    malformed=recognize; frame=malformed.body.value(QStringLiteral("frame")).toObject();
    auto descriptor=frame.value(QStringLiteral("descriptor")).toObject();
    descriptor.insert(QStringLiteral("byte_length"),19999);
    frame.insert(QStringLiteral("descriptor"),descriptor); malformed.body.insert(QStringLiteral("frame"),frame);
    check(!c.send(malformed,nullptr,&error) && error.code==QStringLiteral("E_SHM_BOUNDS"),QStringLiteral("descriptor_valid_bytes_exact"));
    descriptor.insert(QStringLiteral("byte_length"),20000);
    descriptor.insert(QStringLiteral("mapping_name"),QStringLiteral("Local\\RelinkVision_session-1_1"));
    frame.insert(QStringLiteral("descriptor"),descriptor); malformed.body.insert(QStringLiteral("frame"),frame);
    check(!c.send(malformed,nullptr,&error) && error.code==QStringLiteral("E_SHM_OPEN"),QStringLiteral("descriptor_mapping_slot_exact"));
    descriptor.insert(QStringLiteral("mapping_name"),QStringLiteral("Local\\RelinkVision_session-1_0"));
    descriptor.insert(QStringLiteral("capacity_bytes"),24000);
    frame.insert(QStringLiteral("descriptor"),descriptor); malformed.body.insert(QStringLiteral("frame"),frame);
    check(!c.send(malformed,nullptr,&error) && error.code==QStringLiteral("E_LEASE_UNKNOWN"),QStringLiteral("negotiated_pool_capacity_exact"));
    malformed=recognize;
    auto rois=malformed.body.value(QStringLiteral("roi_specs")).toArray();
    rois.append(rois.first()); malformed.body.insert(QStringLiteral("roi_specs"),rois);
    check(!c.send(malformed,nullptr,&error) && c.inFlightCount()==0,QStringLiteral("duplicate_roi_no_request_insert"));
    malformed=recognize; frame=malformed.body.value(QStringLiteral("frame")).toObject();
    frame.insert(QStringLiteral("width"),99); malformed.body.insert(QStringLiteral("frame"),frame);
    check(!c.send(malformed,nullptr,&error) && error.code==QStringLiteral("E_ROI_OUT_OF_BOUNDS"),QStringLiteral("roi_must_fit_actual_frame"));
    check(c.send(recognize),QStringLiteral("rejected_message_id_reusable_for_corrected_input"));
    const int before=c.messageHistorySize();
    auto unsolicited=cancelled; unsolicited.messageId=QStringLiteral("unsolicited-cancelled");
    check(!c.receive(unsolicited,&error) && !c.requestCancelled(QStringLiteral("request-1"))
              && c.messageHistorySize()==before,QStringLiteral("unsolicited_cancel_ack_atomic"));
    check(!c.send(recognize,nullptr,&error) && error.code==QStringLiteral("E_DUPLICATE_MESSAGE")
              && c.inFlightCount()==1,QStringLiteral("duplicate_recognize_not_idempotent"));
    check(c.send(cancel),QStringLiteral("cancel_registered"));
    auto changedCancel=cancel; changedCancel.body.insert(QStringLiteral("reason"),QStringLiteral("deadline"));
    check(!c.send(changedCancel,nullptr,&error) && error.code==QStringLiteral("E_DUPLICATE_MESSAGE"),QStringLiteral("duplicate_id_changed_payload_rejected"));
    check(c.send(cancel) && c.receive(cancelled),QStringLiteral("exact_cancel_retransmission_idempotent"));
    check(c.receive(release),QStringLiteral("cancelled_release_drains_without_terminal"));
    auto repeatedRelease=release; repeatedRelease.messageId=QStringLiteral("release-new-id");
    check(!c.receive(repeatedRelease,&error) && error.code==QStringLiteral("E_DUPLICATE_MESSAGE"),QStringLiteral("release_retry_requires_original_message_id"));
    check(c.receive(release),QStringLiteral("original_release_retry_remains_idempotent"));

    ProtocolLimits small; small.maxSessionMessages=8;
    WorkerProtocol bounded(hello.sessionId,WorkerRole::Coordinator,small);
    check(bounded.send(hello) && bounded.receive(ready) && bounded.send(recognize),QStringLiteral("bounded_history_setup"));
    auto filler=workerError; filler.body.insert(QStringLiteral("request_id"),QJsonValue::Null);
    for (int i=0;i<5;++i) {
        filler.messageId=QStringLiteral("session-error-%1").arg(i);
        check(bounded.receive(filler),QStringLiteral("bounded_history_fill_%1").arg(i));
    }
    filler.messageId=QStringLiteral("session-error-overflow");
    check(!bounded.receive(filler,&error) && error.code==QStringLiteral("E_MESSAGE_TOO_LARGE")
              && bounded.messageHistorySize()==8,QStringLiteral("session_history_rejects_growth_at_soft_limit"));
    check(bounded.receive(result) && bounded.requestTerminal(QStringLiteral("request-1")),QStringLiteral("terminal_reserved_at_history_limit"));
    check(bounded.receive(release),QStringLiteral("lease_release_reserved_at_history_limit"));
    check(bounded.send(shutdown) && bounded.receive(bye) && bounded.state()==SessionState::Closed
              && bounded.messageHistorySize()==12,QStringLiteral("drained_close_reserved_and_history_hard_bounded"));
    WorkerProtocol shutdownBounded(hello.sessionId,WorkerRole::Coordinator,small);
    check(shutdownBounded.send(hello) && shutdownBounded.receive(ready) && shutdownBounded.send(recognize),QStringLiteral("shutdown_history_setup"));
    for (int i=0;i<5;++i) { filler.messageId=QStringLiteral("shutdown-error-%1").arg(i); check(shutdownBounded.receive(filler),QStringLiteral("shutdown_history_fill_%1").arg(i)); }
    check(shutdownBounded.send(shutdown) && shutdownBounded.requestCancelled(QStringLiteral("request-1")),QStringLiteral("shutdown_cancels_even_at_soft_limit"));
    check(!shutdownBounded.receive(bye,&error) && shutdownBounded.state()==SessionState::ShuttingDown
              && shutdownBounded.inFlightCount()==1,QStringLiteral("premature_bye_does_not_close_or_consume_identity"));
    check(shutdownBounded.receive(release) && shutdownBounded.receive(bye),QStringLiteral("release_then_same_bye_succeeds"));

    const auto readyLine=WorkerProtocol::encodeLine(ready);
    ProtocolLimits exact; exact.maxLineBytes=readyLine.size()-1;
    const auto exactLine=WorkerProtocol::encodeLine(ready,&error,exact);
    WorkerEnvelope parsed;
    check(!exactLine.isEmpty() && WorkerProtocol::decodeLine(exactLine.chopped(1),&parsed,&error,exact),QStringLiteral("line_limit_excludes_newline_consistently"));
    NdjsonFramer exactFramer(exact);
    check(exactFramer.feed(exactLine).ok() && exactFramer.finish().ok(),QStringLiteral("framer_accepts_exact_line_budget"));
    check(!WorkerProtocol::decodeLine(exactLine.chopped(1)+" ",&parsed,&error,exact),QStringLiteral("line_budget_plus_one_rejected"));
    NdjsonFramer broken;
    check(!broken.feed("[]\n").ok() && !broken.finish().ok() && broken.failed(),QStringLiteral("framer_finish_preserves_fault"));
    broken.reset(); check(broken.feed(readyLine).ok() && broken.finish().ok(),QStringLiteral("framer_explicit_reset_recovers"));
    ProtocolLimits invalid; invalid.maxFrameBytes=134217729;
    check(!WorkerProtocol::validateEnvelope(ready,invalid,&error) && error.isError(),QStringLiteral("unsafe_protocol_limits_rejected"));
    NdjsonFramer invalidFramer(invalid);
    check(!invalidFramer.feed(readyLine).ok(),QStringLiteral("unsafe_framer_limits_rejected"));
    ProtocolLimits wireLimit; wireLimit.maxLineBytes=1024;
    auto smallHello=hello; smallHello.body.insert(QStringLiteral("max_line_bytes"),1024);
    WorkerProtocol direct(hello.sessionId,WorkerRole::Coordinator,wireLimit);
    check(direct.send(smallHello) && direct.receive(ready),QStringLiteral("direct_receive_budget_setup"));
    auto largeError=workerError; largeError.body.insert(QStringLiteral("request_id"),QJsonValue::Null);
    largeError.body.insert(QStringLiteral("message"),QString(1024,QChar('x')));
    check(!direct.receive(largeError,&error) && error.code==QStringLiteral("E_MESSAGE_TOO_LARGE")
              && direct.messageHistorySize()==2,QStringLiteral("direct_receive_cannot_bypass_ndjson_byte_limit"));
    malformed=recognize; rois=malformed.body.value(QStringLiteral("roi_specs")).toArray();
    auto roi=rois.first().toObject(); roi.insert(QStringLiteral("coordinate_space"),QStringLiteral("wrong"));
    rois[0]=roi; malformed.body.insert(QStringLiteral("roi_specs"),rois);
    error={}; check(!WorkerProtocol::validateEnvelope(malformed,{},&error) && error.isError(),QStringLiteral("invalid_body_always_provides_error"));
    check(WorkerProtocol::validateEnvelope(ready,{},&error) && !error.isError(),QStringLiteral("successful_validation_clears_stale_error"));
}

void leaseHardening()
{
    LeasePool pool(4096);
    const auto first=pool.beginWrite(64);
    check(first.ok && pool.cancel(first.lease) && pool.cancel(first.lease),QStringLiteral("local_cancel_exact_retry_idempotent"));
    auto forged=first.lease; ++forged.generation;
    check(!pool.release(forged),QStringLiteral("released_id_does_not_authorize_wrong_generation"));
    forged=first.lease; forged.slotIndex=1-forged.slotIndex;
    check(!pool.release(forged),QStringLiteral("released_id_does_not_authorize_wrong_slot"));
    LeaseHandle last;
    bool churnOk=true;
    for (int i=0;i<LeasePool::releasedHistoryLimit+16;++i) {
        auto writing=pool.beginWrite(64);
        churnOk=churnOk && writing.ok && pool.cancel(writing.lease);
        last=writing.lease;
    }
    check(churnOk && pool.releasedHistorySize()==LeasePool::releasedHistoryLimit,QStringLiteral("released_lease_history_bounded_under_churn"));
    check(!pool.release(first.lease) && pool.release(last),QStringLiteral("expired_release_fails_closed_recent_release_idempotent"));
    const auto inUse=pool.beginWrite(64), pending=pool.beginWrite(64);
    check(pool.publish(inUse.lease) && pool.acquireForWorker(inUse.lease) && pool.publish(pending.lease)
              && pool.workerUnresponsive(inUse.lease),QStringLiteral("quarantine_with_pending_slot_setup"));
    const auto blocked=pool.replacePending(pending.lease,128);
    check(!blocked.ok && blocked.error==QStringLiteral("E_LEASE_QUARANTINED")
              && pool.current(pending.lease.slotIndex).generation==pending.lease.generation,
          QStringLiteral("pending_replacement_blocked_while_worker_quarantined"));
    check(!pool.release(inUse.lease) && !pool.closeMapping(inUse.lease),QStringLiteral("quarantine_release_needs_exit_confirmation"));
    check(pool.processExitConfirmed(inUse.lease) && pool.closeMapping(inUse.lease)
              && pool.replacePending(pending.lease,128).ok,QStringLiteral("pending_replacement_after_confirmed_exit_close"));
}

void leases()
{
    LeasePool pool(4096);
    check(pool.freeCount()==2,QStringLiteral("two_slots_initially_free"));
    const auto a=pool.beginWrite(64), b=pool.beginWrite(64);
    check(a.ok && b.ok && !pool.beginWrite(64).ok,QStringLiteral("third_frame_not_queued"));
    check(pool.publish(a.lease) && pool.acquireForWorker(a.lease),QStringLiteral("writing_pending_inuse"));
    check(!pool.replacePending(a.lease,64).ok,QStringLiteral("inuse_never_overwritten"));
    check(pool.publish(b.lease),QStringLiteral("second_slot_pending"));
    const auto replacement=pool.replacePending(b.lease,128);
    check(replacement.ok && replacement.lease.generation>b.lease.generation && replacement.lease.leaseId!=b.lease.leaseId,
          QStringLiteral("pending_replacement_new_identity_generation"));
    check(!pool.publish(b.lease),QStringLiteral("old_generation_rejected"));
    check(pool.cancel(a.lease) && pool.current(a.lease.slotIndex).state==LeaseState::AwaitRelease && pool.freeCount()==0,
          QStringLiteral("cancel_does_not_release_inuse"));
    check(pool.release(a.lease) && pool.release(a.lease),QStringLiteral("exact_release_idempotent"));
    check(pool.publish(replacement.lease) && pool.acquireForWorker(replacement.lease),QStringLiteral("next_request_after_release"));
    check(pool.workerUnresponsive(replacement.lease) && !pool.beginWrite(64).ok,QStringLiteral("crash_quarantines_pool"));
    check(!pool.closeMapping(replacement.lease) && pool.processExitConfirmed(replacement.lease)
              && pool.current(replacement.lease.slotIndex).state==LeaseState::Quarantined,
          QStringLiteral("exit_confirmation_separate_from_mapping_close"));
    check(pool.closeMapping(replacement.lease) && pool.freeCount()==2,QStringLiteral("confirmed_exit_and_closed_mapping_reclaim"));
    check(!pool.beginWrite(4097).ok,QStringLiteral("frame_capacity_bound"));
}
}

int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    const QDir directory(argc>1 ? QString::fromLocal8Bit(argv[1])
        : QStringLiteral("docs/business_rebuild/implementation/runtime/fixtures/messages"));
    fixtures(directory);
    framing(read(directory.filePath(QStringLiteral("01_hello.valid.json"))));
    lifecycle(directory);
    leases();
    protocolHardening(directory);
    leaseHardening();
    std::cout << "WORKER_PROTOCOL_TESTS=" << (failures==0?"PASS":"FAIL")
              << "; assertions=" << assertions << "; failures=" << failures
              << "; external_processes=0; image_file_writes=0\n";
    return failures==0?0:1;
}
