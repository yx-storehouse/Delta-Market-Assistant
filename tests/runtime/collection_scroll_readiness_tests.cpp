#include "diagnostics/collection_scroll_readiness.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <iostream>
using namespace relink::diagnostics;
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);int assertions=0,failures=0;
    const auto check=[&](bool ok,const char* name){++assertions;if(!ok)++failures;std::cout<<(ok?"PASS ":"FAIL ")<<name<<'\n';};
    QFile file(argc>1?QString::fromLocal8Bit(argv[1]):QString());
    if(!file.open(QIODevice::ReadOnly)){std::cerr<<"fixture missing\n";return 1;}
    const auto fixture=QJsonDocument::fromJson(file.readAll()).object();
    auto before=fixture["before_layout"].toObject(),after=fixture["after_layout"].toObject();
    const auto synthetic=[&](QJsonObject layout,const QString& id){layout["frame_id"]="synthetic:"+id;layout["frame_sha256"]=QString(64,'a');return layout;};
    CollectionScrollReadiness recorded;
    auto first=recorded.observe(before,1000);auto shifted=recorded.observe(after,1600);
    check(first["ready_for_full_ocr"]==false,"first_complete_scroll_frame_is_not_stability_proof");
    check(shifted["ready_for_full_ocr"]==false&&shifted["stable_frame_count"]==1,"recorded_six_pixel_post_scroll_motion_resets_stability");
    auto second=recorded.observe(synthetic(after,"second"),1660);
    check(second["ready_for_full_ocr"]==false,"two_synthetic_equal_frames_not_enough");
    auto third=recorded.observe(synthetic(after,"third"),1720);
    check(third["ready_for_full_ocr"]==true&&third["stable_source_span_ms"]==120,"three_equal_current_geometries_over_window_allow_ocr_only");
    check(third["actions_enabled"]==false&&third["identity_proven"]==false&&third["requires_original_full_validation"]==true,"ready_gate_never_authorizes_collection");
    check(recorded.observe(synthetic(after,"third"),1750)["ready_for_full_ocr"]==false,"same_acquisition_not_recounted");
    CollectionScrollReadiness fast;
    fast.observe(synthetic(after,"f0"),1000);fast.observe(synthetic(after,"f1"),1001);
    check(fast.observe(synthetic(after,"f2"),1002)["ready_for_full_ocr"]==false,"three_frames_without_time_span_are_not_stable");
    const auto move=[&](QJsonObject l,int y){auto cs=l["cards"].toArray();for(int i=0;i<cs.size();++i){auto c=cs[i].toObject();
        for(const auto* name:{"bounds","fields_bounds"})if(c[name].isArray()){auto b=c[name].toArray();b[1]=b[1].toInt()+y;c[name]=b;}
        cs[i]=c;}l["cards"]=cs;return l;};
    CollectionScrollReadiness drift;
    drift.observe(synthetic(before,"d0"),1000);drift.observe(synthetic(move(before,1),"d1"),1060);
    auto dr=drift.observe(synthetic(move(before,2),"d2"),1120);
    check(dr["ready_for_full_ocr"]==false&&dr["stable_frame_count"]==1,"cumulative_one_pixel_per_frame_drift_does_not_slide_anchor");
    for(int mode=0;mode<7;++mode){CollectionScrollReadiness gate;gate.observe(synthetic(before,"b"),1000);auto x=synthetic(before,"bad");
        if(mode==0)x["complete"]=false;
        if(mode==1)x["same_frame"]=false;
        if(mode==2)x["frame_sha256"]="not-a-hash";
        if(mode==3)x["cards"]=QJsonArray{};
        if(mode==4){auto bar=x["scrollbar"].toObject();bar["thumb_bounds"]=QJsonArray{0,0,0,0};x["scrollbar"]=bar;}
        if(mode==5){auto cards=x["cards"].toArray();auto c=cards[0].toObject();c["bounds"]=QJsonArray{1,2,3.1,4};cards[0]=c;x["cards"]=cards;}
        if(mode==6)x["listing_viewport"]=QJsonArray{1,2,3};
        check(gate.observe(x,1200)["ready_for_full_ocr"]==false,"invalid_complete_or_geometry_evidence_resets_gate");}
    for(int mode=0;mode<4;++mode){CollectionScrollReadiness gate;gate.observe(synthetic(before,"a"),1000);auto x=synthetic(before,"changed");
        if(mode==0){auto cs=x["cards"].toArray();cs.removeLast();x["cards"]=cs;}
        if(mode==1){auto cs=x["cards"].toArray();auto c=cs[0].toObject();c["selected"]=!c["selected"].toBool();cs[0]=c;x["cards"]=cs;}
        if(mode==2){auto bar=x["scrollbar"].toObject();auto b=bar["thumb_bounds"].toArray();b[1]=b[1].toInt()+2;bar["thumb_bounds"]=b;x["scrollbar"]=bar;}
        if(mode==3){auto vp=x["listing_viewport"].toArray();vp[1]=vp[1].toInt()+1;x["listing_viewport"]=vp;}
        check(gate.observe(x,1200)["stable_frame_count"]==1,"membership_selection_scrollbar_or_viewport_change_resets_anchor");}
    std::cout<<"SCROLL_READINESS_TESTS="<<(failures?"FAIL":"PASS")<<"; assertions="<<assertions<<"; failures="<<failures<<"; game_input=false\n";
    return failures?1:0;
}
