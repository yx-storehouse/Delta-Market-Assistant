#include "collection_layout.h"
#include <QCryptographicHash>
#include "frame_sha256.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <QRect>
#include <QMap>
#include <QVector>
#include <limits>

namespace relink::vision {
namespace {
using runtime::observation::FrameEnvelope;
double rounded(double value) { return std::round(value * 100.0) / 100.0; }
double luminance(const FrameEnvelope& frame, int x, int y) {
    const auto* pixel = reinterpret_cast<const unsigned char*>(frame.pixels.constData())
        + qint64(y) * frame.strideBytes + x * 4;
    return (double(pixel[0]) + double(pixel[1]) + double(pixel[2])) / 3.0;
}
struct Stats { double mean = 0, variance = 0; };
Stats horizontal(const FrameEnvelope& frame, int x, int y, int width) {
    double total = 0, squares = 0;
    for (int i=0; i<width; ++i) { const double value=luminance(frame,x+i,y); total+=value; squares+=value*value; }
    const double mean=total/width;
    return {mean,std::max(0.0,squares/width-mean*mean)};
}
}

QJsonObject collectionLayoutProfile(const FrameEnvelope& frame) {
    QJsonObject out{{"schema","collection-layout-profile-v1"},{"complete",false},
        {"layout_proven",false},{"actions_enabled",false},{"image_file_writes",0},
        {"frame_id",frame.frameId},{"same_frame",false}};
    if(frame.frameId.isEmpty() || frame.pixelFormat!="BGRA8" || frame.width!=2560 || frame.height!=1440
        || frame.dpiX!=144 || frame.dpiY!=144 || frame.strideBytes!=2560*4
        || frame.validBytes!=qint64(frame.strideBytes)*frame.height || frame.pixels.size()!=frame.validBytes) {
        out["error"]="E_COLLECTION_PROFILE_FRAME"; return out;
    }
    const auto digest=frameSha256(frame.pixels);
    out["frame_sha256"]=QString::fromLatin1(digest.bytes.toHex());
    out["frame_digest_metrics"]=QJsonObject{{"provider",digest.provider},{"elapsed_ms",digest.elapsedNs/1e6},{"bytes",frame.pixels.size()}};
    out["same_frame"]=true;
    out["viewport"]=QJsonArray{frame.width,frame.height};
    out["search_basis"]="historical_columns_for_candidate_search_only";
    out["y_first"]=280;out["y_last"]=1300;
    out["row_columns"]=QJsonArray{"y","mean","variance","left_3px_mean","right_3px_mean",
        "wide_band_mean","mean_delta_previous_y","wide_band_delta_previous_y"};
    QJsonArray bands;
    for(int column=0;column<2;++column){
        const int x=column==0?120:997;
        const int wideX=column==0?400:1277;
        QJsonArray rows;
        double previousMean=0,previousWide=0;
        for(int y=280;y<=1300;++y){
            const auto whole=horizontal(frame,x,y,865);
            const auto left=horizontal(frame,x,y,3),right=horizontal(frame,x+862,y,3);
            const auto wide=horizontal(frame,wideX,y,400);
            rows.append(QJsonArray{y,rounded(whole.mean),rounded(whole.variance),rounded(left.mean),rounded(right.mean),
                rounded(wide.mean),y==280?0.0:rounded(whole.mean-previousMean),y==280?0.0:rounded(wide.mean-previousWide)});
            previousMean=whole.mean;previousWide=wide.mean;
        }
        bands.append(QJsonObject{{"column",column},{"x_first",x},{"x_last",x+864},
            {"wide_x_first",wideX},{"wide_x_last",wideX+399},{"rows",rows}});
    }
    out["row_bands"]=bands;
    // Each vertical signal is the mean of its 3-pixel horizontal sampling band,
    // not RGB pixels or an encoded screenshot. It makes thin track/thumb and
    // list-boundary candidates inspectable before choosing detector thresholds.
    QJsonArray vertical;
    for(int x=1858;x<=1900;++x){
        QJsonArray means;
        double minimum=255,maximum=0;
        for(int y=280;y<=1300;++y){
            const double mean=horizontal(frame,x-1,y,3).mean;
            minimum=std::min(minimum,mean);maximum=std::max(maximum,mean);
            means.append(rounded(mean));
        }
        vertical.append(QJsonObject{{"x",x},{"sample_width",3},{"y_first",280},{"y_step",1},
            {"minimum",rounded(minimum)},{"maximum",rounded(maximum)},{"means",means}});
    }
    out["right_edge_vertical_profiles"]=vertical;
    return out;
}

namespace detail {
// Pure numerical selection for the native detector and its recorded-measurement
// regression. This is not a layout/identity proof and never enables input.
QJsonObject selectMeasuredTailTop(const QJsonArray& observedRows,const QJsonArray& measuredCandidates){
    constexpr int tolerance=3;
    QVector<int> tops[2],bottoms[2],pitches[2],bottomPitches[2],gaps[2];
    bool coherent=true,pairedRowsAligned=true;
    int completeRowCount=0;
    for(const auto& value:observedRows){
        const auto cells=value.toObject()["cells"].toArray();
        if(cells.size()!=2){coherent=false;break;}
        bool complete=true;int rowTops[2],rowBottoms[2];
        for(int c=0;c<2;++c){
            const auto cell=cells[c].toObject();
            if(cell["column"].toInt(-1)!=c){coherent=false;break;}
            if(cell["top_edge"]!=true ||cell["bottom_edge"]!=true){complete=false;continue;}
            const int top=cell["top"].toInt(-1),bottom=cell["bottom"].toInt(-1);
            if(top<0 ||bottom-top+1<240 ||bottom-top+1>275){coherent=false;break;}
            rowTops[c]=top;rowBottoms[c]=bottom;
        }
        if(!coherent)break;
        if(complete){
            ++completeRowCount;
            pairedRowsAligned=pairedRowsAligned &&std::abs(rowTops[0]-rowTops[1])<=tolerance
                &&std::abs(rowBottoms[0]-rowBottoms[1])<=tolerance;
            for(int c=0;c<2;++c){tops[c].append(rowTops[c]);bottoms[c].append(rowBottoms[c]);}
        }
    }
    QJsonArray columns;
    int minimum[2]={0,0},maximum[2]={0,0};
    for(int c=0;c<2;++c){
        QJsonArray observedTops,observedBottoms,observedPitches,observedBottomPitches,observedGaps;
        for(int i=0;i<tops[c].size();++i){
            observedTops.append(tops[c][i]);observedBottoms.append(bottoms[c][i]);
            if(i){
                const int pitch=tops[c][i]-tops[c][i-1],bottomPitch=bottoms[c][i]-bottoms[c][i-1];
                const int gap=tops[c][i]-bottoms[c][i-1];
                pitches[c].append(pitch);observedPitches.append(pitch);
                bottomPitches[c].append(bottomPitch);observedBottomPitches.append(bottomPitch);
                gaps[c].append(gap);observedGaps.append(gap);
            }
        }
        if(gaps[c].isEmpty())coherent=false;
        else{
            minimum[c]=*std::min_element(gaps[c].begin(),gaps[c].end());
            maximum[c]=*std::max_element(gaps[c].begin(),gaps[c].end());
            if(minimum[c]<=0 ||maximum[c]-minimum[c]>2*tolerance)coherent=false;
            const auto bottomRange=std::minmax_element(bottomPitches[c].begin(),bottomPitches[c].end());
            if(*bottomRange.first<=0 ||*bottomRange.second-*bottomRange.first>2*tolerance)coherent=false;
        }
        columns.append(QJsonObject{{"column",c},{"observed_complete_top_y",observedTops},
            {"observed_complete_bottom_y",observedBottoms},{"observed_top_pitches",observedPitches},
            {"observed_bottom_pitches",observedBottomPitches},{"observed_inter_row_gaps",observedGaps},
            {"minimum_gap",minimum[c]},{"maximum_gap",maximum[c]},
            {"last_complete_top_y",tops[c].isEmpty()?-1:tops[c].last()},
            {"last_complete_bottom_y",bottoms[c].isEmpty()?-1:bottoms[c].last()}});
    }
    if(std::max(minimum[0],minimum[1])-tolerance>std::min(maximum[0],maximum[1])+tolerance)coherent=false;
    // A selected outline may extend its top by five observed pixels while its
    // lower band stays stable. Top-to-top pitch then contaminates the next
    // boundary check (run03: 538->818 is 280, not the ordinary 275). Compare
    // actual next-top/previous-bottom gaps instead; no top is extrapolated.
    // Per-column gap samples can differ even when BOTH top and bottom edges
    // of every paired complete row meet the existing 3px alignment contract.
    // run06: paired tops392/394 and bottoms655/653, then tops666/668,
    // bottoms928/928. Gaps11/15 are coherent; insisting that the next measured
    // 15/15 pair also be <=11+3 discards a genuine measured candidate.
    // A narrowly bounded second witness uses the actual paired-row envelope
    // with the same 3px edge tolerance as the per-column check, only when all
    // contributing paired edges align. S11 pipeline run05: after a favorite,
    // paired bottoms read 655/654 (652/653 one frame earlier), so the gaps
    // became 11/14 while the unchanged 943/943 tail sat 15px below 928/928.
    // It never supplies a missing edge, grows the search, or extrapolates y.
    const int sharedMinimum=std::min(minimum[0],minimum[1]);
    const int sharedMaximum=std::max(maximum[0],maximum[1]);
    const bool sharedWitness=coherent &&pairedRowsAligned &&completeRowCount>=2
        &&sharedMinimum>0 &&sharedMaximum-sharedMinimum<=2*tolerance;
    const QJsonObject pitchProof{{"basis","same_frame_independent_complete_row_gaps_and_bottom_intervals"},
        {"proven",coherent},{"tolerance_pixels",tolerance},{"columns",columns},
        {"paired_complete_rows_aligned",pairedRowsAligned},{"complete_row_count",completeRowCount},
        {"paired_gap_envelope",QJsonObject{{"available",sharedWitness},{"minimum",sharedMinimum},
            {"maximum",sharedMaximum},{"extra_margin_pixels",tolerance},{"coordinates_inferred",false}}},
        {"coordinates_inferred",false}};
    QJsonArray candidates;double best=2;int peak=-1;QJsonArray selectedTops;
    for(const auto& value:measuredCandidates){
        auto candidate=value.toObject();const auto refined=candidate["refined_top_y"].toArray();
        const int left=refined.size()==2?refined[0].toInt(-1):-1;
        const int right=refined.size()==2?refined[1].toInt(-1):-1;
        bool valid=left>=0 &&right>=0 &&std::abs(left-right)<=tolerance;
        QString reason;
        if(left<0)reason="left_top_unobserved";
        else if(right<0)reason="right_top_unobserved";
        else if(std::abs(left-right)>tolerance)reason="column_tops_misaligned";
        if(valid &&coherent){
            QJsonArray distances;bool agrees=true,agreesWithPairedEnvelope=sharedWitness;
            for(int c=0;c<2;++c){
                const int distance=refined[c].toInt()-bottoms[c].last();distances.append(distance);
                agrees=agrees &&distance>=minimum[c]-tolerance &&distance<=maximum[c]+tolerance;
                agreesWithPairedEnvelope=agreesWithPairedEnvelope &&distance>=sharedMinimum-tolerance
                    &&distance<=sharedMaximum+tolerance;
            }
            agreesWithPairedEnvelope=agreesWithPairedEnvelope
                &&std::abs(distances[0].toInt()-distances[1].toInt())<=tolerance;
            candidate["gap_from_last_complete_bottom_y"]=distances;
            candidate["gap_check_basis"]=agrees?"per_column_observed_gaps":
                agreesWithPairedEnvelope?"aligned_paired_rows_observed_envelope":"unproven";
            agrees=agrees ||agreesWithPairedEnvelope;
            candidate["observed_gap_consistent"]=agrees;
            if(!agrees){valid=false;reason="observed_inter_row_gap_mismatch";}
        }
        candidate["accepted_for_ranking"]=valid;candidate["rejection_reason"]=reason;
        candidates.append(candidate);
        const double score=candidate["score"].toDouble();
        if(valid &&score>best){best=score;peak=candidate["peak_y"].toInt(-1);selectedTops=refined;}
    }
    QJsonObject out{{"schema","collection-tail-top-selection-v1"},
        {"basis","same_frame_both_column_refined_top_edges"},
        {"maximum_column_alignment_pixels",tolerance},{"candidates",candidates},
        {"selected_peak_y",peak},{"accepted",peak>=0},{"thresholds_changed",false},
        {"same_frame_pitch",pitchProof},{"actions_enabled",false}};
    if(peak>=0)out["selected_top_y"]=selectedTops;
    return out;
}
} // namespace detail

namespace {
QJsonArray rectangle(int x,int y,int w,int h){return {x,y,w,h};}
struct Row {double mean=0,variance=0,left=0,right=0,wide=0;};
struct Signals {
    QVector<Row> columns[2];
    QMap<int,QVector<double>> vertical;
    bool valid=false;
    Row row(int column,int y) const {return columns[column].at(y-280);}
    double v(int x,int y) const {return vertical.value(x).at(y-280);}
    double contrast(int x,int y) const {return v(x,y)-(v(x-4,y)+v(x+4,y))/2;}
    double leftBackgroundExcess(int x,int y) const {
        // The two bands lie outside the thin track. Their affine continuation
        // models local scene slope without using a bright right-hand texture.
        return v(x,y)-3*v(x-6,y)+2*v(x-9,y);
    }
    bool trackSupport(int x,int y) const {return contrast(x,y)>5 ||leftBackgroundExcess(x,y)>5;}
};
Signals readSignals(const QJsonObject& profile){
    Signals s;
    if(profile["schema"]!="collection-layout-profile-v1" || profile["same_frame"]!=true
        ||profile["viewport"].toArray()!=QJsonArray{2560,1440})return s;
    const auto columns=profile["row_bands"].toArray();
    if(columns.size()!=2)return s;
    for(int c=0;c<2;++c){
        const auto rows=columns[c].toObject()["rows"].toArray();if(rows.size()!=1021)return s;
        for(int i=0;i<rows.size();++i){const auto a=rows[i].toArray();if(a.size()!=8 ||a[0]!=280+i)return s;
            s.columns[c].append({a[1].toDouble(),a[2].toDouble(),a[3].toDouble(),a[4].toDouble(),a[5].toDouble()});}
    }
    for(const auto& entry:profile["right_edge_vertical_profiles"].toArray()){
        const auto o=entry.toObject();const auto a=o["means"].toArray();if(a.size()!=1021)return s;
        QVector<double> values;values.reserve(a.size());for(const auto& v:a)values.append(v.toDouble());
        s.vertical[o["x"].toInt()]=values;
    }
    for(int x=1858;x<=1900;++x)if(!s.vertical.contains(x))return s;
    s.valid=true;return s;
}
struct Span {int first=0,last=-1;int length()const{return last-first+1;}};
QVector<Span> runs(const QVector<bool>& values,int first,int minimum){
    QVector<Span> result;int start=-1;
    for(int i=0;i<=values.size();++i){
        if(i<values.size() && values[i]){if(start<0)start=i;}
        else if(start>=0){if(i-start>=minimum)result.append({first+start,first+i-1});start=-1;}
    }
    return result;
}
QJsonObject scrollbar(const Signals& s){
    // Three-pixel center-minus-flanks contrast removes the translucent scene's
    // large vertical luminance drift. The real four-frame replay measured the
    // track at y304..1202 and the moving thumb at 304/310/316/304 (height62).
    int center=-1;Span track;double best=-1;
    for(int x=1868;x<=1888;++x){
        QVector<bool> flags;for(int y=290;y<=1220;++y)flags.append(s.contrast(x,y)>10);
        for(auto span:runs(flags,290,350)){
            // High contrast seeds the track. Extend only over a continuously
            // observed lower-contrast line: the translucent background reduces
            // the real lower track to 7.7 in probe07, but its true end drops to
            // zero abruptly. Probe08 also has an actual bright texture in the
            // right flank at y1160..1166. An independent affine background
            // from the left flank still measures >9.6 track excess there; it
            // falls below1.3 after the true endpoint in all seven real frames.
            // No missing pixels are filled and no endpoint is fixed by size.
            while(span.first>290 &&s.trackSupport(x,span.first-1))--span.first;
            while(span.last<1220 &&s.trackSupport(x,span.last+1))++span.last;
            double score=0;for(int y=span.first;y<=span.last;++y)score+=std::min(30.0,s.contrast(x,y));
            score+=span.length()*100;
            if(score>best){best=score;track=span;center=x;}
        }
    }
    if(center<0)return {{"error","E_COLLECTION_SCROLLBAR_TRACK"}};
    // Publish the measured centerline rather than unstable translucent flanks.
    // Its one-pixel rectangle is contained in both the track and the thumb;
    // neither its x nor its endpoints are inherited from a previous frame.
    const int left=center,right=center;
    QVector<bool> flags;for(int y=track.first;y<=track.last;++y)flags.append(s.contrast(center,y)>35);
    const auto thumbs=runs(flags,track.first,8);
    if(thumbs.size()!=1 ||thumbs[0].length()>=track.length())return {{"error","E_COLLECTION_SCROLLBAR_THUMB"}};
    const auto thumb=thumbs[0];
    return {{"track_bounds",rectangle(left,track.first,right-left+1,track.length())},
        {"thumb_bounds",rectangle(left,thumb.first,right-left+1,thumb.length())},
        {"measured_center_x",center},{"geometry_basis","observed_centerline_only"},{"method","symmetric_contrast_with_independent_left_affine_background"}};
}
bool bottomCandidate(const Signals& s,int c,int y,bool requireDirection=true){
    const auto r=s.row(c,y),after=s.row(c,y+4),before=s.row(c,y-4);
    return r.left-after.left>25 &&r.right-after.right>12 &&r.wide-after.wide>7
        && r.variance<400 &&(!requireDirection ||before.wide>after.wide+.8);
}
bool narrowBottomRidge(const Signals& s,int c,int y){
    const auto r=s.row(c,y),before=s.row(c,y-4),after=s.row(c,y+4);
    // Remove the first-order vertical scene gradient. In the two actual run03
    // failures the wide before/after direction reversed, but this narrow ridge
    // still measured 8.81..10.22 levels in BOTH columns at the missing edge.
    return r.variance<400 &&r.left-(before.left+after.left)/2>25
        &&r.right-(before.right+after.right)/2>12 &&r.wide-(before.wide+after.wide)/2>5;
}
bool sceneRelativePeerRidge(const Signals& s,int c,int y){
    const auto r=s.row(c,y),before=s.row(c,y-4),after=s.row(c,y+4);
    // Run09: a peer still had a strict three-band ridge, while the opposite
    // right endpoint was diluted by its fixed 3px sampling band (9.835), or
    // its existing scene variance was already 389..452 (edge 409..431).
    // Do not raise the primary thresholds. This secondary witness requires
    // an independent strict peer, a strong left/wide ridge, a right ridge
    // exceeding the measured local background change, and <=10% additional
    // variance over that background. It is never a standalone bottom.
    const double rightRidge=r.right-(before.right+after.right)/2;
    const bool stableVariance=r.variance<400
        ||r.variance<=1.10*std::max(before.variance,after.variance);
    return stableVariance &&r.left-(before.left+after.left)/2>25
        &&r.wide-(before.wide+after.wide)/2>5
        &&rightRidge>std::max(1.0,std::abs(before.right-after.right));
}
bool bottomFillDirection(const Signals& s,int y){
    // Selected/hovered top outlines have a large inward fill transition.
    // The independent outside background removes local scene slope.
    const double before=s.row(1,y-4).right-s.v(1866,y-4);
    const double after=s.row(1,y+4).right-s.v(1866,y+4);
    return before-after>-5;
}
bool corroboratedBottomRidge(const Signals& s,int y){
    bool paired=false;
    for(int c=0;c<2;++c)if(narrowBottomRidge(s,c,y))for(int d=-2;d<=2;++d)
        paired=paired ||narrowBottomRidge(s,1-c,y+d) ||sceneRelativePeerRidge(s,1-c,y+d);
    if(!paired)return false;
    // A selected outline plus a hovered peer also makes two bright TOP lines
    // (real probe08). Reject its large inward side-fill transition. The local
    // outside background cancels fog drift; recorded bottom edges range down
    // to -2.34 here, whereas that observed top is -18.22..-20.33.
    return bottomFillDirection(s,y);
}
bool corroboratedSceneRelativeEdge(const Signals& s,int c,int y){
    if(!sceneRelativePeerRidge(s,c,y) ||!bottomFillDirection(s,y))return false;
    for(int d=-2;d<=2;++d)if(narrowBottomRidge(s,1-c,y+d))return true;
    return false;
}
struct RowSpan {int top=0,bottom=0;bool topEdge=false,bottomEdge=false;};
QVector<int> detectedBottoms(const Signals& s,int top,int bottom){
    QVector<bool> flags;
    for(int y=top;y<=bottom-4;++y)flags.append(bottomCandidate(s,0,y)||bottomCandidate(s,1,y)||corroboratedBottomRidge(s,y));
    QVector<int> out;
    for(const auto cluster:runs(flags,top,1)){
        // Merge multiple antialias rows of one observed horizontal edge.
        int edge=cluster.last;
        if(!out.isEmpty() &&edge-out.last()<6)out.last()=edge;else out.append(edge);
    }
    return out;
}
int actualTop(const Signals& s,int column,int peak);
int findTop(const Signals& s,int first,int last,bool pairedSceneVariance=false,QJsonObject* tailSelectionEvidence=nullptr,
            const QJsonArray& observedRows={}){
    double best=2;int peak=-1;QJsonArray candidates;
    for(int y=std::max(285,first);y<=std::min(1297,last);++y){
        double score[2];bool usable[2],pairedUsable[2];
        for(int c=0;c<2;++c){score[c]=s.row(c,y+3).wide-2*s.row(c,y-1).wide+s.row(c,y-5).wide;
            const bool measuredRise=score[c]>2 &&s.row(c,y+3).wide-s.row(c,y-1).wide>2;
            usable[c]=s.row(c,y+3).variance<400 &&measuredRise;
            pairedUsable[c]=measuredRise &&s.row(c,y+3).variance<=1.10
                *std::max(s.row(c,y-1).variance,s.row(c,y-5).variance);}
        // Only a tail search may use this secondary route, and both columns
        // must independently show the same local fill step. The run09 middle
        // retry had 470..505 scene variance on the actual clipped next row;
        // its step did not increase that variance. A single noisy rise is not
        // enough, nor does a missing tail become an observed empty region.
        if(pairedSceneVariance &&pairedUsable[0] &&pairedUsable[1])usable[0]=usable[1]=true;
        // A selected/hovered edge can obscure one column's small fill step.
        // Its peer supplies a candidate, not the final card edge.
        if(!usable[0] &&!usable[1])continue;
        double current=usable[0]&&usable[1]?std::min(score[0],score[1]):(usable[0]?score[0]:score[1]);
        if(pairedSceneVariance){
            // A clipped-tail candidate has no lower horizontal edge to reject
            // an interior texture. Probe01's stronger one-column peak at
            // y1010 displaced the actual paired y983 edge; refining the left
            // column then returned -1. Refine BOTH measured edges before
            // comparing scores. This changes only the tail route, not any
            // photometric threshold or the ordinary complete-row search.
            const int leftTop=actualTop(s,0,y),rightTop=actualTop(s,1,y);
            const bool valid=leftTop>=0 &&rightTop>=0 &&std::abs(leftTop-rightTop)<=3;
            QString reason;
            if(leftTop<0)reason="left_top_unobserved";
            else if(rightTop<0)reason="right_top_unobserved";
            else if(std::abs(leftTop-rightTop)>3)reason="column_tops_misaligned";
            candidates.append(QJsonObject{{"peak_y",y},{"score",current},
                {"column_scores",QJsonArray{score[0],score[1]}},
                {"usable_columns",QJsonArray{usable[0],usable[1]}},
                {"refined_top_y",QJsonArray{leftTop,rightTop}},
                {"accepted_for_ranking",valid},{"rejection_reason",reason}});
            continue;
        }
        if(current>best){best=current;peak=y;}
    }
    if(pairedSceneVariance){
        // AS Val run02 also contains a genuinely paired interior texture,
        // 25px below the weaker real edge. Independent complete-row pitches
        // reject that measured-but-inconsistent candidate without fabricating
        // a replacement coordinate or changing any photometric threshold.
        auto selection=detail::selectMeasuredTailTop(observedRows,candidates);
        selection["search_interval"]=QJsonArray{std::max(285,first),std::min(1297,last)};
        peak=selection["selected_peak_y"].toInt(-1);
        if(tailSelectionEvidence)*tailSelectionEvidence=selection;
    }
    return peak;
}
int actualTop(const Signals& s,int column,int peak);
int actualBottom(const Signals& s,int column,int edge);
QJsonObject shortTailGapEvidence(const Signals& s,const QVector<RowSpan>& rows,int bottom){
    const int lastBottom=rows.last().bottom,gap=bottom-lastBottom;
    QJsonObject proof{{"accepted",false},{"basis","same_frame_measured_inter_row_gap"},
        {"gap_pixels",gap},{"maximum_gap_pixels",16},{"actions_enabled",false}};
    if(gap<=12 ||gap>16){proof["error"]="E_COLLECTION_TAIL_GAP_BOUND";return proof;}
    QJsonArray measured;
    for(int c=0;c<2;++c){
        QJsonArray gaps;int largest=0;
        for(int i=1;i<rows.size();++i){
            if(!rows[i].topEdge ||!rows[i-1].bottomEdge)continue;
            const int nextTop=actualTop(s,c,rows[i].top),priorBottom=actualBottom(s,c,rows[i-1].bottom);
            const int distance=nextTop-priorBottom;
            if(nextTop<0 ||priorBottom<0 ||distance<=0 ||distance>16)continue;
            gaps.append(QJsonObject{{"previous_bottom",priorBottom},{"next_top",nextTop},{"distance",distance}});
            largest=std::max(largest,distance);
        }
        measured.append(QJsonObject{{"column",c},{"intervals",gaps},{"largest_measured_distance",largest}});
        proof["observed_inter_row_gaps"]=measured;
        if(gaps.size()<2 ||gap>largest){proof["error"]="E_COLLECTION_TAIL_GAP_NOT_CORROBORATED";return proof;}
        // An inter-row gap may follow a smooth translucent scene slope, but
        // not a persistent new card fill. Check each column independently.
        // Three in-viewport samples reject a fill step, while the measured
        // single-pixel viewport-bottom stroke is not mistaken for a new row.
        for(int y=lastBottom+3;y<=bottom-2;++y){
            const double background=(s.row(c,y-2).wide-s.row(c,y-5).wide)/3;
            const double before=s.row(c,y-1).wide;
            const double rise=s.row(c,y).wide-before;
            if(rise<=std::max(.65,background+.65))continue;
            bool sustained=true;
            for(int d=0;d<3;++d)sustained=sustained
                &&s.row(c,y+d).wide-(before+(d+1)*background)>.65;
            if(sustained){
                proof["error"]="E_COLLECTION_TAIL_GAP_FILL_RISE";
                proof["column"]=c;proof["fill_rise_y"]=y;return proof;
            }
        }
    }
    proof["no_sustained_fill_rise"]=true;proof["accepted"]=true;return proof;
}
QVector<RowSpan> rowSpans(const Signals& s,int top,int bottom,QJsonObject* tailEvidence=nullptr,QJsonObject* tailSelectionEvidence=nullptr){
    const auto bottoms=detectedBottoms(s,top,bottom);QVector<RowSpan> out;int previous=top-10;
    for(const int edge:bottoms){
        if(!out.isEmpty() &&edge-out.last().bottom<200)continue;
        const int candidate=findTop(s,std::max({top,previous+6,edge-275}),edge-240);
        if(candidate<0){if(out.isEmpty())out.append({top,edge,false,true});else return {};}
        else out.append({candidate,edge,true,true});
        previous=edge;
    }
    if(out.isEmpty())return out;
    QJsonArray observedRows;
    for(const auto& row:out){
        QJsonArray cells;
        for(int c=0;c<2;++c)cells.append(QJsonObject{{"column",c},{"top",row.topEdge?actualTop(s,c,row.top):top},
            {"bottom",row.bottomEdge?actualBottom(s,c,row.bottom):bottom},{"top_edge",row.topEdge},{"bottom_edge",row.bottomEdge}});
        observedRows.append(QJsonObject{{"cells",cells}});
    }
    const int tail=findTop(s,previous+6,std::min(bottom-4,previous+40),true,tailSelectionEvidence,observedRows);
    if(tail>=0 &&bottom-tail>12)out.append({tail,bottom,true,false});
    else if(tail<0 &&bottom-previous>12){
        // Run10 has a 16px tail matching already observed inter-row gaps in
        // both columns. A run09-style 35px unknown region remains rejected.
        const auto proof=shortTailGapEvidence(s,out,bottom);
        if(tailEvidence)*tailEvidence=proof;
        if(!proof["accepted"].toBool())return {};
    }
    return out;
}
int actualTop(const Signals& s,int column,int peak){
    // Weak translucent scene rises can precede the actual selected outline
    // (recorded y538 texture versus the y541..543 white stripe). Prefer only
    // a unique, narrow stripe measured across both ends and the central 400px
    // band. Its start stays inside the original search interval. This does
    // not assert selection: both long vertical sides and the unchanged >150
    // horizontal mean are still required later on the same source pixels.
    const int first=std::max(284,peak-3),last=peak+4;
    const auto white=[&](int y){const auto r=s.row(column,y);
        return r.wide>150 &&r.left>150 &&r.right>150 &&r.variance<400;};
    int stripe=-1,count=0;
    for(int y=first;y<=last;++y){
        if(!white(y) ||white(y-1))continue;
        int end=y;while(end<y+5 &&white(end+1))++end;
        const int width=end-y+1;
        const auto before=s.row(column,y-1),after=s.row(column,end+1);
        if(width>=2 &&width<=4 &&before.wide<150 &&after.wide<150
            &&s.row(column,y).wide-before.wide>35
            &&s.row(column,end).wide-after.wide>35){stripe=y;++count;}
    }
    if(count==1)return stripe;
    for(int y=std::max(284,peak-3);y<=peak+4;++y){
        const double rise=s.row(column,y).wide-s.row(column,y-1).wide;
        const double background=(s.row(column,y-2).wide-s.row(column,y-5).wide)/3;
        if(rise>std::max(.65,background+.65))return y;
    }
    return -1;
}
int actualBottom(const Signals& s,int column,int edge){
    int last=-1;
    for(int y=edge-4;y<=edge+3;++y)if(bottomCandidate(s,column,y,false)||narrowBottomRidge(s,column,y)
        ||corroboratedSceneRelativeEdge(s,column,y))last=y;
    return last;
}
struct Side {int x=-1;double score=0,support=0,brightness=0;QJsonArray profile;};
Side verticalSide(const FrameEnvelope& frame,int nominal,int top,int bottom,bool left){
    Side result;
    const int start=top+std::min(10,(bottom-top)/5),end=bottom-std::min(50,(bottom-top)/4);
    if(end-start<10)return result;
    struct Point {int x=0;double excess=0,support=0,brightness=0;};
    QVector<Point> points;
    for(int x=nominal-7;x<=nominal+7;++x){
        double total=0,brightness=0;int support=0,count=0;
        for(int y=start;y<=end;y+=3){
            // Outside samples model the scene's smooth local x-gradient, not
            // the bright selection stroke or the four-level card fill. Find
            // the actual pixel where persistent excess begins/ends; a wide
            // difference kernel's center is NOT a card boundary.
            // On the first column's right edge the inter-column gap is
            // narrow: never sample the selected neighbor's x996 white stroke.
            const double near=horizontal(frame,left?nominal-6:nominal+3,y,3).mean;
            const double far=horizontal(frame,left?nominal-9:nominal+6,y,3).mean;
            const double slope=(left?near-far:far-near)/3;
            const double center=nominal+(left?-5.0:4.0);
            const double pixel=luminance(frame,x,y),delta=pixel-(near+slope*(x-center));
            total+=std::clamp(delta,-20.0,255.0);support+=delta>.65;brightness+=pixel;++count;
        }
        const Point point{x,total/count,double(support)/count,brightness/count};points.append(point);
        result.profile.append(QJsonArray{x,rounded(point.excess),rounded(point.support),rounded(point.brightness)});
        result.score=std::max(result.score,point.excess);
    }
    const auto supported=[](const Point& p){return p.excess>.65 &&p.support>.65;};
    if(left){
        for(int i=0;i+2<points.size();++i)if(supported(points[i]) &&supported(points[i+1]) &&supported(points[i+2])){
            result.x=points[i].x;result.support=points[i].support;result.brightness=points[i].brightness;break;}
    }else{
        for(int i=points.size()-1;i>=2;--i)if(supported(points[i]) &&supported(points[i-1]) &&supported(points[i-2])){
            result.x=points[i].x;result.support=points[i].support;result.brightness=points[i].brightness;break;}
    }
    if(result.score<1.5)result.x=-1;
    return result;
}
double borderBrightness(const FrameEnvelope& frame,const QRect& box){
    double total=0;int count=0;
    for(int y=box.top();y<=box.bottom();++y)for(int x=box.left();x<=box.right();++x){total+=luminance(frame,x,y);++count;}
    return count?total/count:0;
}
bool integerRectangle(const QJsonValue& value,QRect& result){
    if(!value.isArray())return false;
    const auto a=value.toArray();if(a.size()!=4)return false;
    for(const auto& v:a)if(!v.isDouble() ||!std::isfinite(v.toDouble())
        ||v.toDouble()!=std::floor(v.toDouble()) ||v.toDouble()<0 ||v.toDouble()>2560)return false;
    if(a[2].toInt()<=0 ||a[3].toInt()<=0)return false;
    result=QRect(a[0].toInt(),a[1].toInt(),a[2].toInt(),a[3].toInt());return true;
}
bool sourceHash(const QJsonValue& value){
    if(!value.isString() ||value.toString().size()!=64)return false;
    for(const auto c:value.toString())if(!((c>='0' &&c<='9') ||(c>='a' &&c<='f') ||(c>='A' &&c<='F')))return false;
    return true;
}
QVector<int> receiptHorizontalEdges(const Signals& s,int column,int center,bool top){
    // The reference bounds only limit a search. Every returned coordinate is
    // a current-frame observed rise or ridge; a missing edge stays missing.
    QVector<int> values;
    for(int peak=center-6;peak<=center+6;++peak){
        const int edge=top?actualTop(s,column,peak):actualBottom(s,column,peak);
        if(edge>=center-6 &&edge<=center+6 &&!values.contains(edge))values.append(edge);
    }
    std::sort(values.begin(),values.end());
    QVector<int> clustered;
    int previous=-100;
    for(const int value:values){
        if(value-previous>3)clustered.append(value);
        else if(!top)clustered.last()=value;
        previous=value;
    }
    return clustered;
}
QJsonObject receiptMeasuredCard(const FrameEnvelope& frame,int leftHint,int rightHint,int top,int bottom){
    const auto left=verticalSide(frame,leftHint,top,bottom,true),right=verticalSide(frame,rightHint,top,bottom,false);
    if(left.x<0 ||right.x<0 ||right.x-left.x<840 ||right.x-left.x>885)return {};
    const int width=right.x-left.x+1,height=bottom-top+1;
    const double topWhite=borderBrightness(frame,QRect(left.x+280,top,400,std::min(3,height)));
    const double bottomWhite=borderBrightness(frame,QRect(left.x+280,bottom-2,400,3));
    const bool selected=left.score>35 &&right.score>35 &&(topWhite>150 ||bottomWhite>150);
    return {{"id",QStringLiteral("edge:%1:%2:%3:%4").arg(left.x).arg(top).arg(right.x).arg(bottom)},
        {"bounds",rectangle(left.x,top,width,height)},{"bounds_basis","observed"},
        {"edges",QJsonObject{{"top",true},{"bottom",true},{"left",true},{"right",true}}},
        {"selected",selected},{"fields_bounds",rectangle(left.x+1,bottom-42,width-2,42)},
        {"condition_bounds",rectangle(left.x+1,bottom-42,115,42)},
        {"price_bounds",rectangle(right.x-149,bottom-42,149,42)},
        {"edge_measurements",QJsonObject{{"left_contrast",left.score},{"right_contrast",right.score},
            {"left_support",left.support},{"right_support",right.support},{"top_mean",topWhite},{"bottom_mean",bottomWhite},
            {"left_profile",left.profile},{"right_profile",right.profile}}}};
}
QJsonObject receiptSelectedCard(const FrameEnvelope& frame,const QJsonObject& profile,const QJsonObject& reference){
    QJsonObject proof{{"schema","collection-selected-card-proof-v1"},{"scope","receipt_only"},
        {"proven",false},{"actions_enabled",false},{"unique_selection_proven",false},
        {"local_selection_conflict_checked",false},{"same_frame",profile["same_frame"]},
        {"frame_id",frame.frameId},{"frame_sha256",profile["frame_sha256"]},
        {"prior_card_reference",reference},{"image_file_writes",0}};
    if(profile.contains("error")){proof["error"]=profile["error"];return proof;}
    QRect prior,fields;
    if(reference["frame_id"].toString().isEmpty() ||!sourceHash(reference["frame_sha256"])
        ||reference["card_id"].toString().isEmpty() ||!integerRectangle(reference["bounds"],prior)
        ||!integerRectangle(reference["fields_bounds"],fields)
        ||prior.width()<841 ||prior.width()>886 ||prior.height()<240 ||prior.height()>275
        ||prior.top()<294 ||prior.bottom()>1287
        ||fields!=QRect(prior.left()+1,prior.bottom()-42,prior.width()-2,42)){
        proof["error"]="E_COLLECTION_RECEIPT_REFERENCE";return proof;
    }
    const int column=prior.center().x()<997?0:1;
    const int nominalLeft=column?997:120,nominalRight=column?1861:984;
    if(std::abs(prior.left()-nominalLeft)>8 ||std::abs(prior.right()-nominalRight)>8){
        proof["error"]="E_COLLECTION_RECEIPT_REFERENCE";return proof;
    }
    const auto s=readSignals(profile);
    if(!s.valid){proof["error"]="E_COLLECTION_PROFILE_SIGNALS";return proof;}
    const auto tops=receiptHorizontalEdges(s,column,prior.top(),true);
    const auto bottoms=receiptHorizontalEdges(s,column,prior.bottom(),false);
    QJsonArray topEvidence,bottomEvidence;
    for(const auto y:tops)topEvidence.append(y);
    for(const auto y:bottoms)bottomEvidence.append(y);
    proof["horizontal_edge_candidates"]=QJsonObject{{"tops",topEvidence},{"bottoms",bottomEvidence}};
    if(tops.isEmpty() ||bottoms.isEmpty()){
        proof["error"]="E_COLLECTION_RECEIPT_HORIZONTAL_EDGE";return proof;
    }
    QJsonArray candidates;
    bool hasSidePair=false;
    for(const int top:tops)for(const int bottom:bottoms){
        if(bottom-top+1<240 ||bottom-top+1>275)continue;
        const auto card=receiptMeasuredCard(frame,prior.left(),prior.right(),top,bottom);
        hasSidePair=hasSidePair ||!card.isEmpty();
        if(card["selected"].toBool())candidates.append(card);
    }
    if(candidates.size()!=1){
        proof["candidate_count"]=candidates.size();
        proof["error"]=candidates.size()>1?"E_COLLECTION_RECEIPT_GEOMETRY_AMBIGUOUS":
            (hasSidePair?"E_COLLECTION_RECEIPT_NOT_SELECTED":"E_COLLECTION_RECEIPT_VERTICAL_EDGE");
        return proof;
    }
    // The local conflict check covers the peer card in the same horizontal
    // band, not an unseen tail or the whole viewport. A selected peer invalidates
    // this receipt proof; an unresolved peer is retained as uncertainty rather
    // than silently promoted to a claim of global uniqueness.
    const int peer=1-column;
    const auto peerTops=receiptHorizontalEdges(s,peer,prior.top(),true);
    const auto peerBottoms=receiptHorizontalEdges(s,peer,prior.bottom(),false);
    const int peerLeft=peer?997:120,peerRight=peer?1861:984;
    int peerMeasured=0;
    for(const int top:peerTops)for(const int bottom:peerBottoms){
        if(bottom-top+1<240 ||bottom-top+1>275)continue;
        const auto peerCard=receiptMeasuredCard(frame,peerLeft,peerRight,top,bottom);
        if(peerCard.isEmpty())continue;
        ++peerMeasured;
        if(peerCard["selected"].toBool()){
            proof["local_conflicting_card"]=peerCard;proof["local_selection_conflict_checked"]=true;
            proof["error"]="E_COLLECTION_RECEIPT_SELECTION_CONFLICT";return proof;
        }
    }
    proof["local_selection_conflict_checked"]=true;
    proof["local_selection_check_scope"]="reference_horizontal_band_and_peer_column_only";
    proof["peer_geometry_resolved"]=peerMeasured==1;
    proof["unresolved_regions_may_contain_selection"]=true;
    proof["card"]=candidates[0];proof["proven"]=true;return proof;
}
}

QJsonObject collectionLayoutEdgeCandidates(const QJsonObject& profile){
    QJsonObject out{{"schema","collection-edge-candidates-v1"},{"complete",false},{"actions_enabled",false},
        {"frame_id",profile["frame_id"]},{"frame_sha256",profile["frame_sha256"]}};
    const auto s=readSignals(profile);if(!s.valid){out["error"]="E_COLLECTION_PROFILE_SIGNALS";return out;}
    const auto bar=scrollbar(s);out["scrollbar_candidate"]=bar;
    if(bar.contains("error")){out["error"]=bar["error"];return out;}
    const auto track=bar["track_bounds"].toArray();const int top=track[1].toInt(),bottom=top+track[3].toInt()-1;
    QJsonArray rows;QJsonObject tailEvidence,tailSelectionEvidence;
    for(const auto& row:rowSpans(s,top,bottom,&tailEvidence,&tailSelectionEvidence)){
        QJsonArray cells;
        for(int c=0;c<2;++c)cells.append(QJsonObject{{"column",c},{"top",row.topEdge?actualTop(s,c,row.top):top},
            {"bottom",row.bottomEdge?actualBottom(s,c,row.bottom):bottom},{"top_edge",row.topEdge},{"bottom_edge",row.bottomEdge}});
        rows.append(QJsonObject{{"cells",cells}});
    }
    if(!tailEvidence.isEmpty())out["tail_evidence"]=tailEvidence;
    if(!tailSelectionEvidence.isEmpty())out["tail_top_selection"]=tailSelectionEvidence;
    out["rows"]=rows;return out;
}

QJsonObject detectCollectionLayout(const FrameEnvelope& frame,const QJsonObject& receiptReference){
    QJsonObject out{{"schema","collection-layout-v1"},{"detector","visible_edges"},{"complete",false},
        {"same_frame",false},{"frame_id",frame.frameId},{"image_file_writes",0},{"cards",QJsonArray{}}};
    const auto profile=collectionLayoutProfile(frame);
    if(!receiptReference.isEmpty())out["selected_card_proof"]=receiptSelectedCard(frame,profile,receiptReference);
    if(profile.contains("error")){out["error"]=profile["error"];return out;}
    out["same_frame"]=true;out["frame_sha256"]=profile["frame_sha256"];out["viewport"]=QJsonArray{frame.width,frame.height};
    const auto evidence=collectionLayoutEdgeCandidates(profile);out["edge_evidence"]=evidence;
    if(evidence.contains("error")){out["error"]=evidence["error"];return out;}
    const auto bar=evidence["scrollbar_candidate"].toObject();out["scrollbar"]=bar;
    const auto track=bar["track_bounds"].toArray();const int viewportTop=track[1].toInt(),viewportBottom=viewportTop+track[3].toInt()-1;
    // The scrollbar supplies the vertical search interval, with four pixels
    // reserved for observed selection-border expansion above its top. Actual
    // card edges are still independently measured, never filled from this box.
    out["listing_viewport"]=rectangle(108,viewportTop-4,1765,viewportBottom-viewportTop+5);
    const auto rows=evidence["rows"].toArray();
    if(rows.isEmpty()){out["error"]="E_COLLECTION_ROW_EDGES";return out;}
    QJsonArray cards;int selectedCount=0,fullCount=0;
    for(const auto& rowValue:rows)for(const auto& value:rowValue.toObject()["cells"].toArray()){
        const auto cell=value.toObject();const int column=cell["column"].toInt();
        const int top=cell["top"].toInt(),bottom=cell["bottom"].toInt();
        const bool topEdge=cell["top_edge"].toBool(),bottomEdge=cell["bottom_edge"].toBool();
        if(top<viewportTop-4 ||bottom>viewportBottom ||bottom<=top){out["error"]="E_COLLECTION_CARD_HORIZONTAL_EDGE";return out;}
        if((!topEdge ||!bottomEdge) &&bottom-top+1>275){
            out["error"]="E_COLLECTION_PARTIAL_SPANS_ROWS";
            out["edge_failure"]=QJsonObject{{"column",column},{"top",top},{"bottom",bottom},{"height",bottom-top+1}};
            return out;
        }
        const int height=bottom-top+1;
        const bool full=topEdge &&bottomEdge &&height>=240 &&height<=275;
        if(topEdge &&bottomEdge &&!full){out["error"]="E_COLLECTION_CARD_HEIGHT";return out;}
        const int searchLeft=column==0?120:997,searchRight=column==0?984:1861;
        const auto left=verticalSide(frame,searchLeft,top,bottom,true),right=verticalSide(frame,searchRight,top,bottom,false);
        const bool sidePair=left.x>=0 &&right.x>=0 &&right.x-left.x>=840 &&right.x-left.x<=885;
        if(full &&!sidePair){
            out["edge_failure"]=QJsonObject{{"column",column},{"top",top},{"bottom",bottom},{"left_x",left.x},{"right_x",right.x},
                {"left_score",left.score},{"right_score",right.score},{"left_support",left.support},{"right_support",right.support},
                {"left_profile",left.profile},{"right_profile",right.profile}};
            out["error"]="E_COLLECTION_CARD_VERTICAL_EDGE";return out;}
        // A clipped, non-selectable row may have a side obscured by the
        // bottom haze. Preserve its measured y interval as a search-region ROI
        // and explicitly mark unproven sides false; never turn it into a card
        // lease, infer its fields, or erase the complete rows above it.
        const bool leftEdge=sidePair ||left.x>=0,rightEdge=sidePair ||right.x>=0;
        const int leftX=left.x>=0?left.x:(column==0?108:991);
        const int rightX=right.x>=0?right.x:(column==0?990:1872);
        if(rightX<=leftX){out["error"]="E_COLLECTION_PARTIAL_SEARCH_REGION";return out;}
        const int width=rightX-leftX+1;
        const QString id=QStringLiteral("edge:%1:%2:%3:%4").arg(leftX).arg(top).arg(rightX).arg(bottom);
        double topWhite=0,bottomWhite=0;
        if(topEdge)topWhite=borderBrightness(frame,QRect(searchLeft+280,top,400,std::min(3,height)));
        if(bottomEdge)bottomWhite=borderBrightness(frame,QRect(searchLeft+280,bottom-2,400,3));
        // White outline on both long vertical sides plus a horizontal border
        // identifies selection; scene brightness/hover fill alone cannot.
        const bool selected=left.score>35 &&right.score>35 &&(topWhite>150 ||bottomWhite>150);
        selectedCount+=selected;fullCount+=full;
        QJsonObject card{{"id",id},{"bounds",rectangle(leftX,top,width,height)},
            {"bounds_basis",sidePair?"observed":"clipped_search_region"},
            {"edges",QJsonObject{{"top",topEdge},{"bottom",bottomEdge},{"left",leftEdge},{"right",rightEdge}}},
            {"selected",selected},{"fields_bounds",QJsonValue::Null},
            {"edge_measurements",QJsonObject{{"left_contrast",left.score},{"right_contrast",right.score},
                {"left_support",left.support},{"right_support",right.support},{"top_mean",topWhite},{"bottom_mean",bottomWhite},
                {"left_profile",left.profile},{"right_profile",right.profile}}}};
        if(full){const int fy=bottom-42,fw=width-2;
            card["fields_bounds"]=rectangle(leftX+1,fy,fw,42);
            card["condition_bounds"]=rectangle(leftX+1,fy,115,42);
            card["price_bounds"]=rectangle(rightX-149,fy,149,42);
        }
        cards.append(card);
    }
    if(selectedCount>1){out["error"]="E_COLLECTION_SELECTION_AMBIGUOUS";return out;}
    if(!fullCount){out["error"]="E_COLLECTION_NO_COMPLETE_CARD";return out;}
    out["cards"]=cards;out["complete"]=true;out["page_exhausted"]=false;return out;
}
} // namespace relink::vision
