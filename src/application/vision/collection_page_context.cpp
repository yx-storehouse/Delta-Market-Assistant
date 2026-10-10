#include "collection_page_context.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QRegularExpression>

namespace relink::vision {
namespace {
using runtime::observation::FrameEnvelope;
bool supported(const FrameEnvelope& f) {
    return !f.frameId.isEmpty() && f.sourceMonoMs >= 0 && f.pixelFormat == "BGRA8"
        && f.width == 2560 && f.height == 1440 && f.dpiX == 144 && f.dpiY == 144
        && f.strideBytes == 2560 * 4 && f.validBytes == qint64(f.strideBytes) * f.height
        && f.pixels.size() == f.validBytes;
}
QByteArray regionHash(const FrameEnvelope& f, const QRect& box) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = box.top(); y <= box.bottom(); ++y)
        hash.addData(QByteArrayView(f.pixels.constData() + qint64(y)*f.strideBytes + box.x()*4, box.width()*4));
    return hash.result();
}
bool sameTarget(const TargetWindow& a, const TargetWindow& b) {
    return a.valid() && b.valid() && a.hwnd==b.hwnd && a.pid==b.pid
        && a.processCreated==b.processCreated && a.windowClass==b.windowClass
        && a.executableName==b.executableName && a.clientRect==b.clientRect
        && a.monitorRect==b.monitorRect && a.monitor==b.monitor && a.dpi==b.dpi;
}
}

QList<QPair<QString,QRect>> CollectionPageContext::guardRegions() {
    // Exact raw-pixel comparison, not perceptual similarity or a new brightness
    // threshold. Any animation/hover/overlay touching these bands falls back.
    // The centre seam and right strip complement the header/control/title
    // guards: a centre dialog or right filter must not be hidden by top-only
    // evidence. These guards do not prove selected-card identity.
    return {{"sale_header",QRect(170,64,300,96)},
            {"listing_controls",QRect(110,230,1760,70)},
            {"product_title",QRect(1915,238,530,54)},
            {"centre_overlay_seam",QRect(988,340,7,850)},
            {"right_overlay_strip",QRect(2425,370,20,650)}};
}
void CollectionPageContext::clear() {
    m_valid=false; m_hashes.clear(); m_page={}; m_controls={}; m_issuedProof={};
    m_frameId.clear(); m_frameSha.clear(); m_sourceMs=-1;
}
bool CollectionPageContext::remember(const FrameEnvelope& frame,const TargetWindow& target,const QJsonObject& packet) {
    clear();
    const auto page=packet["startup_page"].toObject();
    const auto layout=packet["collection_layout"].toObject();
    const auto observation=packet["collection_observation"].toObject();
    const auto checks=page["anchor_checks"].toObject();
    if(!supported(frame) || !target.valid() || target.dpi!=144
       || packet["capture_passed"]!=true || packet["ocr_passed"]!=true
       || packet["full_frame_ocr_performed"]!=true || packet["ocr"].toObject()["coverage"]!="full_client"
       || (packet.contains("collection_page_context") && packet["collection_page_context"].toObject()["used"]==true)
       || page["valid_input"]!=true || page["page"]!="skin_listings" || page["overlay"]!="none"
       || checks["listing.sale"]!=true || checks["listing.default_sort"]!=true
       || layout["complete"]!=true || layout["same_frame"]!=true || layout["frame_id"]!=frame.frameId
       || observation["same_frame"]!=true || observation["frame_id"]!=frame.frameId
       || observation["frame_sha256"]!=layout["frame_sha256"]
       || !QRegularExpression("^[0-9a-f]{64}$").match(layout["frame_sha256"].toString()).hasMatch()) return false;
    for(const auto& r:observation["regions"].toArray()) {
        const auto region=r.toObject();
        if(region["kind"]=="listing_controls" && region["ok"]==true && region["truncated"]==false
           && !region["words"].toArray().isEmpty()) m_controls=region;
    }
    if(m_controls.isEmpty())return false;
    for(const auto& guard:guardRegions())m_hashes.append(regionHash(frame,guard.second));
    m_frameId=frame.frameId; m_frameSha=layout["frame_sha256"].toString();
    m_sourceMs=frame.sourceMonoMs; m_target=target; m_page=page; m_valid=true;
    return true;
}
QJsonObject CollectionPageContext::check(const FrameEnvelope& frame,const TargetWindow& target,const QString& digest) const {
    m_issuedProof={};
    QJsonObject out{{"schema","collection-page-context-v1"},{"used",false},
        {"scope","page_context_continuation_only"},{"frame_id",frame.frameId},{"frame_sha256",digest},
        {"calibration_frame_id",m_frameId},{"calibration_frame_sha256",m_frameSha},
        {"same_frame_guards",true},{"old_candidate_fields_reused",false},{"actions_enabled",false},
        {"current_regions_eligible",false}};
    auto fail=[&](const char* reason){out["reason"]=reason;return out;};
    if(!m_valid)return fail("no_full_page_calibration");
    if(!QRegularExpression("^[0-9a-f]{64}$").match(digest).hasMatch())return fail("invalid_frame_digest");
    if(!supported(frame) || !sameTarget(m_target,target))return fail("target_or_frame_changed");
    if(frame.frameId==m_frameId || frame.sourceMonoMs<=m_sourceMs)return fail("new_frame_required");
    if(frame.sourceMonoMs-m_sourceMs>60000)return fail("calibration_expired");
    // Current ROI recognition may replace the exact-pixel comparison only
    // AFTER all server-owned calibration/target/freshness checks pass. This
    // flag alone is not a page classification or an action authorization.
    out["current_regions_eligible"]=true;
    const auto guards=guardRegions();QJsonArray evidence;bool equal=true;
    for(int i=0;i<guards.size();++i) {
        const auto hash=regionHash(frame,guards[i].second);const bool same=hash==m_hashes[i];
        const auto box=guards[i].second;equal=equal &&same;
        evidence.append(QJsonObject{{"name",guards[i].first},
            {"bounds",QJsonArray{box.x(),box.y(),box.width(),box.height()}},
            {"sha256",QString::fromLatin1(hash.toHex())},{"exact_pixels_equal",same}});
    }
    out["guards"]=evidence;
    if(!equal)return fail("guard_pixels_changed");
    out["used"]=true;out["reason"]="current_guard_pixels_equal_to_full_calibration";
    m_issuedProof=out;
    return out;
}
QJsonObject CollectionPageContext::page(const QJsonObject& proof) const {
    if(!m_valid || proof["used"]!=true || proof!=m_issuedProof)return {};
    auto page=m_page; auto checks=page["anchor_checks"].toObject();
    // Never carry a previous toast or full-watchlist notification forward.
    checks["collection.added"]=false; checks["collection.full"]=false;
    page["anchor_checks"]=checks;page["calibration"]="same_page_exact_current_pixel_guards";
    page["reason"]="CALIBRATED_LISTING_CONTEXT_CONTINUED";page["context_continuation"]=true;
    page["token_count"]=0;page["low_score_tokens"]=0;page["actions_enabled"]=false;
    return page;
}
QJsonObject CollectionPageContext::controls(const QJsonObject& proof) const {
    if(page(proof).isEmpty())return {};
    auto controls=m_controls;
    controls["evidence_basis"]="exact_current_control_pixels_equal_to_calibration";
    controls["ocr_reused_from_frame_id"]=m_frameId;
    controls["current_frame_id"]=proof["frame_id"];
    controls["current_frame_sha256"]=proof["frame_sha256"];
    return controls;
}
}
