#include "application/vision/collection_page_context.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <functional>
#include <iostream>

using relink::runtime::observation::FrameEnvelope;
using relink::vision::CollectionPageContext;
using relink::vision::TargetWindow;

namespace {
QString digest(const FrameEnvelope& frame) {
    return QString::fromLatin1(QCryptographicHash::hash(frame.pixels,QCryptographicHash::Sha256).toHex());
}
FrameEnvelope sample() {
    FrameEnvelope f;
    f.frameId="synthetic:full-calibration";f.sourceMonoMs=1000;
    f.width=2560;f.height=1440;f.dpiX=f.dpiY=144;f.pixelFormat="BGRA8";
    f.strideBytes=2560*4;f.validBytes=qint64(f.strideBytes)*f.height;
    f.pixels=QByteArray(f.validBytes,'\x31');
    return f;
}
TargetWindow target() {
    TargetWindow t;
    t.hwnd=123;t.pid=456;t.processCreated=789;t.windowClass="SyntheticWindow";
    t.executableName="synthetic.exe";t.clientRect=QRect(0,0,2560,1440);
    t.monitorRect=QRect(0,0,2560,1440);t.monitor=321;t.dpi=144;
    return t;
}
QJsonObject packet(const FrameEnvelope& f) {
    const auto sha=digest(f);
    const QJsonObject word{{"text","default sort"},{"x",1153},{"y",253},{"width",85},{"height",20}};
    const QJsonObject controls{{"kind","listing_controls"},{"ok",true},{"truncated",false},
        {"words",QJsonArray{word}}};
    QJsonObject out{{"capture_passed",true},{"ocr_passed",true},{"full_frame_ocr_performed",true}};
    out["ocr"]=QJsonObject{{"coverage","full_client"},{"full_frame_ocr_performed",true},
        {"frame_id",f.frameId},{"frame_sha256",sha}};
    QJsonObject page{{"valid_input",true},{"page","skin_listings"},{"overlay","none"},
        {"actions_enabled",false},{"token_count",150},{"low_score_tokens",0}};
    page["anchor_checks"]=QJsonObject{{"listing.sale",true},{"listing.default_sort",true},
        {"collection.added",true},{"collection.full",true}};
    out["startup_page"]=page;
    out["collection_layout"]=QJsonObject{{"complete",true},{"same_frame",true},
        {"frame_id",f.frameId},{"frame_sha256",sha},{"cards",QJsonArray{"OLD_CARD_DO_NOT_REUSE"}}};
    out["collection_selected_card"]=QJsonObject{{"favorite_warm_fraction",0.15},{"identity","OLD_STAR_DO_NOT_REUSE"}};
    QJsonArray regions{controls};
    regions.append(QJsonObject{{"kind","product_title"},{"words",QJsonArray{"OLD_TITLE_DO_NOT_REUSE"}}});
    regions.append(QJsonObject{{"kind","card_fields"},{"words",QJsonArray{"OLD_PRICE_DO_NOT_REUSE"}}});
    regions.append(QJsonObject{{"kind","selected_detail"},{"words",QJsonArray{"OLD_WEAR_DO_NOT_REUSE"}}});
    out["collection_observation"]=QJsonObject{{"same_frame",true},{"frame_id",f.frameId},
        {"frame_sha256",sha},{"regions",regions}};
    return out;
}
void field(QJsonObject& p,const char* object,const char* key,const QJsonValue& value) {
    auto obj=p[object].toObject();obj[key]=value;p[object]=obj;
}
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    int checks=0,failures=0;
    auto expect=[&](bool ok,const QString& name){++checks;if(!ok){++failures;std::cout<<"FAIL "<<name.toStdString()<<'\n';}};
    const auto base=sample();const auto window=target();const auto full=packet(base);
    auto fresh=base;fresh.frameId="synthetic:new";fresh.sourceMonoMs=1001;
    const auto sha=digest(fresh);
    CollectionPageContext context;
    auto miss=context.check(fresh,window,sha);
    expect(miss["used"]==false && miss["reason"]=="no_full_page_calibration","uncalibrated_is_miss");
    expect(miss["current_regions_eligible"]==false,"uncalibrated_cannot_request_current_region_continuation");
    expect(context.page(miss).isEmpty() && context.controls(miss).isEmpty(),"uncalibrated_exports_nothing");
    expect(context.remember(base,window,full),"complete_full_listing_calibrates");
    const auto proof=context.check(fresh,window,sha);
    expect(proof["used"]==true,"new_frame_identical_guards_hit");
    expect(proof["current_regions_eligible"]==true,"only_valid_current_calibrated_target_eligible");
    expect(proof["scope"]=="page_context_continuation_only" && proof["old_candidate_fields_reused"]==false
        && proof["actions_enabled"]==false,"proof_never_authorizes_action_or_reuses_candidate");
    expect(proof["frame_id"]==fresh.frameId && proof["frame_sha256"]==sha,"proof_binds_new_frame");
    expect(proof["guards"].toArray().size()==CollectionPageContext::guardRegions().size(),"all_guards_recorded");
    auto page=context.page(proof);auto controls=context.controls(proof);
    expect(page["page"]=="skin_listings" && page["context_continuation"]==true,"page_is_explicit_continuation");
    expect(page["token_count"]==0 && page["actions_enabled"]==false,"page_is_not_fake_full_ocr");
    expect(page["anchor_checks"].toObject()["collection.added"]==false
        && page["anchor_checks"].toObject()["collection.full"]==false,"old_added_and_capacity_flags_cleared");
    expect(controls["ocr_reused_from_frame_id"]==base.frameId && controls["current_frame_id"]==fresh.frameId
        && controls["current_frame_sha256"]==sha,"controls_reuse_provenance_is_not_new_ocr");

    const QList<QPair<QString,std::function<void(QJsonObject&)>>> badPackets{
        {"capture_failed",[](auto& p){p["capture_passed"]=false;}},
        {"ocr_failed",[](auto& p){p["ocr_passed"]=false;}},
        {"not_full_ocr",[](auto& p){p["full_frame_ocr_performed"]=false;field(p,"ocr","full_frame_ocr_performed",false);}},
        {"missing_full_ocr_claim",[](auto& p){p.remove("full_frame_ocr_performed");}},
        {"partial_coverage",[](auto& p){field(p,"ocr","coverage","calibrated_listing_fields");}},
        {"no_full_ocr_evidence",[](auto& p){p.remove("ocr");}},
        {"invalid_page_input",[](auto& p){field(p,"startup_page","valid_input",false);}},
        {"other_page",[](auto& p){field(p,"startup_page","page","watchlist_listings");}},
        {"overlay",[](auto& p){field(p,"startup_page","overlay","listing_filter");}},
        {"missing_sale",[](auto& p){auto page=p["startup_page"].toObject();field(page,"anchor_checks","listing.sale",false);p["startup_page"]=page;}},
        {"missing_sort",[](auto& p){auto page=p["startup_page"].toObject();field(page,"anchor_checks","listing.default_sort",false);p["startup_page"]=page;}},
        {"incomplete_layout",[](auto& p){field(p,"collection_layout","complete",false);}},
        {"layout_not_same_frame",[](auto& p){field(p,"collection_layout","same_frame",false);}},
        {"layout_wrong_frame",[](auto& p){field(p,"collection_layout","frame_id","synthetic:old");}},
        {"projection_not_same_frame",[](auto& p){field(p,"collection_observation","same_frame",false);}},
        {"projection_wrong_frame",[](auto& p){field(p,"collection_observation","frame_id","synthetic:old");}},
        {"projection_wrong_digest",[](auto& p){field(p,"collection_observation","frame_sha256",QString(64,'b'));}},
        {"invalid_digest",[](auto& p){field(p,"collection_observation","frame_sha256","bad");field(p,"collection_layout","frame_sha256","bad");}},
        {"continuation_cannot_recalibrate",[](auto& p){p["collection_page_context"]=QJsonObject{{"used",true}};}},
        {"no_controls",[](auto& p){field(p,"collection_observation","regions",QJsonArray{});}},
        {"failed_controls",[](auto& p){auto o=p["collection_observation"].toObject();auto a=o["regions"].toArray();auto c=a[0].toObject();c["ok"]=false;a[0]=c;o["regions"]=a;p["collection_observation"]=o;}},
        {"truncated_controls",[](auto& p){auto o=p["collection_observation"].toObject();auto a=o["regions"].toArray();auto c=a[0].toObject();c["truncated"]=true;a[0]=c;o["regions"]=a;p["collection_observation"]=o;}},
        {"empty_controls",[](auto& p){auto o=p["collection_observation"].toObject();auto a=o["regions"].toArray();auto c=a[0].toObject();c["words"]=QJsonArray{};a[0]=c;o["regions"]=a;p["collection_observation"]=o;}}
    };
    for(const auto& test:badPackets) {
        expect(context.remember(base,window,full),"reset_valid_before_bad_"+test.first);
        auto p=full;test.second(p);
        expect(!context.remember(base,window,p),"reject_"+test.first);
        const auto rejected=context.check(fresh,window,sha);
        expect(rejected["used"]==false,"rejection_clears_prior_"+test.first);
        expect(rejected["current_regions_eligible"]==false,"invalid_calibration_blocks_current_regions_"+test.first);
    }

    expect(context.remember(base,window,full),"restore_for_freshness_tests");
    auto old=fresh;old.frameId=base.frameId;
    auto rejected=context.check(old,window,sha);
    expect(rejected["used"]==false && rejected["current_regions_eligible"]==false,"same_frame_id_rejected");
    old=fresh;old.sourceMonoMs=base.sourceMonoMs;
    rejected=context.check(old,window,sha);
    expect(rejected["used"]==false && rejected["current_regions_eligible"]==false,"same_source_timestamp_rejected");
    old.sourceMonoMs=base.sourceMonoMs-1;
    rejected=context.check(old,window,sha);
    expect(rejected["used"]==false && rejected["current_regions_eligible"]==false,"older_timestamp_rejected");
    old.sourceMonoMs=base.sourceMonoMs+60000;
    expect(context.check(old,window,sha)["used"]==true,"sixty_second_boundary_is_defined");
    old.sourceMonoMs=base.sourceMonoMs+60001;
    rejected=context.check(old,window,sha);
    expect(rejected["used"]==false && rejected["current_regions_eligible"]==false,"older_than_sixty_seconds_rejected");
    for(const auto& bad:QStringList{"", "bad", QString(64,'G')}) {
        rejected=context.check(fresh,window,bad);
        expect(rejected["used"]==false && rejected["current_regions_eligible"]==false,"malformed_current_digest_rejected");
    }

    const QList<QPair<QString,std::function<void(TargetWindow&)>>> targetMutations{
        {"hwnd",[](auto& t){++t.hwnd;}},{"pid",[](auto& t){++t.pid;}},
        {"process_created",[](auto& t){++t.processCreated;}},
        {"window_class",[](auto& t){t.windowClass="OtherClass";}},
        {"executable",[](auto& t){t.executableName="other.exe";}},
        {"client_rect",[](auto& t){t.clientRect.translate(1,0);}},
        {"monitor_rect",[](auto& t){t.monitorRect.translate(1,0);}},
        {"monitor",[](auto& t){++t.monitor;}},{"dpi",[](auto& t){t.dpi=96;}},
        {"invalid",[](auto& t){t.hwnd=0;}}
    };
    for(const auto& mutation:targetMutations) {
        auto changed=window;mutation.second(changed);
        const auto rejected=context.check(fresh,changed,sha);
        expect(rejected["used"]==false && rejected["current_regions_eligible"]==false,"target_changed_"+mutation.first);
    }
    const QList<QPair<QString,std::function<void(FrameEnvelope&)>>> frameMutations{
        {"empty_id",[](auto& f){f.frameId.clear();}},{"negative_time",[](auto& f){f.sourceMonoMs=-1;}},
        {"width",[](auto& f){--f.width;}},{"height",[](auto& f){--f.height;}},
        {"dpi_x",[](auto& f){f.dpiX=96;}},{"dpi_y",[](auto& f){f.dpiY=96;}},
        {"pixel_format",[](auto& f){f.pixelFormat="RGBA8";}},{"stride",[](auto& f){--f.strideBytes;}},
        {"valid_bytes",[](auto& f){--f.validBytes;}},{"buffer_size",[](auto& f){f.pixels.chop(1);}}
    };
    for(const auto& mutation:frameMutations) {
        auto changed=fresh;mutation.second(changed);
        const auto rejected=context.check(changed,window,sha);
        expect(rejected["used"]==false && rejected["current_regions_eligible"]==false,"frame_changed_"+mutation.first);
        CollectionPageContext invalidContext;
        expect(!invalidContext.remember(changed,window,packet(changed)),"bad_frame_cannot_calibrate_"+mutation.first);
    }

    // Check first/centre/last pixels and every BGRA byte of every guard. This
    // catches off-by-one rows/columns and accidentally ignored alpha bytes.
    for(const auto& guard:CollectionPageContext::guardRegions()) {
        expect(QRect(0,0,base.width,base.height).contains(guard.second),"guard_inside_frame_"+guard.first);
        for(const auto& point:QList<QPoint>{guard.second.topLeft(),guard.second.center(),guard.second.bottomRight()}) {
            for(int channel=0;channel<4;++channel) {
                auto changed=fresh;
                const auto index=qint64(point.y())*changed.strideBytes+point.x()*4+channel;
                changed.pixels[index]=char(changed.pixels[index]^1);
                const auto observed=context.check(changed,window,QString(64,'b'));
                expect(observed["used"]==false && observed["reason"]=="guard_pixels_changed",
                    "single_guard_byte_miss_"+guard.first+QString::number(channel));
                expect(observed["current_regions_eligible"]==true && observed["actions_enabled"]==false,
                    "guard_miss_only_permits_new_regions_not_action_"+guard.first);
                expect(context.page(observed).isEmpty() && context.controls(observed).isEmpty(),"miss_exports_no_old_context");
            }
        }
    }
    for(const auto& item:QList<QPair<QString,QPoint>>{{"price",QPoint(950,550)},
            {"wear",QPoint(2250,675)},{"star",QPoint(2340,320)}}) {
        for(const auto& guard:CollectionPageContext::guardRegions())
            expect(!guard.second.contains(item.second),"candidate_field_not_page_guard_"+item.first);
        auto changed=fresh;const auto index=qint64(item.second.y())*changed.strideBytes+item.second.x()*4;
        changed.pixels[index]=char(changed.pixels[index]^1);
        const auto fieldProof=context.check(changed,window,digest(changed));
        expect(fieldProof["used"]==true,"candidate_pixel_change_does_not_reclassify_page_"+item.first);
        const auto serialized=QJsonDocument(QJsonObject{{"proof",fieldProof},{"page",context.page(fieldProof)},
            {"controls",context.controls(fieldProof)}}).toJson(QJsonDocument::Compact);
        expect(!serialized.contains("OLD_") && !serialized.contains("favorite_warm_fraction")
            && !serialized.contains("selected_detail") && !serialized.contains("card_fields"),
            "page_context_contains_no_old_candidate_"+item.first);
        expect(context.page(fieldProof)["token_count"]==0 && context.page(fieldProof)["actions_enabled"]==false,
            "candidate_change_does_not_create_ocr_or_action_"+item.first);
    }
    for(const auto& item:QList<QPair<QString,QJsonValue>>{{"used",false},
            {"calibration_frame_id","synthetic:other"},{"calibration_frame_sha256",QString(64,'b')},
            {"frame_id",""},{"frame_id",base.frameId},{"frame_sha256","bad"}}) {
        const auto issued=context.check(fresh,window,sha);
        expect(!context.page(issued).isEmpty(),"proof_tamper_test_starts_with_current_valid_proof");
        auto modified=issued;modified[item.first]=item.second;
        expect(context.page(modified).isEmpty() && context.controls(modified).isEmpty(),"invalid_proof_exports_nothing_"+item.first);
    }
    const auto issued=context.check(fresh,window,sha);
    auto another=fresh;another.frameId="synthetic:later";++another.sourceMonoMs;
    const auto later=context.check(another,window,sha);
    expect(context.page(issued).isEmpty() && !context.page(later).isEmpty(),"new_check_invalidates_prior_proof");
    auto invalid=another;invalid.dpiX=96;
    context.check(invalid,window,sha);
    expect(context.page(later).isEmpty(),"failed_check_invalidates_prior_proof");
    context.clear();
    expect(context.page(proof).isEmpty() && context.controls(proof).isEmpty(),"clear_invalidates_previously_issued_proof");
    rejected=context.check(fresh,window,sha);
    expect(rejected["used"]==false && rejected["current_regions_eligible"]==false,"clear_requires_new_full_calibration");
    std::cout<<"COLLECTION_PAGE_CONTEXT_TESTS="<<(failures?"FAIL":"PASS")<<"; assertions="<<checks
        <<"; failures="<<failures<<"; synthetic_input=true; game_input=false; image_file_writes=0\n";
    return failures?1:0;
}
