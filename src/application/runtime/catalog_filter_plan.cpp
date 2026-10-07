#include "catalog_filter_plan.h"
namespace relink::runtime {
CatalogFilterPlan planCatalogFilter(const vision::CatalogFilterState& o,const CatalogFilterTarget& t) {
    CatalogFilterPlan p;p.snapshotId=t.snapshotId;p.sourceFrameId=o.frameId;
    const QStringList grades{"legendary","epic","rare","common"};
    if(t.snapshotId.trimmed().isEmpty() || t.snapshotId.size()>200 || t.seasonLabel.trimmed().isEmpty()
       || t.seasonLabel.size()>64 || t.seasonLabel!=t.seasonLabel.trimmed()) {p.reason="E_FILTER_TARGET";return p;}
    for(const auto& g:t.grades)if(!grades.contains(g)){p.reason="E_FILTER_GRADE_TARGET";return p;}
    if(!o.validPage || !o.complete || o.frameId.isEmpty() || o.frameSha256.size()!=64 || o.seasonLabel.isEmpty()){
        p.reason="E_FILTER_OBSERVATION_REQUIRED";return p;
    }
    for(const auto& id:QStringList{"owned","unowned","legendary","epic","rare","common"})
        if(!o.boxes.contains(id) || o.boxes[id].state==vision::CheckState::Unknown){p.reason="E_FILTER_OBSERVATION_REQUIRED";return p;}
    const auto add=[&](const QString& step,const QString& field,const QJsonValue& before,const QJsonValue& after){
        p.steps.append(QJsonObject{{"source_step",step},{"field",field},{"observed",before},{"required",after},
            {"readback_required",true},{"execute",false}});
    };
    p.valid=true;
    if(o.seasonLabel!=t.seasonLabel){
        add("S10","season",o.seasonLabel,t.seasonLabel);
        // Season selection can rebuild the other controls. Their old states
        // must not become future click instructions after this boundary.
        p.reason="reobserve_after_season_change";return p;
    }
    for(const auto& id:QStringList{"owned","unowned"}){
        const bool before=o.boxes[id].state==vision::CheckState::Checked;
        const bool after=id=="owned"?t.owned:t.unowned;
        if(before!=after)add("S12",id,before,after);
    }
    // Ownership changes may also rebuild controls. Recompute grades only
    // after the ownership readbacks, not from pre-change pixel evidence.
    if(!p.steps.isEmpty()){p.reason="reobserve_after_ownership_change";return p;}
    QSet<QString> selected;
    for(const auto& id:grades)if(o.boxes[id].state==vision::CheckState::Checked)selected.insert(id);
    if(selected!=t.grades){
        // S13 clears residual selections, then S14 selects the desired set.
        // A future executor must keep this plan's cursor (not regenerate it
        // after each toggle, which would repeatedly clear partial selections).
        for(const auto& id:grades)if(selected.contains(id))add("S13",id,true,false);
        for(const auto& id:grades)if(t.grades.contains(id))add("S14",id,false,true);
    }
    p.alreadyMatches=p.steps.isEmpty();p.reason=p.alreadyMatches?"preserve_matching_filter":"readback_each_grade_step";
    return p;
}
QJsonObject CatalogFilterPlan::toJson() const {
    return {{"valid",valid},{"already_matches",alreadyMatches},{"reason",reason},{"snapshot_id",snapshotId},
        {"source_frame_id",sourceFrameId},{"steps",steps},{"actions_enabled",false},{"target_grouping_inferred",false}};
}
} // namespace relink::runtime
