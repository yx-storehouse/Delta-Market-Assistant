#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <cstdio>

namespace relink::vision::detail {
QJsonObject selectMeasuredTailTop(const QJsonArray&,const QJsonArray&);
}
using relink::vision::detail::selectMeasuredTailTop;
namespace {int count=0,failures=0;void check(bool b,const char* n){++count;if(!b){++failures;std::printf("FAIL %s\n",n);}}}

int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    QFile file(argc>1?QString::fromLocal8Bit(argv[1]):QString());
    check(file.open(QIODevice::ReadOnly),"recorded fixture opens");
    const auto data=QJsonDocument::fromJson(file.readAll()).object();
    const auto samples=data["samples"].toArray();
    check(samples.size()==3 &&data["pixels_included"]==false,"three original numerical observations, not recreated pixels");
    for(const auto& v:samples){
        const auto sample=v.toObject();const auto rows=sample["observed_complete_rows"].toArray();
        const auto candidates=sample["candidates"].toArray();const auto copyRows=rows,copyCandidates=candidates;
        const auto selected=selectMeasuredTailTop(rows,candidates);
        check(sample["original_accepted"]==false,"source records original rejection");
        check(selected["accepted"]==true &&selected["selected_top_y"]==sample["wanted_measured_top"],"original observed 943/943 pair selected without extrapolation");
        check(rows==copyRows &&candidates==copyCandidates,"source inputs unchanged");
        const auto envelope=selected["same_frame_pitch"].toObject()["paired_gap_envelope"].toObject();
        check(envelope["available"]==true &&envelope["minimum"]==11 &&envelope["maximum"]==15
            &&envelope["extra_margin_pixels"]==3,"measured 11..15 envelope with the per-column 3px edge tolerance");
        check(selected["thresholds_changed"]==false &&selected["actions_enabled"]==false,"numerical selection never authorizes input");
        check(selectMeasuredTailTop(rows,{})["accepted"]==false,"no measured candidate, no invented tail");
        auto candidate=candidates[0].toObject();
        for(const QJsonArray bad: {QJsonArray{-1,943},QJsonArray{943,-1},QJsonArray{943,950},QJsonArray{968,968},QJsonArray{947,947}}){
            auto changed=candidate;changed["refined_top_y"]=bad;changed["score"]=100;
            check(selectMeasuredTailTop(rows,QJsonArray{changed})["accepted"]==false,"missing/misaligned/texture/outside-envelope candidate rejected");
        }
        auto mixed=candidates;auto texture=candidate;texture["refined_top_y"]=QJsonArray{968,968};texture["score"]=100;mixed.append(texture);
        check(selectMeasuredTailTop(rows,mixed)["selected_top_y"]==sample["wanted_measured_top"],"stronger paired interior texture still rejected");
        auto unpaired=rows;auto row=unpaired[0].toObject();auto cells=row["cells"].toArray();auto cell=cells[1].toObject();
        cell["top"]=399;cells[1]=cell;row["cells"]=cells;unpaired[0]=row;
        check(selectMeasuredTailTop(unpaired,candidates)["accepted"]==false,"shared envelope requires aligned complete row edges");
        // A normal 14/15 same-frame sample retains the original per-column path.
        auto normal=rows;row=normal[0].toObject();cells=row["cells"].toArray();cell=cells[0].toObject();
        cell["bottom"]=652;cells[0]=cell;row["cells"]=cells;normal[0]=row;
        const auto legacy=selectMeasuredTailTop(normal,candidates);
        check(legacy["accepted"]==true &&legacy["candidates"].toArray()[0].toObject()["gap_check_basis"]=="per_column_observed_gaps",
              "already passing original gap route stays original");
    }
    {
        // S11 pipeline run05 frame after a favorite (numbers only): paired
        // bottoms 655/654 then 928/928, gaps 11/14, unchanged tail 943/943.
        const auto cell=[](int column,int top,int bottom){return QJsonObject{{"column",column},{"top",top},{"bottom",bottom},
            {"top_edge",true},{"bottom_edge",true}};};
        const QJsonArray rows{QJsonObject{{"cells",QJsonArray{cell(0,392,655),cell(1,393,654)}}},
            QJsonObject{{"cells",QJsonArray{cell(0,666,928),cell(1,668,928)}}}};
        const QJsonArray tail{QJsonObject{{"refined_top_y",QJsonArray{943,943}},{"score",3.73},{"peak_y",943},
            {"usable_columns",QJsonArray{true,true}}}};
        const auto run05=selectMeasuredTailTop(rows,tail);
        check(run05["accepted"]==true &&run05["selected_top_y"]==QJsonArray{943,943}
            &&run05["candidates"].toArray()[0].toObject()["gap_check_basis"]=="aligned_paired_rows_observed_envelope",
            "recorded run05 11/14 gaps with a 15px tail accepted via the toleranced paired envelope");
        const QJsonArray far{QJsonObject{{"refined_top_y",QJsonArray{946,946}},{"score",3.73},{"peak_y",946},
            {"usable_columns",QJsonArray{true,true}}}};
        check(selectMeasuredTailTop(rows,far)["accepted"]==false,"tail 18px below stays outside the toleranced envelope");
    }
    std::printf("COLLECTION_TAIL_GAP_TESTS=%s; assertions=%d; failures=%d; recorded_numeric_only=true; game_input=false\n",failures?"FAIL":"PASS",count,failures);
    return failures?1:0;
}
