#include "application/vision/collection_listing_regions.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <functional>
#include <iostream>

using relink::runtime::observation::FrameEnvelope;
using relink::vision::classifyCollectionListingRegions;
using relink::vision::collectionListingRegionSpecs;
using relink::vision::projectCurrentCollectionRegion;
namespace {
FrameEnvelope frame() {
    FrameEnvelope f;f.frameId="synthetic:current";f.sourceMonoMs=1000;
    f.width=2560;f.height=1440;f.dpiX=f.dpiY=144;f.strideBytes=2560*4;
    f.validBytes=qint64(f.strideBytes)*f.height;f.pixels=QByteArray(f.validBytes,'\0');return f;
}
QJsonObject word(const QString& text,double x,double y,double width=20,double height=20) {
    return {{"text",text},{"x",x},{"y",y},{"width",width},{"height",height}};
}
QJsonArray split(const QString& text,int x,int y) {
    QJsonArray out;for(const auto c:text){out.append(word(QString(c),x,y));x+=21;}return out;
}
QJsonArray regions(const FrameEnvelope& f,const QString& sha) {
    QJsonArray out;
    for(const auto& spec:collectionListingRegionSpecs()) {
        QJsonArray tokens;
        if(spec.first=="sale_header")tokens=split(QStringLiteral("在售"),200,100);
        if(spec.first=="listing_controls")tokens=split(QStringLiteral("默认排序"),1153,250);
        if(spec.first=="detail_anchors") {
            tokens=split(QStringLiteral("成色"),1930,660);
            for(const auto& w:split(QStringLiteral("相似皮肤"),1930,575))tokens.append(w);
        }
        out.append(QJsonObject{{"kind",spec.first},{"ok",true},{"same_frame",true},{"truncated",false},
            {"frame_id",f.frameId},{"frame_sha256",sha},{"words",tokens},
            {"bounds",QJsonArray{spec.second.x(),spec.second.y(),spec.second.width(),spec.second.height()}}});
    }
    return out;
}
void change(QJsonArray& a,int index,const std::function<void(QJsonObject&)>& edit) {
    auto r=a[index].toObject();edit(r);a[index]=r;
}
void addLabel(QJsonArray& a,int index,const QString& text,int x,int y,bool splitWords=true) {
    change(a,index,[&](auto& r){auto words=r["words"].toArray();
        if(splitWords){for(const auto& w:split(text,x,y))words.append(w);}
        else words.append(word(text,x,y,text.size()*20));
        r["words"]=words;});
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);int count=0,failures=0;
    auto check=[&](bool pass,const QString& name){++count;if(!pass){++failures;std::cout<<"FAIL "<<name.toStdString()<<'\n';}};
    const auto f=frame();const auto sha=QString(64,'a');const auto valid=regions(f,sha);
    auto classify=[&](const QJsonArray& a){return classifyCollectionListingRegions(f,sha,a);};
    auto result=classify(valid);
    check(result["matched"]==true && result["valid_input"]==true,"split_chinese_current_anchors_match");
    check(result["coverage"]=="current_listing_regions" && result["full_frame_ocr_performed"]==false,"partial_not_full_ocr");
    check(result["requires_prior_full_calibration"]==true && result["actions_enabled"]==false
        && result["old_candidate_fields_reused"]==false,"no_history_replacement_or_action_authority");
    auto page=result["startup_page"].toObject();
    check(page["page"]=="skin_listings" && page["overlay"]=="none" && page["context_continuation"]==true,"listing_continuation_only");
    check(page["anchor_checks"].toObject()["collection.added"]==false
        && page["anchor_checks"].toObject()["collection.full"]==false,"no_old_toast_or_capacity");
    check(result["anchor_evidence"].toArray().size()==4 && result["regions"].toArray().size()==4,"all_current_bindings_recorded");
    for(const auto& item:result["regions"].toArray())
        check(item.toObject()["frame_id"]==f.frameId && item.toObject()["frame_sha256"]==sha,"same_frame_region_evidence");
    const auto specs=collectionListingRegionSpecs();
    check(specs.size()==4 && specs[0].second==QRect(170,64,300,96)
        && specs[1].second==QRect(110,230,1760,70) && specs[2].second==QRect(1905,560,540,190)
        && specs[3].second==QRect(700,370,1160,570),"exact_requested_rois");
    for(int index=0;index<4;++index) {
        for(const auto& key:QStringList{"kind","ok","same_frame","frame_id","frame_sha256","bounds","words"}) {
            auto a=valid;change(a,index,[&](auto& r){r.remove(key);});const auto failed=classify(a);
            check(failed["matched"]==false && failed["requires_full_frame_fallback"]==true,"missing_required_region_field_"+key);
        }
        for(const auto& mutation:QList<QPair<QString,QJsonValue>>{{"ok",false},{"same_frame",false},{"truncated",true},
                {"frame_id","synthetic:old"},{"frame_sha256",QString(64,'b')},{"kind","unknown"}}) {
            auto a=valid;change(a,index,[&](auto& r){r[mutation.first]=mutation.second;});
            check(classify(a)["matched"]==false,"wrong_region_binding_"+mutation.first);
        }
        auto a=valid;change(a,index,[&](auto& r){auto b=r["bounds"].toArray();b[0]=b[0].toDouble()+1;r["bounds"]=b;});
        check(classify(a)["matched"]==false,"one_pixel_roi_offset_rejected");
        a=valid;change(a,index,[&](auto& r){auto b=r["bounds"].toArray();b[2]=b[2].toDouble()+.5;r["bounds"]=b;});
        check(classify(a)["matched"]==false,"fractional_roi_extent_rejected");
    }
    auto a=valid;a.removeLast();check(classify(a)["matched"]==false,"missing_region_fails");
    a=valid;a.append(valid[0]);check(classify(a)["matched"]==false,"extra_region_fails");
    a=valid;a[3]=valid[0];check(classify(a)["matched"]==false,"duplicate_region_fails");
    for(int index=0;index<3;++index) {
        a=valid;change(a,index,[](auto& r){r["words"]=QJsonArray{};});
        check(classify(a)["matched"]==false,"missing_positive_anchor_fails");
    }
    // Empty modal OCR is allowed only as an explicit successful current ROI;
    // removing the ROI or marking its OCR failed is not an empty-scene proof.
    check(valid[3].toObject()["words"].toArray().isEmpty() && classify(valid)["matched"]==true,"successful_empty_modal_roi_allowed");
    a=valid;change(a,2,[](auto& r){r["words"]=split(QStringLiteral("成色"),1930,660);});
    check(classify(a)["matched"]==false,"similar_label_mandatory");
    a=valid;change(a,2,[](auto& r){r["words"]=split(QStringLiteral("相似皮肤"),1930,575);});
    check(classify(a)["matched"]==false,"condition_label_mandatory");
    a=valid;change(a,1,[](auto& r){r["words"]=split(QStringLiteral("按价格"),1153,250);});
    check(classify(a)["matched"]==false,"other_sort_not_silently_supported");
    for(bool splitWords:{false,true}) {
        for(const auto& text:QStringList{QStringLiteral("确认"),QStringLiteral("确定"),QStringLiteral("取消"),
                QStringLiteral("获得"),QStringLiteral("已下架"),QStringLiteral("已售出"),QStringLiteral("购买"),
                QStringLiteral("交易成功"),QStringLiteral("关注数量已达上限"),QStringLiteral("已达关注上限")}) {
            a=valid;addLabel(a,3,text,1000,450,splitWords);result=classify(a);
            check(result["matched"]==false && result["reason"]=="E_LISTING_REGIONS_BLOCKING_LABEL","modal_blocker_"+text);
        }
        for(const auto& text:QStringList{QStringLiteral("筛选"),QStringLiteral("价格区间"),QStringLiteral("所有成色"),
                QStringLiteral("确定"),QStringLiteral("确认"),QStringLiteral("取消"),QStringLiteral("已下架"),QStringLiteral("已售出")}) {
            a=valid;addLabel(a,2,text,2150,700,splitWords);result=classify(a);
            check(result["matched"]==false && result["reason"]=="E_LISTING_REGIONS_BLOCKING_LABEL","detail_blocker_"+text);
        }
    }
    a=valid;change(a,3,[](auto& r){r["words"]=QJsonArray{word(QStringLiteral("请确"),1000,450,40),word(QStringLiteral("认操作"),1041,450,60)};});
    check(classify(a)["reason"]=="E_LISTING_REGIONS_BLOCKING_LABEL","blocker_across_token_prefix_and_suffix");
    for(bool vertical:{false,true}) {
        a=valid;change(a,0,[&](auto& r){auto words=r["words"].toArray();auto last=words[1].toObject();
            last[vertical?"y":"x"]=vertical?140:350;words[1]=last;r["words"]=words;});
        check(classify(a)["matched"]==false,"distant_or_other_row_tokens_do_not_join");
    }
    for(const auto& mutation:QList<QPair<QString,QJsonValue>>{{"text",""},{"text",false},{"width",0},{"height",-1},
            {"x",0},{"y",0},{"score",.69},{"score",1.1},{"score","0.99"}}) {
        a=valid;change(a,0,[&](auto& r){auto words=r["words"].toArray();auto w=words[0].toObject();w[mutation.first]=mutation.second;words[0]=w;r["words"]=words;});
        check(classify(a)["matched"]==false,"invalid_word_falls_back_"+mutation.first);
    }
    a=valid;change(a,0,[](auto& r){auto words=r["words"].toArray();auto w=words[0].toObject();w["text"]=QString(129,'x');words[0]=w;r["words"]=words;});
    check(classify(a)["matched"]==false,"long_token_rejected");
    a=valid;change(a,3,[](auto& r){QJsonArray words;for(int i=0;i<257;++i)words.append(word("1",900,450));r["words"]=words;});
    check(classify(a)["matched"]==false,"region_token_limit");
    a=valid;change(a,0,[](auto& r){r["words"]=QJsonArray{word(QStringLiteral("在 售"),200,100,45)};});
    check(classify(a)["matched"]==true,"unicode_space_normalization_only");
    for(const auto& mutation:QList<QPair<QString,std::function<void(FrameEnvelope&)>>>{{"width",[](auto& f){--f.width;}},
            {"height",[](auto& f){--f.height;}},{"dpi_x",[](auto& f){f.dpiX=96;}},{"dpi_y",[](auto& f){f.dpiY=96;}},
            {"id",[](auto& f){f.frameId.clear();}},{"time",[](auto& f){f.sourceMonoMs=-1;}},
            {"stride",[](auto& f){--f.strideBytes;}},{"bytes",[](auto& f){--f.validBytes;}},
            {"pixel_format",[](auto& f){f.pixelFormat="RGBA8";}},{"pixels",[](auto& f){f.pixels.chop(1);}}}) {
        auto changed=f;mutation.second(changed);check(classifyCollectionListingRegions(changed,sha,valid)["matched"]==false,"frame_contract_"+mutation.first);
    }
    for(const auto& bad:QStringList{"","bad",QString(64,'G')})
        check(classifyCollectionListingRegions(f,bad,valid)["matched"]==false,"bad_digest_rejected");
    // The Chinese detail parent is recognized on this exact frame. Projection
    // preserves raw current words, not any title/price/star or prior result.
    const QRect detailBounds(1920,640,505,66);
    auto detail=valid[2].toObject();auto detailWords=detail["words"].toArray();
    detailWords.append(word("S(0.123456)",2208,660,150));detail["words"]=detailWords;
    detail["provider"]="Windows.Media.Ocr";
    auto project=[&](const QJsonObject& source){return projectCurrentCollectionRegion(source,f.frameId,sha,"selected_detail",detailBounds);};
    auto projected=project(detail);
    check(!projected.isEmpty() && projected["kind"]=="selected_detail" && projected["words"].toArray().size()==3,"current_detail_subset_projects");
    check(projected["frame_id"]==f.frameId && projected["frame_sha256"]==sha && projected["same_frame"]==true,"projection_keeps_current_frame_binding");
    check(projected["coverage"]=="same_frame_region_projection" && projected["historical_cache_used"]==false
        && projected["projection_ocr_performed"]==false && projected["old_candidate_fields_reused"]==false,"projection_is_not_old_cache_or_new_ocr");
    check(projected["words"].toArray().last()==detailWords.last(),"projection_preserves_numeric_word_verbatim");
    for(const auto& w:projected["words"].toArray())
        check(w.toObject()["y"].toDouble()>=640,"nonselected_similar_label_excluded");
    for(const auto& mutation:QList<QPair<QString,QJsonValue>>{{"ok",false},{"same_frame",false},{"truncated",true},
            {"frame_id","synthetic:old"},{"frame_sha256",QString(64,'b')},{"kind","selected_detail"},
            {"words",QJsonArray{}},{"words",false}}) {
        auto source=detail;source[mutation.first]=mutation.second;
        check(project(source).isEmpty(),"projection_bad_parent_"+mutation.first);
    }
    for(const auto& key:QStringList{"ok","same_frame","frame_id","frame_sha256","kind","bounds","words"}) {
        auto source=detail;source.remove(key);check(project(source).isEmpty(),"projection_missing_parent_field_"+key);
    }
    auto source=detail;source["bounds"]=QJsonArray{1906,560,540,190};
    check(project(source).isEmpty(),"projection_shifted_parent_rejected");
    source=detail;source["words"]=split(QStringLiteral("相似皮肤"),1930,575);
    check(project(source).isEmpty(),"empty_selected_projection_falls_back");
    for(const auto& crossing:QList<QJsonObject>{word("L",1910,660,20),word("R",2420,660,10),
            word("T",1950,630,20,20),word("B",1950,700,20,20)}) {
        source=detail;auto ws=source["words"].toArray();ws.append(crossing);source["words"]=ws;
        check(project(source).isEmpty(),"crossing_each_projection_edge_falls_back");
    }
    for(const auto& mutation:QList<QPair<QString,QJsonValue>>{{"x",0},{"y",0},{"width",0},{"height",-1},
            {"text",""},{"score",.69},{"score",2},{"score","1"}}) {
        source=detail;auto ws=source["words"].toArray();auto w=ws[0].toObject();w[mutation.first]=mutation.second;ws[0]=w;source["words"]=ws;
        check(project(source).isEmpty(),"projection_invalid_token_"+mutation.first);
    }
    for(const auto& box:QList<QRect>{QRect(0,0,10,10),QRect(1920,640,0,66),QRect(1920,640,505,0),QRect(1920,640,900,66)})
        check(projectCurrentCollectionRegion(detail,f.frameId,sha,"selected_detail",box).isEmpty(),"invalid_projection_bounds");
    check(projectCurrentCollectionRegion(detail,"synthetic:other",sha,"selected_detail",detailBounds).isEmpty(),"projection_wrong_requested_frame");
    check(projectCurrentCollectionRegion(detail,f.frameId,QString(64,'b'),"selected_detail",detailBounds).isEmpty(),"projection_wrong_requested_digest");
    check(projectCurrentCollectionRegion(detail,f.frameId,"bad","selected_detail",detailBounds).isEmpty(),"projection_malformed_digest");
    check(projectCurrentCollectionRegion(detail,f.frameId,sha,"selected_detail_precise",detailBounds).isEmpty(),"chinese_projection_does_not_replace_english_precise_ocr");
    check(projectCurrentCollectionRegion(detail,f.frameId,sha,"",detailBounds).isEmpty(),"projection_kind_required");
    std::cout<<"COLLECTION_LISTING_REGIONS_TESTS="<<(failures?"FAIL":"PASS")<<"; assertions="<<count
        <<"; failures="<<failures<<"; synthetic_input=true; game_input=false; image_file_writes=0\n";
    return failures?1:0;
}
