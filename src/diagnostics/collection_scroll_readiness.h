#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace relink::diagnostics {
// Negative-only gate for the end of wheel movement. Comparing with the FIRST
// stable candidate prevents slow cumulative drift from passing adjacent-only
// comparisons. These measurements do not rebind a card or prove item identity.
class CollectionScrollReadiness final {
public:
    QJsonObject observe(const QJsonObject& layout,qint64 sourceMs) {
        QJsonObject out{{"schema","collection-scroll-readiness-v1"},{"scope","negative_readiness_gate_only"},
            {"frame_id",layout["frame_id"]},{"frame_sha256",layout["frame_sha256"]},
            {"same_frame",true},{"actions_enabled",false},{"identity_proven",false},
            {"minimum_stable_source_span_ms",100},{"minimum_stable_frames",3},
            {"ready_for_full_ocr",false},{"ocr_deferred",true},{"requires_original_full_validation",true}};
        if(!valid(layout)||sourceMs<0||sourceMs<=m_lastMs||layout["frame_id"].toString()==m_lastId) {
            m_anchor={};m_count=0;m_firstMs=-1;
            out["reason"]="new_complete_layout_required";return out;
        }
        m_lastMs=sourceMs;m_lastId=layout["frame_id"].toString();
        const bool same=!m_anchor.isEmpty()&&matches(m_anchor,layout);
        if(!same){m_anchor=layout;m_firstMs=sourceMs;m_count=1;}
        else ++m_count;
        const bool ready=m_count>=3&&sourceMs-m_firstMs>=100;
        out["reason"]=ready?"current_layout_stable_after_wheel":same?"observing_stable_window":"layout_moved_or_first_frame";
        out["anchor_frame_id"]=m_anchor["frame_id"];
        out["stable_frame_count"]=m_count;out["stable_source_span_ms"]=double(sourceMs-m_firstMs);
        out["ready_for_full_ocr"]=ready;out["ocr_deferred"]=!ready;
        return out;
    }
private:
    static bool rect(const QJsonValue& v) {
        const auto a=v.toArray();if(a.size()!=4)return false;
        for(const auto& n:a)if(!n.isDouble()||!std::isfinite(n.toDouble())||n.toDouble()!=std::floor(n.toDouble())||n.toDouble()<0||n.toDouble()>8192)return false;
        return a[2].toInt()>0&&a[3].toInt()>0;
    }
    static bool valid(const QJsonObject& x) {
        if(x["complete"]!=true||x["same_frame"]!=true||x["schema"]!="collection-layout-v1"
           ||x["frame_id"].toString().isEmpty()||!QRegularExpression("^[0-9a-f]{64}$").match(x["frame_sha256"].toString()).hasMatch()
           ||!rect(x["listing_viewport"]))return false;
        const auto bar=x["scrollbar"].toObject();if(!rect(bar["thumb_bounds"])||!rect(bar["track_bounds"]))return false;
        const auto cards=x["cards"].toArray();if(cards.isEmpty()||cards.size()>64)return false;
        for(const auto& value:cards){const auto c=value.toObject();if(!rect(c["bounds"])||c["bounds_basis"]!="observed"||!c["selected"].isBool())return false;
            for(const auto* side:{"left","top","right","bottom"})if(!c["edges"].toObject()[side].isBool())return false;
            if(!c["fields_bounds"].isNull()&&!rect(c["fields_bounds"]))return false;}
        return true;
    }
    static bool nearRect(const QJsonValue& a,const QJsonValue& b,int xLimit,int yLimit) {
        if(a.isNull()||b.isNull())return a.isNull()&&b.isNull();
        if(!rect(a)||!rect(b))return false;
        const auto aa=a.toArray(),bb=b.toArray();
        return std::abs(aa[0].toInt()-bb[0].toInt())<=xLimit&&std::abs(aa[1].toInt()-bb[1].toInt())<=yLimit
            &&std::abs(aa[0].toInt()+aa[2].toInt()-bb[0].toInt()-bb[2].toInt())<=xLimit
            &&std::abs(aa[1].toInt()+aa[3].toInt()-bb[1].toInt()-bb[3].toInt())<=yLimit;
    }
    static bool matches(const QJsonObject& a,const QJsonObject& b) {
        if(a["viewport"]!=b["viewport"]||a["listing_viewport"]!=b["listing_viewport"]
           ||a["scrollbar"].toObject()["track_bounds"]!=b["scrollbar"].toObject()["track_bounds"]
           ||!nearRect(a["scrollbar"].toObject()["thumb_bounds"],b["scrollbar"].toObject()["thumb_bounds"],0,1))return false;
        auto aa=a["cards"].toArray(),bb=b["cards"].toArray();if(aa.size()!=bb.size())return false;
        // Native enumeration order is row then column; also reject any changed
        // member instead of sorting a missing/reordered card into an old slot.
        for(int i=0;i<aa.size();++i){const auto x=aa[i].toObject(),y=bb[i].toObject();
            if(x["edges"]!=y["edges"]||x["selected"]!=y["selected"]
               ||!nearRect(x["bounds"],y["bounds"],2,1)||!nearRect(x["fields_bounds"],y["fields_bounds"],2,1))return false;
        }
        return true;
    }
    QJsonObject m_anchor;
    QString m_lastId;
    qint64 m_firstMs=-1,m_lastMs=-1;
    int m_count=0;
};
}
