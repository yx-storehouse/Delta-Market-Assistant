#pragma once

#include "skin_page_classifier.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QRectF>
#include <QRegularExpression>

namespace relink::vision {
// Text evidence from ONE already acquired full-client frame. Regions are
// observation windows, not click targets or a calibrated purchase adapter.
inline QJsonObject projectPurchaseObservation(const QJsonObject& observation,
        const QJsonObject& page) {
    QJsonObject out{{"schema","purchase-observation-v1"},{"same_frame",true},
        {"frame_id",observation["frame_id"]},{"frame_sha256",observation["frame_sha256"]},
        {"viewport",QJsonArray{observation["width"],observation["height"]}},
        {"page",page["page"]},{"overlay",page["overlay"]},{"valid",false},
        {"actions_enabled",false},{"purchase_authorized",false},{"image_file_writes",0},
        {"geometry_role","text_observation_only_not_click_targets"},
        {"live_calibrated",false},{"regions",QJsonArray{}}};
    auto fail=[&](const char* code){out["error"]=code;return out;};
    if(observation["coverage"]!="full_client" ||!classifySkinPage(observation).validInput)
        return fail("E_PURCHASE_FULL_OCR_REQUIRED");
    if(observation["frame_id"].toString().isEmpty()
        ||!QRegularExpression("^[0-9a-f]{64}$").match(observation["frame_sha256"].toString()).hasMatch())
        return fail("E_PURCHASE_FRAME_BINDING");
    if(observation["width"]!=2560 ||observation["height"]!=1440)
        return fail("E_PURCHASE_VIEWPORT_UNCALIBRATED");
    const QList<QPair<QString,QRect>> specs{
        {"watch_header",QRect(110,64,1760,134)},
        {"listing_text",QRect(110,303,1760,930)},
        {"selected_detail",QRect(1900,232,550,890)},
        {"purchase_footer",QRect(1900,1130,550,175)},
        {"dialog_text",QRect(360,210,1500,1030)},
        // The short result toast centred near the top ("订单尚未开放购买",
        // "抢购队列已满，无法购买"; user screenshots 2026-10-09): its text sits
        // around y 205-245, across the edge of dialog_text. Same band as the
        // classifier's 成功添加至我的关注 receipt toast (.35,.12,.30,.08), but
        // ending above the listing's sort dropdown ("按稀有度升序" at y ~249-275,
        // live toast01).
        {"toast",QRect(896,150,768,102)}};
    QJsonArray regions;
    for(const auto& spec:specs){
        QJsonArray words;
        for(const auto& value:observation["words"].toArray()){
            const auto w=value.toObject();
            const QRectF bounds(w["x"].toDouble(),w["y"].toDouble(),w["width"].toDouble(),w["height"].toDouble());
            if(QRectF(spec.second).contains(bounds))words.append(value);
        }
        regions.append(QJsonObject{{"kind",spec.first},{"bounds",QJsonArray{spec.second.x(),spec.second.y(),spec.second.width(),spec.second.height()}},
            {"words",words},{"ok",true},{"truncated",false},{"same_frame",true},
            {"frame_id",out["frame_id"]},{"frame_sha256",out["frame_sha256"]},
            {"source","current_full_client_ocr"}});
    }
    out["regions"]=regions;out["valid"]=true;
    return out;
}
}
