#include "application/vision/latest_frame_stream.h"
#include <QCoreApplication>
#include <QThread>
#include <deque>
#include <iostream>
#include <future>
using namespace relink::vision;
using namespace relink::runtime::observation;
namespace {
int assertions=0, failures=0;
void check(bool ok,const char* name) { ++assertions;if(!ok)++failures;std::cout<<(ok?"PASS ":"FAIL ")<<name<<'\n'; }
struct Feeder {
    std::mutex mutex;std::condition_variable changed;std::deque<CaptureReply> replies;bool cancelled=false;
    int cancelCalls=0;
    CaptureReply pull() { std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[&]{return cancelled||!replies.empty();});
        if(cancelled){CaptureReply r;r.status=CaptureStatus::Cancelled;r.errorCode="TEST_CANCELLED";return r;}
        auto r=std::move(replies.front());replies.pop_front();return r; }
    void cancel(){std::lock_guard<std::mutex> lock(mutex);++cancelCalls;cancelled=true;changed.notify_all();}
    void push(CaptureReply r){std::lock_guard<std::mutex> lock(mutex);replies.push_back(std::move(r));changed.notify_all();}
};
TargetWindow target() {
    TargetWindow t;t.hwnd=1;t.pid=2;t.processCreated=3;t.windowClass="fixture";t.executableName="fixture.exe";
    t.monitor=4;t.clientRect=QRect(0,0,4,4);t.monitorRect=t.clientRect;t.dpi=96;return t;
}
CaptureReply frame(const QString& id,qint64 at=-1) {
    if(at<0)at=captureClockMs();
    CaptureReply r;r.status=CaptureStatus::Captured;
    auto& f=r.frame;f.frameId=id;f.sourceKind="dxgi";f.windowRef="window:2:1:3";
    f.width=f.height=4;f.strideBytes=16;f.validBytes=64;f.pixels=QByteArray(64,'x');
    f.sourceMonoMs=at;f.sourceUncertaintyMs=1;f.captureStartMonoMs=at;f.captureEndMonoMs=at;
    f.freshnessBasis=FreshnessBasis::SourceTimestamp;return r;
}
ObservationDemand demand(qint64 barrier=-1) {
    ObservationDemand d;d.demandId="consumer-demand";d.context={"run","session",captureClockDomain(),"step",0,1};
    d.createdMonoMs=d.notBeforeMonoMs=barrier<0?captureClockMs():barrier;
    d.deadlineMonoMs=captureClockMs()+600;d.maxFrameAgeMs=1000;
    d.roiSpecs={{"client","client_physical_px",0,0,4,4,1}};return d;
}
bool waitFor(const std::function<bool()>& predicate) {
    const auto end=captureClockMs()+800;
    while(captureClockMs()<end){if(predicate())return true;QThread::msleep(1);}return predicate();
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);std::atomic<bool> cancelled{false};
    {
        Feeder f;LatestFramePump p([&]{return f.pull();},[&]{f.cancel();});
        check(!p.metrics()["started"].toBool(),"construction_has_no_capture_thread");
        check(p.take(0,captureClockMs()+50,1000,cancelled).errorCode=="E_FRAME_STREAM_REQUEST","unstarted_read_rejected");
        p.start();p.start();
        for(int i=0;i<5;++i){QThread::msleep(3);f.push(frame(QString::number(i)));}
        check(waitFor([&]{return p.metrics()["produced"].toInt()==5;}),"producer_runs_independently_of_consumer");
        auto m=p.metrics();check(m["pending_frames"]==1&&m["replaced_without_queue"]==4,"only_latest_pending_frame_no_fifo_backlog");
        auto r=p.take(captureClockMs()-100,captureClockMs()+100,1000,cancelled);
        check(r.status==CaptureStatus::Captured&&r.frame.frameId=="4","consumer_receives_newest_frame_not_oldest");
        check(p.metrics()["pending_frames"]==0,"consumed_slot_released");
        check(r.frame.pixels==QByteArray(64,'x'),"pixels_remain_available_to_consumer");
        QThread::msleep(3);f.push(frame("after-consume"));
        check(waitFor([&]{return p.metrics()["produced"].toInt()==6;}),"capture_continues_during_consumer_processing");
        p.stop();p.stop();check(f.cancelCalls==1,"idempotent_stop_cancels_producer_once");
        check(p.metrics()["stopped"]==true&&p.metrics()["pending_frames"]==0,"stop_drains_pending_pixels");
    }
    {
        Feeder f;LatestFramePump p([&]{return f.pull();},[&]{f.cancel();});p.start();
        f.push(frame("old",captureClockMs()-200));
        check(waitFor([&]{return p.metrics()["produced"]==1;}),"old_frame_produced_before_request");
        const auto barrier=captureClockMs();
        auto future=std::async(std::launch::async,[&]{return p.take(barrier,captureClockMs()+400,1000,cancelled);});
        QThread::msleep(8);check(future.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout,"pre_click_cached_pixels_never_satisfy_new_request");
        f.push(frame("fresh"));auto r=future.get();
        check(r.status==CaptureStatus::Captured&&r.frame.frameId=="fresh"&&r.frame.sourceMonoMs-1>=barrier,"fresh_request_barrier_and_uncertainty_preserved");
        check(p.metrics()["rejected_pre_barrier_or_stale"]==1,"discarded_old_frame_counted");
        auto timeout=p.take(captureClockMs(),captureClockMs()+15,1000,cancelled);
        check(timeout.errorCode=="E_CAPTURE_DEADLINE","bounded_wait_has_real_deadline");
        std::atomic<bool> stop{false};auto interrupted=std::async(std::launch::async,[&]{return p.take(captureClockMs(),captureClockMs()+400,1000,stop);});
        QThread::msleep(5);stop.store(true);p.wake();
        check(interrupted.get().status==CaptureStatus::Cancelled,"cancellation_interrupts_wait");
        p.stop();
    }
    {
        Feeder f;LatestFramePump p([&]{return f.pull();},[&]{f.cancel();});p.start();
        auto d=demand();LatestFrameObservationSource source(p,target(),d,[](const auto&){return QString();});
        ObservationAdapter adapter(&source);QString error;check(adapter.start(d,captureClockMs(),&error),"consumer_uses_original_observation_adapter");
        const auto request=adapter.beginAcquire(captureClockMs(),&error);
        auto future=std::async(std::launch::async,[&]{return source.capture(request);});
        QThread::msleep(4);auto physical=frame("physical-acquisition");f.push(physical);
        auto reply=future.get();auto completed=adapter.completeCapture(reply,captureClockMs());auto accepted=adapter.consumePending(captureClockMs());
        check(completed.status==CaptureStatus::Captured&&accepted.accepted,"streamed_frame_passes_unchanged_adapter");
        check(accepted.frame.frameId==physical.frame.frameId&&accepted.frame.sourceMonoMs==physical.frame.sourceMonoMs
            &&accepted.frame.captureStartMonoMs==physical.frame.captureStartMonoMs,"actual_acquisition_identity_and_times_unchanged");
        check(accepted.frame.context.sessionId==d.context.sessionId&&accepted.frame.demandId==d.demandId,"consumer_delivery_context_is_current_demand");
        check(adapter.releaseFrame(accepted.frame.leaseId,accepted.frame.slotGeneration),"consumer_lease_released_normally");
        adapter.close();check(!p.metrics()["stopped"].toBool(),"closing_one_request_does_not_destroy_stream");
        check(source.capture(request).errorCode=="E_CAPTURE_REQUEST_INVALID","same_request_not_replayed");
        p.stop();
    }
    for(const auto& mode:{QString("window"),QString("shape"),QString("dpi")}) {
        Feeder f;LatestFramePump p([&]{return f.pull();},[&]{f.cancel();});p.start();auto d=demand();
        LatestFrameObservationSource source(p,target(),d,[](const auto&){return QString();});
        auto future=std::async(std::launch::async,[&]{return source.capture({d,"request",1,d.notBeforeMonoMs});});
        QThread::msleep(3);auto r=frame("mismatch");
        if(mode=="window")r.frame.windowRef="window:7:8:9";
        if(mode=="shape"){r.frame.width=3;}
        if(mode=="dpi")r.frame.dpiX=144;
        f.push(r);check(future.get().errorCode=="E_FRAME_STREAM_BINDING","wrong_target_shape_or_dpi_never_delivered");p.stop();
    }
    {
        Feeder f;LatestFramePump p([&]{return f.pull();},[&]{f.cancel();});p.start();auto d=demand();
        int calls=0;LatestFrameObservationSource source(p,target(),d,[&](const auto&){return ++calls==1?QString():QString("E_TARGET_CHANGED");});
        auto future=std::async(std::launch::async,[&]{return source.capture({d,"request",1,d.notBeforeMonoMs});});
        QThread::msleep(3);f.push(frame("after-check"));
        check(future.get().errorCode=="E_TARGET_CHANGED","target_revalidated_after_wait_before_use");p.stop();
    }
    for(int mode=0;mode<3;++mode) {
        Feeder f;LatestFramePump p([&]{return f.pull();},[&]{f.cancel();});p.start();auto r=frame("first");
        if(mode==0)r.frame.pixels.clear();
        else if(mode==1)r.frame.sourceMonoMs=r.frame.captureEndMonoMs+100;
        else r.frame.freshnessBasis=FreshnessBasis::Unproven;
        f.push(r);check(waitFor([&]{return p.metrics()["stopped"].toBool();}),"invalid_producer_frame_terminates_stream");
        check(p.take(0,captureClockMs()+100,1000,cancelled).errorCode=="E_FRAME_STREAM_INVALID_FRAME","invalid_data_not_returned_as_pixels");p.stop();
    }
    {
        Feeder f;LatestFramePump p([&]{return f.pull();},[&]{f.cancel();});p.start();
        auto old=frame("aged",captureClockMs()-300);f.push(old);
        check(waitFor([&]{return p.metrics()["produced"]==1;}),"aged_frame_available_before_tight_age_limit");
        check(p.take(captureClockMs()-500,captureClockMs()+15,20,cancelled).errorCode=="E_CAPTURE_DEADLINE",
            "old_frame_rejected_even_after_request_barrier");
        p.stop();
    }
    {
        Feeder f;LatestFramePump p([&]{return f.pull();},[&]{f.cancel();});p.start();
        auto d=demand();d.roiSpecs[0].width=5;
        LatestFrameObservationSource source(p,target(),d,[](const auto&){return QString();});
        check(source.capture({d,"roi-outside",1,d.notBeforeMonoMs}).errorCode=="E_ROI_OUTSIDE_FRAME","stream_keeps_original_roi_target_bounds");
        p.stop();
    }
    {
        Feeder f;LatestFramePump p([&]{return f.pull();},[&]{f.cancel();});p.start();auto d=demand();
        LatestFrameObservationSource source(p,target(),d,[](const auto&){return QString();});
        auto wrong=d;wrong.context.cancelEpoch++;
        source.cancel(d.demandId,wrong.context);
        auto future=std::async(std::launch::async,[&]{return source.capture({d,"still-active",1,d.notBeforeMonoMs});});
        QThread::msleep(3);f.push(frame("not-cancelled"));
        check(future.get().status==CaptureStatus::Captured,"wrong_cancel_context_does_not_cancel_current_demand");p.stop();
    }
    {
        Feeder f;LatestFramePump p([&]{return f.pull();},[&]{f.cancel();});p.start();
        auto future=std::async(std::launch::async,[&]{return p.take(captureClockMs(),captureClockMs()+500,1000,cancelled);});
        CaptureReply error;error.errorCode="E_TARGET_FOREGROUND";f.push(error);
        check(future.get().errorCode=="E_TARGET_FOREGROUND","producer_failure_wakes_consumer_with_original_error");p.stop();
    }
    {
        int cancels=0;LatestFramePump p([]()->CaptureReply{throw std::runtime_error("fixture");},[&]{++cancels;});p.start();
        check(waitFor([&]{return p.metrics()["stopped"].toBool();}),"producer_exception_is_contained");
        check(p.take(0,captureClockMs()+100,1000,cancelled).errorCode=="E_FRAME_STREAM_EXCEPTION","exception_has_terminal_error_not_old_frame");
        p.stop();check(cancels==1,"failed_worker_is_joined_and_cancelled_once");
    }
    {
        Feeder f;LatestFramePump p([&]{return f.pull();},[&]{f.cancel();});p.start();auto a=frame("same");f.push(a);
        check(waitFor([&]{return p.metrics()["produced"]==1;}),"one_valid_frame_before_duplicate");
        QThread::msleep(3);f.push(frame("same"));
        check(waitFor([&]{return p.metrics()["stopped"].toBool();}),"duplicate_acquisition_id_is_terminal");
        check(p.metrics()["pending_frames"]==0,"fault_discards_even_previous_pending_frame");p.stop();
    }
    std::cout<<"LATEST_FRAME_STREAM_TESTS="<<(failures?"FAIL":"PASS")<<"; assertions="<<assertions<<"; failures="<<failures<<"; game_capture=false; image_file_writes=0\n";
    return failures?1:0;
}
