#include "catalog_filter_reader.h"
#include "skin_page_classifier.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace relink::vision {
namespace {
using runtime::observation::FrameEnvelope;
struct Word { QString text; QRectF box; };
bool pixelsValid(const FrameEnvelope& f) {
    return f.pixelFormat == "BGRA8" && f.width > 0 && f.width <= 8192 && f.height > 0 && f.height <= 8192
        && f.strideBytes >= qint64(f.width)*4 && f.strideBytes <= qint64(f.width)*4+65536
        && f.validBytes == qint64(f.strideBytes)*f.height && f.pixels.size() == f.validBytes;
}
QString labelIn(const QJsonObject& o, const QRectF& region) {
    QVector<Word> words;
    for (const auto& v : o["words"].toArray()) {
        const auto w=v.toObject();
        const QRectF b(w["x"].toDouble(),w["y"].toDouble(),w["width"].toDouble(),w["height"].toDouble());
        if (!region.contains(b.center())) continue;
        if (w.contains("score") && (!w["score"].isDouble() || w["score"].toDouble()<.7)) return {};
        QString t=w["text"].toString().normalized(QString::NormalizationForm_KC);
        t.remove(QRegularExpression(QStringLiteral("\\s+")));
        if (t.isEmpty() || t.size()>64 || words.size()>=32) return {};
        words.append({t,b});
    }
    std::sort(words.begin(),words.end(),[](const Word& a,const Word& b){return a.box.left()<b.box.left();});
    QString result;
    for (int i=0;i<words.size();++i) {
        if(i) {
            const auto& a=words[i-1].box;const auto& b=words[i].box;
            if (std::abs(a.center().y()-b.center().y())>.5*std::min(a.height(),b.height())
                || b.left()-a.right() > std::max(a.height(),b.height())
                || b.left()-a.right() < -.3*std::min(a.height(),b.height())) return {};
        }
        result+=words[i].text;
    }
    return result.size()<=64 ? result : QString();
}
}
QString toString(CheckState s) {
    return s==CheckState::Checked ? QStringLiteral("checked") : s==CheckState::Unchecked ? QStringLiteral("unchecked") : QStringLiteral("unknown");
}
CheckState classifyFilterCheckboxMeasurement(const QJsonObject& signal) {
    const char* keys[]={"interior_mean","interior_stddev","center_mean","center_stddev","bright_fraction","green_fraction","sample_count"};
    for(const auto key:keys)if(!signal[key].isDouble() || !std::isfinite(signal[key].toDouble()) || signal[key].toDouble()<0)return CheckState::Unknown;
    const auto borders=signal["border_means"].toArray();
    if(borders.size()!=4 || signal["sample_count"].toDouble()<16 || signal["sample_count"].toDouble()>16384
       || std::floor(signal["sample_count"].toDouble())!=signal["sample_count"].toDouble())return CheckState::Unknown;
    double minBorder=255,maxBorder=0;
    for(const auto v:borders){if(!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble()<0 || v.toDouble()>255)return CheckState::Unknown;
        minBorder=std::min(minBorder,v.toDouble());maxBorder=std::max(maxBorder,v.toDouble());}
    const double mean=signal["interior_mean"].toDouble(),dev=signal["interior_stddev"].toDouble(),
        center=signal["center_mean"].toDouble(),centerDev=signal["center_stddev"].toDouble(),
        bright=signal["bright_fraction"].toDouble(),green=signal["green_fraction"].toDouble();
    if(mean>255 || center>255 || dev>128 || centerDev>128 || bright>1 || green>1)return CheckState::Unknown;
    if(mean>=8 && mean<=42 && dev<=5 && minBorder-mean>=7 && bright==0 && green==0
       && center>=8 && center<=42 && centerDev<=5 && std::abs(center-mean)<=5)return CheckState::Unchecked;
    // The observed selected style is a WHITE inset square, not a green check.
    // Require its bright flat center plus dark perimeter and mixed edge band;
    // a flat bright rectangle, icon speck or a missing border is not selected.
    if(mean>=120 && mean<=210 && dev>=20 && dev<=70 && center>=160 && center<=225 && centerDev<=6
       && bright>=.75 && bright<=.97 && green==0 && minBorder>=35 && maxBorder<=100 && center-maxBorder>=80)
        return CheckState::Checked;
    return CheckState::Unknown;
}
FilterCheckbox measureFilterCheckbox(const FrameEnvelope& frame, const QRect& rect, const QString& label) {
    FilterCheckbox out;out.label=label;out.bounds=rect;
    if(!pixelsValid(frame) || rect.width()<16 || rect.height()!=rect.width() || rect.width()>128
       || !QRect(0,0,frame.width,frame.height).contains(rect)) {out.reason="E_FILTER_PIXELS";return out;}
    const int margin=std::max(4,rect.width()/5);
    const QRect core=rect.adjusted(margin,margin,-margin,-margin);
    const int centerMargin=std::max(margin+2,rect.width()*3/10);
    const QRect center=rect.adjusted(centerMargin,centerMargin,-centerMargin,-centerMargin);
    double sum=0,squared=0,centerSum=0,centerSquared=0;int total=0,centerTotal=0,bright=0,green=0;double sides[4]={};int sideCounts[4]={};
    for(int y=rect.top();y<=rect.bottom();++y)for(int x=rect.left();x<=rect.right();++x){
        const auto* p=reinterpret_cast<const unsigned char*>(frame.pixels.constData()+qint64(y)*frame.strideBytes+qint64(x)*4);
        const double l=(int(p[0])+int(p[1])+int(p[2]))/3.0;
        if(core.contains(x,y)){sum+=l;squared+=l*l;++total;bright+=l>=100;green+=int(p[1])>=70 && int(p[1])-int(p[2])>=25 && int(p[1])-int(p[0])>=8;}
        if(center.contains(x,y)){centerSum+=l;centerSquared+=l*l;++centerTotal;}
        const bool edge[]={y<rect.top()+3,y>rect.bottom()-3,x<rect.left()+3,x>rect.right()-3};
        for(int i=0;i<4;++i)if(edge[i]){sides[i]+=l;++sideCounts[i];}
    }
    const double mean=sum/total,deviation=std::sqrt(std::max(0.0,squared/total-mean*mean));
    QJsonArray borders;
    for(int i=0;i<4;++i){sides[i]/=sideCounts[i];borders.append(sides[i]);}
    const double centerMean=centerSum/centerTotal;
    out.measurement={{"interior_mean",mean},{"interior_stddev",deviation},{"bright_fraction",double(bright)/total},
        {"green_fraction",double(green)/total},{"border_means",borders},{"sample_count",total},
        {"center_mean",centerMean},{"center_stddev",std::sqrt(std::max(0.0,centerSquared/centerTotal-centerMean*centerMean))}};
    out.state=classifyFilterCheckboxMeasurement(out.measurement);
    out.reason=out.state==CheckState::Unchecked ? "closed_dark_empty_box" : out.state==CheckState::Checked ? "white_inset_selected_box" : "E_FILTER_CHECKBOX_AMBIGUOUS";
    return out;
}
CatalogFilterState readCatalogFilter(const FrameEnvelope& frame, const QJsonObject& o) {
    CatalogFilterState out;out.frameId=frame.frameId;
    const auto page=classifySkinPage(o);
    if(!page.validInput || page.page!=SkinPage::CatalogFilter || page.overlay!=PageOverlay::None){out.reason="E_FILTER_PAGE";return out;}
    if(!pixelsValid(frame) || frame.frameId.isEmpty() || o["coordinate_space"]!="client_physical_px"
        || o["width"].toInt()!=frame.width || o["height"].toInt()!=frame.height){out.reason="E_FILTER_FRAME";return out;}
    out.validPage=true;
    if(frame.width!=2560 || frame.height!=1440 || frame.dpiX!=144 || frame.dpiY!=144){out.reason="E_FILTER_GEOMETRY_UNCALIBRATED";return out;}
    out.frameSha256=QString::fromLatin1(QCryptographicHash::hash(frame.pixels,QCryptographicHash::Sha256).toHex());
    if(o["frame_id"].toString()!=out.frameId || o["frame_sha256"].toString()!=out.frameSha256){out.reason="E_FILTER_FRAME_BINDING";return out;}
    out.seasonLabel=labelIn(o,QRectF(770,426,270,42));
    const QStringList ids{"owned","unowned","legendary","epic","rare","common"};
    const QStringList labels{QStringLiteral("已拥有"),QStringLiteral("未拥有"),QStringLiteral("传说品阶"),QStringLiteral("史诗品阶"),QStringLiteral("稀有品阶"),QStringLiteral("普通品阶")};
    const QPoint locations[]={{754,534},{1073,534},{754,630},{1073,630},{1392,630},{1711,630}};
    out.complete=!out.seasonLabel.isEmpty();
    for(int i=0;i<ids.size();++i){
        const QRect box(locations[i],QSize(36,36));
        auto measured=measureFilterCheckbox(frame,box,labels[i]);
        const auto readLabel=labelIn(o,QRectF(box.right()+8,box.top(),180,36));
        measured.observedLabel=readLabel;
        if(readLabel!=labels[i]) {measured.state=CheckState::Unknown;measured.reason="E_FILTER_LABEL";}
        out.complete=out.complete && measured.state!=CheckState::Unknown;
        out.boxes.insert(ids[i],measured);
    }
    out.reason=out.complete ? "filter_state_observed" : "E_FILTER_INCOMPLETE";
    return out;
}
QJsonObject CatalogFilterState::toJson() const {
    QJsonObject values;
    for(auto i=boxes.cbegin();i!=boxes.cend();++i){const auto& b=i.value();values[i.key()]=QJsonObject{
        {"label",b.label},{"observed_label",b.observedLabel},{"state",toString(b.state)},{"reason",b.reason},{"measurement",b.measurement},
        {"bounds",QJsonArray{b.bounds.x(),b.bounds.y(),b.bounds.width(),b.bounds.height()}}};}
    return {{"schema","catalog-filter-state-v1"},{"valid_page",validPage},{"complete",complete},{"reason",reason},
        {"frame_id",frameId},{"frame_sha256",frameSha256},{"season_label",seasonLabel},{"checkboxes",values},
        {"geometry_profile","catalog-2560x1440-dpi144-v1"},{"same_frame",true},{"actions_enabled",false},
        {"image_file_writes",0},{"calibration_evidence","catalog_filter_signals_20261007"},{"all_states_live_validated",false}};
}
} // namespace relink::vision
