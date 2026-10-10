#include "collection_listing_regions.h"
#include <QHash>
#include <QRectF>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <functional>

namespace relink::vision {
namespace {
struct Token { QString text; QRectF box; };
struct Match { bool found=false; bool exhausted=false; QRectF box; int count=0; };
bool finiteNumber(const QJsonValue& v) { return v.isDouble() && std::isfinite(v.toDouble()); }
QString normalized(QString text) {
    text=text.normalized(QString::NormalizationForm_KC);
    text.remove(QRegularExpression(QStringLiteral("\\s+")));
    return text;
}
QJsonArray rect(const QRectF& r) { return {r.x(),r.y(),r.width(),r.height()}; }
bool sameBounds(const QJsonValue& value,const QRect& expected) {
    if(!value.isArray())return false;
    const auto a=value.toArray();if(a.size()!=4)return false;
    const int wanted[]{expected.x(),expected.y(),expected.width(),expected.height()};
    for(int i=0;i<4;++i)if(!finiteNumber(a[i]) || a[i].toDouble()!=wanted[i])return false;
    return true;
}
bool adjacent(const Token& left,const Token& right) {
    if(right.box.left()<=left.box.left())return false;
    const double font=std::min(left.box.height(),right.box.height());
    const double gap=right.box.left()-left.box.right();
    return gap>=-.35*font && gap<=.60*font
        && std::abs(left.box.center().y()-right.box.center().y())<=.5*font;
}
Match findLabel(const QList<Token>& tokens,const QString& label,bool substring) {
    Match match;int budget=8192;
    for(const auto& token:tokens) {
        if((substring && token.text.contains(label)) || token.text==label)
            return {true,false,token.box,1};
    }
    std::function<bool(int,int,QRectF,int)> extend;
    extend=[&](int last,int used,QRectF box,int depth) {
        if(used==label.size()){match={true,false,box,depth};return true;}
        if(depth>=16)return false;
        for(int i=0;i<tokens.size();++i) {
            if(--budget<0){match.exhausted=true;return false;}
            if(!adjacent(tokens[last],tokens[i]))continue;
            const auto remaining=label.mid(used);
            const auto joined=box.united(tokens[i].box);
            if(substring && tokens[i].text.startsWith(remaining)) {
                match={true,false,joined,depth+1};return true;
            }
            if(!remaining.startsWith(tokens[i].text))continue;
            if(extend(i,used+tokens[i].text.size(),joined,depth+1))return true;
            if(match.exhausted)return false;
        }
        return false;
    };
    for(int i=0;i<tokens.size();++i) {
        if(!substring) {
            if(!label.startsWith(tokens[i].text))continue;
            if(extend(i,tokens[i].text.size(),tokens[i].box,1))return match;
        } else {
            for(int suffix=std::min(tokens[i].text.size(),label.size()-1);suffix>0;--suffix) {
                if(tokens[i].text.right(suffix)!=label.left(suffix))continue;
                if(extend(i,suffix,tokens[i].box,1))return match;
                if(match.exhausted)return match;
            }
        }
        if(match.exhausted)return match;
    }
    return match;
}
}

QList<QPair<QString,QRect>> collectionListingRegionSpecs() {
    return {{"sale_header",QRect(170,64,300,96)},
        {"listing_controls",QRect(110,230,1760,70)},
        {"detail_anchors",QRect(1905,560,540,190)},
        {"modal_guard",QRect(700,370,1160,570)}};
}

QJsonObject projectCurrentCollectionRegion(const QJsonObject& sourceRegion,
        const QString& frameId,const QString& frameSha256,const QString& desiredKind,const QRect& bounds) {
    // This reuse is intentionally only the Chinese detail -> selected-detail
    // projection. The separately configured English precise numeric ROI is
    // not replaced by Chinese parent text.
    const QRect parent(1905,560,540,190);
    if(frameId.isEmpty() || !QRegularExpression("^[0-9a-f]{64}$").match(frameSha256).hasMatch()
        || desiredKind!="selected_detail" || sourceRegion["kind"]!="detail_anchors"
        || sourceRegion["ok"]!=true || sourceRegion["same_frame"]!=true || sourceRegion["truncated"]==true
        || sourceRegion["frame_id"]!=frameId || sourceRegion["frame_sha256"]!=frameSha256
        || !sameBounds(sourceRegion["bounds"],parent)
        || bounds.width()<=0 || bounds.height()<=0 || !parent.contains(bounds)
        || !sourceRegion["words"].isArray() || sourceRegion["words"].toArray().size()>256)return {};
    QJsonArray projected;int totalBytes=0;
    for(const auto& value:sourceRegion["words"].toArray()) {
        if(!value.isObject())return {};
        const auto word=value.toObject();
        if(!word["text"].isString() || normalized(word["text"].toString()).isEmpty()
            || word["text"].toString().size()>128)return {};
        totalBytes+=word["text"].toString().toUtf8().size();if(totalBytes>65536)return {};
        for(const auto& key:{"x","y","width","height"})if(!finiteNumber(word[key]))return {};
        const QRectF box(word["x"].toDouble(),word["y"].toDouble(),word["width"].toDouble(),word["height"].toDouble());
        if(box.width()<=0 || box.height()<=0 || !QRectF(parent).contains(box))return {};
        if(word.contains("score") && (!finiteNumber(word["score"])
            || word["score"].toDouble()<.70 || word["score"].toDouble()>1))return {};
        if(QRectF(bounds).contains(box))projected.append(word);
        else if(QRectF(bounds).intersects(box))return {};
    }
    if(projected.isEmpty())return {};
    return {{"kind",desiredKind},{"ok",true},{"same_frame",true},{"truncated",false},
        {"frame_id",frameId},{"frame_sha256",frameSha256},
        {"bounds",QJsonArray{bounds.x(),bounds.y(),bounds.width(),bounds.height()}},{"words",projected},
        {"coverage","same_frame_region_projection"},{"source_region_kind","detail_anchors"},
        {"source_region_bounds",sourceRegion["bounds"]},{"source_provider",sourceRegion["provider"]},
        {"evidence_basis","current_parent_ocr_words_projected_from_same_frame"},
        {"old_candidate_fields_reused",false},{"historical_cache_used",false},
        {"projection_ocr_performed",false},{"actions_enabled",false}};
}

QJsonObject classifyCollectionListingRegions(const runtime::observation::FrameEnvelope& frame,
        const QString& frameSha256,const QJsonArray& regions) {
    QJsonObject checks{{"listing.sale",false},{"listing.default_sort",false},
        {"listing.condition",false},{"listing.similar",false},{"listing.page_number",false},
        {"collection.added",false},{"collection.full",false}};
    QJsonObject page{{"valid_input",false},{"page","unknown"},{"overlay","none"},
        {"anchor_checks",checks},{"actions_enabled",false},{"context_continuation",true},
        {"coverage","current_listing_regions"},{"full_frame_ocr_performed",false},
        {"calibration","current_same_frame_listing_regions_after_prior_full_calibration"},
        {"token_count",0},{"low_score_tokens",0},{"live_calibrated",false}};
    QJsonObject out{{"schema","collection-listing-regions-v1"},{"matched",false},{"valid_input",false},
        {"coverage","current_listing_regions"},{"full_frame_ocr_performed",false},
        {"requires_prior_full_calibration",true},{"requires_full_frame_fallback",true},
        {"actions_enabled",false},{"old_candidate_fields_reused",false},
        {"frame_id",frame.frameId},{"frame_sha256",frameSha256},{"same_frame",true}};
    auto fail=[&](const QString& reason) {out["reason"]=reason;page["reason"]=reason;page["anchor_checks"]=checks;out["startup_page"]=page;return out;};
    if(frame.frameId.isEmpty() || frame.sourceMonoMs<0 || frame.width!=2560 || frame.height!=1440
        || frame.dpiX!=144 || frame.dpiY!=144 || frame.pixelFormat!="BGRA8"
        || frame.strideBytes!=2560*4 || frame.validBytes!=qint64(frame.strideBytes)*frame.height
        || frame.pixels.size()!=frame.validBytes)return fail("E_LISTING_REGIONS_FRAME");
    if(!QRegularExpression("^[0-9a-f]{64}$").match(frameSha256).hasMatch())return fail("E_LISTING_REGIONS_DIGEST");
    const auto specs=collectionListingRegionSpecs();
    if(regions.size()!=specs.size())return fail("E_LISTING_REGIONS_COUNT");
    QHash<QString,QRect> bounds;for(const auto& spec:specs)bounds.insert(spec.first,spec.second);
    QHash<QString,QList<Token>> words;QJsonArray bindings;int total=0,totalBytes=0;bool scores=false;
    for(const auto& value:regions) {
        if(!value.isObject())return fail("E_LISTING_REGIONS_REGION");
        const auto region=value.toObject();const auto name=region["kind"].toString();
        if(!bounds.contains(name) || words.contains(name))return fail("E_LISTING_REGIONS_KIND");
        if(region["ok"]!=true || region["same_frame"]!=true || region["truncated"]==true
            || region["frame_id"]!=frame.frameId || region["frame_sha256"]!=frameSha256
            || !sameBounds(region["bounds"],bounds[name]))return fail("E_LISTING_REGIONS_BINDING");
        if(!region["words"].isArray() || region["words"].toArray().size()>256)return fail("E_LISTING_REGIONS_WORDS");
        QList<Token> parsed;
        for(const auto& v:region["words"].toArray()) {
            if(!v.isObject())return fail("E_LISTING_REGIONS_TOKEN");
            const auto w=v.toObject();
            if(!w["text"].isString() || w["text"].toString().isEmpty() || w["text"].toString().size()>128)
                return fail("E_LISTING_REGIONS_TEXT");
            totalBytes+=w["text"].toString().toUtf8().size();
            if(totalBytes>65536)return fail("E_LISTING_REGIONS_TEXT_LIMIT");
            for(const auto& key:{"x","y","width","height"})if(!finiteNumber(w[key]))return fail("E_LISTING_REGIONS_WORD_BOUNDS");
            const QRectF box(w["x"].toDouble(),w["y"].toDouble(),w["width"].toDouble(),w["height"].toDouble());
            if(box.width()<=0 || box.height()<=0 || !QRectF(bounds[name]).contains(box))return fail("E_LISTING_REGIONS_WORD_BOUNDS");
            if(w.contains("score")) {
                scores=true;
                if(!finiteNumber(w["score"]) || w["score"].toDouble()<0 || w["score"].toDouble()>1)
                    return fail("E_LISTING_REGIONS_SCORE");
                if(w["score"].toDouble()<.70)return fail("E_LISTING_REGIONS_LOW_SCORE");
            }
            const auto text=normalized(w["text"].toString());
            if(text.isEmpty())return fail("E_LISTING_REGIONS_TEXT");
            parsed.append({text,box});++total;
        }
        words.insert(name,parsed);
        bindings.append(QJsonObject{{"kind",name},{"bounds",region["bounds"]},
            {"frame_id",frame.frameId},{"frame_sha256",frameSha256},{"same_frame",true},{"word_count",parsed.size()}});
    }
    page["token_count"]=total;page["provider_scores_available"]=scores;page["valid_input"]=true;
    out["valid_input"]=true;out["regions"]=bindings;
    const QStringList modalBlockers{QStringLiteral("确认"),QStringLiteral("确定"),QStringLiteral("取消"),
        QStringLiteral("获得"),QStringLiteral("已下架"),QStringLiteral("已售出"),QStringLiteral("购买"),
        QStringLiteral("交易成功"),QStringLiteral("关注数量已达上限"),QStringLiteral("已达关注上限")};
    const QStringList detailBlockers{QStringLiteral("筛选"),QStringLiteral("价格区间"),QStringLiteral("所有成色"),
        QStringLiteral("确定"),QStringLiteral("确认"),QStringLiteral("取消"),QStringLiteral("已下架"),QStringLiteral("已售出")};
    for(const auto& source:QStringList{"modal_guard","detail_anchors"}) {
        for(const auto& label:source=="modal_guard"?modalBlockers:detailBlockers) {
            const auto match=findLabel(words[source],label,true);
            if(match.exhausted)return fail("E_LISTING_REGIONS_LABEL_BUDGET");
            if(match.found) {
                page["overlay"]=source=="modal_guard"?"result_dialog":"listing_filter";
                out["blocker"]=QJsonObject{{"region",source},{"label",label},{"bounds",rect(match.box)},{"word_count",match.count}};
                return fail("E_LISTING_REGIONS_BLOCKING_LABEL");
            }
        }
    }
    struct Required { const char* key;const char* region;QString label; };
    const Required required[]={{"listing.sale","sale_header",QStringLiteral("在售")},
        {"listing.default_sort","listing_controls",QStringLiteral("默认排序")},
        {"listing.condition","detail_anchors",QStringLiteral("成色")},
        {"listing.similar","detail_anchors",QStringLiteral("相似皮肤")}};
    QJsonArray matches;QStringList missing;
    for(const auto& item:required) {
        const auto match=findLabel(words[item.region],item.label,false);
        if(match.exhausted)return fail("E_LISTING_REGIONS_LABEL_BUDGET");
        checks[item.key]=match.found;
        if(!match.found)missing.append(item.key);
        else matches.append(QJsonObject{{"anchor",item.key},{"region",item.region},{"label",item.label},
            {"bounds",rect(match.box)},{"word_count",match.count}});
    }
    out["anchor_evidence"]=matches;
    if(!missing.isEmpty()){out["missing_anchors"]=QJsonArray::fromStringList(missing);return fail("E_LISTING_REGIONS_REQUIRED_ANCHOR");}
    page["page"]="skin_listings";page["overlay"]="none";page["anchor_checks"]=checks;
    page["reason"]="CURRENT_LISTING_REGIONS_MATCH";
    page["anchors"]=QJsonArray{"list.sale","list.sort","list.condition","list.similar"};
    page["candidate_pages"]=QJsonArray{"skin_listings"};
    out["matched"]=true;out["requires_full_frame_fallback"]=false;out["reason"]="CURRENT_LISTING_REGIONS_MATCH";
    out["startup_page"]=page;
    return out;
}
}
