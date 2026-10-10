#include "application/vision/collection_layout.h"
#include "application/vision/collection_ocr_fields.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <cstdio>
#include <QFile>
#include <algorithm>

// Narrow numerical detector detail, intentionally not a business/public API.
namespace relink::vision::detail {
QJsonObject selectMeasuredTailTop(const QJsonArray& observedRows,const QJsonArray& measuredCandidates);
}

int main(int argc,char** argv){
    QCoreApplication app(argc,argv);int checks=0,failures=0;
    const auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL %s\n",name);}};
    const auto priceWords=[](std::initializer_list<QString> texts){QJsonArray words;for(const auto& text:texts)words.append(QJsonObject{{"text",text}});return words;};
    check(!relink::vision::collectionPriceWordsAreNumeric({}),"empty price requires same-frame re-read");
    check(!relink::vision::collectionPriceWordsAreNumeric(priceWords({"goo"})),"actual OCR letters are never substituted as 900");
    check(!relink::vision::collectionPriceWordsAreNumeric(priceWords({"9","oo"})),"partial numeric token does not hide illegal price text");
    check(!relink::vision::collectionPriceWordsAreNumeric(priceWords({","})),"punctuation without any digit is not numeric text");
    check(relink::vision::collectionPriceWordsAreNumeric(priceWords({"230"})),"numeric primary price does not need extra OCR");
    check(relink::vision::collectionPriceWordsAreNumeric(priceWords({"1,200"})),"numeric separators remain raw parser input");
    const auto rawWide=priceWords({QString(QChar(0xff19))+QChar(0xff10)+QChar(0xff10)});const auto rawWideBefore=rawWide;
    check(relink::vision::collectionPriceWordsAreNumeric(rawWide) &&rawWide==rawWideBefore,"lexical check does not mutate OCR words");
    const QRect conditionBounds(120,1072,115,42);
    // Exact recorded native word boxes from collection_local_hotpath/run05,
    // step 000086, third frame. These are OCR evidence, not saved game pixels.
    const QJsonArray borderCondition{
        QJsonObject{{"text","|"},{"x",120.0},{"y",1093.5945527357849},{"width",0.46815072616725606},{"height",19.0065443949188}},
        QJsonObject{{"text",QStringLiteral("成")},{"x",127.1728750168037},{"y",1085.7801821536802},{"width",20.482673358676713},{"height",20.482673358676717}},
        QJsonObject{{"text",QStringLiteral("色")},{"x",148.70310531432813},{"y",1085.8056232390718},{"width",19.45853969074287},{"height",19.458539690742874}},
        QJsonObject{{"text","C"},{"x",169.72126877788563},{"y",1086.818549725168},{"width",13.423682484826799},{"height",17.812394387155184}}};
    const auto borderConditionBefore=borderCondition;
    check(!relink::vision::collectionConditionWordsAreLiteral(borderCondition),"recorded border token remains an invalid condition, not discarded text");
    check(relink::vision::collectionConditionRefinementRegion(borderCondition,conditionBounds)==QRect(122,1072,113,42),"recorded thin left border proposes two-pixel inset OCR on real same-frame pixels");
    check(borderCondition==borderConditionBefore,"refinement planning leaves all original words unchanged");
    const auto changedCondition=[&](int index,const char* name,const QJsonValue& value){
        auto result=borderCondition;auto word=result[index].toObject();word[name]=value;result[index]=word;return result;};
    for(const auto* text:{"I","l","1"})
        check(relink::vision::collectionConditionRefinementRegion(changedCondition(0,"text",text),conditionBounds).isEmpty(),"letters and digits do not become removable border strokes");
    check(relink::vision::collectionConditionRefinementRegion(changedCondition(0,"x",122),conditionBounds).isEmpty(),"internal vertical bar cannot authorize border crop");
    check(relink::vision::collectionConditionRefinementRegion(changedCondition(0,"width",2),conditionBounds).isEmpty(),"wide token is not the recorded thin edge line");
    check(relink::vision::collectionConditionRefinementRegion(changedCondition(1,"x",123),conditionBounds).isEmpty(),"refinement never crops next to a possibly touched glyph");
    check(relink::vision::collectionConditionRefinementRegion(changedCondition(3,"text","5"),conditionBounds).isEmpty(),"invalid grade is not inferred during retry planning");
    auto doubleBorder=borderCondition;doubleBorder.append(borderCondition.first());
    check(relink::vision::collectionConditionRefinementRegion(doubleBorder,conditionBounds).isEmpty(),"multiple strokes are not stripped as one measured edge");
    check(relink::vision::collectionConditionRefinementRegion(changedCondition(1,"y",1060),conditionBounds).isEmpty(),"out-of-region words cannot plan an inset read");
    check(relink::vision::collectionConditionRefinementRegion({},conditionBounds).isEmpty(),"empty condition does not invent a crop or grade");
    for(const auto grade:QStringLiteral("sSaAbBcC")){
        const QJsonArray literal{QJsonObject{{"text",QStringLiteral("成色")+grade},{"x",127},{"y",1085},{"width",58},{"height",21}}};
        check(relink::vision::collectionConditionWordsAreLiteral(literal),"literal observed ASCII grade remains exact and case-compatible");
        check(relink::vision::collectionConditionRefinementRegion(literal,conditionBounds).isEmpty(),"successful primary condition does not request another OCR");
    }
    const QJsonArray trailingNewline{QJsonObject{{"text",QStringLiteral("成色C\n")},{"x",127},{"y",1085},{"width",58},{"height",21}}};
    check(!relink::vision::collectionConditionWordsAreLiteral(trailingNewline),"literal condition predicate rejects trailing newline rather than dollar-anchor match");
    using relink::runtime::observation::FrameEnvelope;
    FrameEnvelope frame;frame.frameId="synthetic:layout-profile";frame.width=2560;frame.height=1440;
    frame.dpiX=frame.dpiY=144;frame.strideBytes=frame.width*4;frame.validBytes=qint64(frame.strideBytes)*frame.height;
    frame.pixels=QByteArray(frame.validBytes,char(80));
    const auto before=frame.pixels;
    const auto profile=relink::vision::collectionLayoutProfile(frame);
    check(profile["same_frame"]==true && profile["frame_id"]==frame.frameId,"profile is bound to input frame");
    check(profile["frame_sha256"].toString().size()==64,"profile has source hash");
    check(profile["complete"]==false && profile["layout_proven"]==false && profile["actions_enabled"]==false,"measurements never establish actionable layout");
    const auto bands=profile["row_bands"].toArray();
    check(bands.size()==2,"two historical candidate search columns");
    for(const auto& band:bands){const auto rows=band.toObject()["rows"].toArray();
        check(rows.size()==1021,"every requested row has measured features");
        const auto row=rows[20].toArray();
        check(row[0]==300 && row[1]==80 && row[2]==0 && row[3]==80 && row[4]==80 && row[5]==80 && row[6]==0 && row[7]==0,"uniform frame gives exact mean variance and gradient");}
    const auto vertical=profile["right_edge_vertical_profiles"].toArray();
    check(vertical.size()==43 && vertical[0].toObject()["x"]==1858 && vertical.last().toObject()["x"]==1900,"right edge covers observed search range");
    check(vertical[0].toObject()["means"].toArray().size()==1021 && vertical[0].toObject()["minimum"]==80,"vertical features are measured in memory");
    check(frame.pixels==before,"profiling does not mutate source pixels");
    check(!profile.contains("cards") && !profile.contains("scrollbar") && !profile.contains("preview_png_base64"),"profile never fabricates card or scrollbar geometry");
    check(relink::vision::detectCollectionLayout(frame)["complete"]==false,"flat image cannot become actionable");
    const auto fill=[&](int x,int y,int width,int height,int value){
        for(int yy=y;yy<y+height;++yy)for(int xx=x;xx<x+width;++xx){auto* pixel=reinterpret_cast<unsigned char*>(frame.pixels.data())+qint64(yy)*frame.strideBytes+xx*4;
            pixel[0]=pixel[1]=pixel[2]=static_cast<unsigned char>(value);pixel[3]=255;}
    };
    fill(1877,304,3,899,104);fill(1877,304,3,62,180);
    const int tops[]={304,580,855,1130},bottoms[]={567,840,1115,1202};
    for(int row=0;row<4;++row)for(int column=0;column<2;++column){const int x=column?997:120,y=tops[row],bottom=bottoms[row];
        fill(x,y,865,bottom-y+1,84);fill(x,y,865,1,81);fill(x,y+1,865,1,82);
        if(row<3){fill(x,bottom-1,865,2,99);fill(x,bottom-1,3,2,180);fill(x+862,bottom-1,3,2,160);}
    }
    fill(120,304,865,2,245);fill(120,566,865,2,245);fill(120,304,2,264,245);fill(983,304,2,264,245);
    const auto layout=relink::vision::detectCollectionLayout(frame);
    if(!layout["complete"].toBool())std::fprintf(stderr,"synthetic detector: %s\n",QJsonDocument(layout).toJson(QJsonDocument::Compact).constData());
    check(layout["complete"]==true,"observed synthetic edges produce full enumeration");
    check(layout["cards"].toArray().size()==8,"complete enumeration includes two clipped tail cards");
    int selected=0,full=0,partial=0;
    for(const auto& value:layout["cards"].toArray()){const auto card=value.toObject();selected+=card["selected"].toBool();
        if(card["fields_bounds"].isNull()){++partial;check(!card.contains("condition_bounds")&&!card.contains("price_bounds"),"clipped fields never invented");}
        else {++full;check(card.contains("condition_bounds")&&card.contains("price_bounds"),"full fields provide separate measured-anchor regions");}}
    check(full==6 &&partial==2,"six full and two partial synthetic cards");
    check(selected==1,"selection comes from actual white border");
    const auto bar=layout["scrollbar"].toObject();
    check(bar["track_bounds"].toArray().at(1)==304 &&bar["track_bounds"].toArray().at(3)==899,"track inferred from local contrast");
    check(bar["thumb_bounds"].toArray().at(1)==304 &&bar["thumb_bounds"].toArray().at(3)==62,"thumb measured from stronger local contrast");
    // A prior card only limits a same-frame pixel search. Receipt geometry is
    // separate from complete viewport enumeration and never licenses a click.
    const auto selectedPixels=frame.pixels;
    // A narrow, measured white top must win over a weak scene rise three
    // pixels before it. The bottom is deliberately not white, so selection
    // cannot accidentally pass using the other horizontal edge.
    fill(120,304,865,264,84);fill(120,304,865,1,81);fill(120,305,865,1,82);
    fill(120,566,865,2,99);fill(120,566,3,2,180);fill(982,566,3,2,160);
    fill(120,577,865,1,82);fill(120,578,865,1,83);
    fill(120,580,865,2,245);fill(120,580,2,261,245);fill(983,580,2,261,245);
    const auto weakRisePixels=frame.pixels;
    const auto weakRiseLayout=relink::vision::detectCollectionLayout(frame);
    const auto weakRiseCards=weakRiseLayout["cards"].toArray();
    check(weakRiseLayout["complete"]==true &&weakRiseCards.size()==8,
        "weak pre-border scene texture does not prevent full layout enumeration");
    if(weakRiseCards.size()==8){const auto target=weakRiseCards[2].toObject();
        check(target["bounds"].toArray()[1]==580 &&target["selected"]==true,
            "actual two-row white top wins over unrelated earlier weak rise");
        check(target["edge_measurements"].toObject()["top_mean"].toDouble()>150
            &&target["edge_measurements"].toObject()["bottom_mean"].toDouble()<150,
            "selection uses original unchanged brightness gate on the real top stripe");
    }
    const auto selectedCountIn=[](const QJsonObject& value){int count=0;
        for(const auto& card:value["cards"].toArray())count+=card.toObject()["selected"].toBool();return count;};
    fill(108,582,24,257,80);
    check(selectedCountIn(relink::vision::detectCollectionLayout(frame))==0,
        "horizontal stripe without its long left outline does not prove selected");
    frame.pixels=weakRisePixels;fill(977,582,20,257,80);
    check(selectedCountIn(relink::vision::detectCollectionLayout(frame))==0,
        "horizontal stripe without its long right outline does not prove selected");
    frame.pixels=weakRisePixels;fill(122,581,861,1,84);
    check(selectedCountIn(relink::vision::detectCollectionLayout(frame))==0,
        "isolated one-row white impulse does not gain stripe priority or pass the original mean gate");
    frame.pixels=weakRisePixels;fill(122,580,861,2,84);fill(122,610,861,2,245);
    check(selectedCountIn(relink::vision::detectCollectionLayout(frame))==0,
        "bright interior texture outside the observed top search does not become a selected outline");
    frame.pixels=weakRisePixels;fill(122,580,861,12,245);
    check(selectedCountIn(relink::vision::detectCollectionLayout(frame))==0,
        "wide bright hover fill does not receive narrow stripe priority");
    frame.pixels=weakRisePixels;
    fill(997,580,865,2,245);fill(997,580,2,261,245);fill(1860,580,2,261,245);
    const auto ambiguousStripe=relink::vision::detectCollectionLayout(frame);
    check(ambiguousStripe["complete"]==false &&ambiguousStripe["error"]=="E_COLLECTION_SELECTION_AMBIGUOUS",
        "two actual white outlined cards remain an ambiguous selection");
    frame.pixels=selectedPixels;
    const auto priorSelected=layout["cards"].toArray()[0].toObject();
    const QJsonObject receiptReference{{"frame_id","synthetic:before-receipt"},
        {"frame_sha256",layout["frame_sha256"]},{"card_id",priorSelected["id"]},
        {"bounds",priorSelected["bounds"]},{"fields_bounds",priorSelected["fields_bounds"]}};
    check(!layout.contains("selected_card_proof"),"ordinary enumeration does not invent a receipt reference");
    frame.frameId="synthetic:receipt-current";
    const auto receiptLayout=relink::vision::detectCollectionLayout(frame,receiptReference);
    const auto receiptProof=receiptLayout["selected_card_proof"].toObject();
    check(receiptProof["proven"]==true &&receiptProof["scope"]=="receipt_only","selected card has independent receipt-only geometry");
    check(receiptProof["card"].toObject()["bounds"]==priorSelected["bounds"]
        &&receiptProof["card"].toObject()["fields_bounds"]==priorSelected["fields_bounds"],"receipt fields use the currently measured four edges");
    check(receiptProof["same_frame"]==true &&receiptProof["frame_id"]==frame.frameId
        &&receiptProof["frame_sha256"]==receiptLayout["frame_sha256"]
        &&receiptProof["prior_card_reference"]==receiptReference,"receipt proof retains old reference and binds current frame hash");
    check(receiptProof["actions_enabled"]==false &&receiptProof["unique_selection_proven"]==false
        &&receiptProof["local_selection_conflict_checked"]==true,"local receipt geometry never claims global uniqueness or enables another action");
    fill(120,1114,865,2,84);fill(997,1114,865,2,84);
    const auto receiptMissingTail=relink::vision::detectCollectionLayout(frame,receiptReference);
    check(receiptMissingTail["complete"]==false &&receiptMissingTail["error"]=="E_COLLECTION_PARTIAL_SPANS_ROWS"
        &&receiptMissingTail["selected_card_proof"].toObject()["proven"]==true,"missing unrelated lower separator preserves local receipt proof but blocks enumeration");
    check(!receiptMissingTail["page_exhausted"].toBool()
        &&receiptMissingTail["cards"].toArray().isEmpty(),"receipt proof never promotes unenumerated tail into observed actionable cards");
    frame.pixels=selectedPixels;
    fill(120,1130,865,73,80);fill(997,1130,865,73,80);
    const auto receiptUnknownTail=relink::vision::detectCollectionLayout(frame,receiptReference);
    check(receiptUnknownTail["complete"]==false
        &&receiptUnknownTail["selected_card_proof"].toObject()["proven"]==true,"unresolved tail does not discard independent selected-card receipt evidence");
    frame.pixels=selectedPixels;
    fill(120,566,865,2,84);
    const auto missingSelectedBottom=relink::vision::detectCollectionLayout(frame,receiptReference)["selected_card_proof"].toObject();
    check(missingSelectedBottom["proven"]==false &&missingSelectedBottom["error"]=="E_COLLECTION_RECEIPT_HORIZONTAL_EDGE","prior bottom coordinate never replaces a missing current bottom edge");
    frame.pixels=selectedPixels;
    fill(120,288,865,24,84);
    const auto missingSelectedTop=relink::vision::detectCollectionLayout(frame,receiptReference)["selected_card_proof"].toObject();
    check(missingSelectedTop["proven"]==false &&missingSelectedTop["error"]=="E_COLLECTION_RECEIPT_HORIZONTAL_EDGE","prior top coordinate never replaces a missing current top edge");
    frame.pixels=selectedPixels;
    fill(108,308,25,256,80);
    const auto missingSelectedSide=relink::vision::detectCollectionLayout(frame,receiptReference)["selected_card_proof"].toObject();
    check(missingSelectedSide["proven"]==false &&missingSelectedSide["error"]=="E_COLLECTION_RECEIPT_VERTICAL_EDGE","missing current side cannot become receipt geometry");
    frame.pixels=selectedPixels;
    fill(997,304,865,2,245);fill(997,566,865,2,245);fill(997,304,2,264,245);fill(1860,304,2,264,245);
    const auto twoSelected=relink::vision::detectCollectionLayout(frame,receiptReference)["selected_card_proof"].toObject();
    check(twoSelected["proven"]==false &&twoSelected["error"]=="E_COLLECTION_RECEIPT_SELECTION_CONFLICT","nearby independently measured second selected outline rejects receipt proof");
    frame.pixels=selectedPixels;
    // Move real pixels rather than editing the old reference. The output must
    // follow their new edges and must not copy the old coordinates.
    fill(120,304,865,264,80);
    fill(121,306,865,264,84);fill(121,306,865,2,245);fill(121,568,865,2,245);
    fill(121,306,2,264,245);fill(984,306,2,264,245);
    const auto shifted=relink::vision::detectCollectionLayout(frame,receiptReference)["selected_card_proof"].toObject();
    check(shifted["proven"]==true &&shifted["card"].toObject()["bounds"].toArray()==QJsonArray{121,306,865,264},"receipt proof remeasures shifted original pixels rather than reusing prior bounds");
    check(shifted["card"].toObject()["fields_bounds"].toArray()==QJsonArray{122,527,863,42},"shifted fields remain anchored to current bottom edge");
    auto badReference=receiptReference;badReference["bounds"]=QJsonArray{120,304,865.5,264};
    check(relink::vision::detectCollectionLayout(frame,badReference)["selected_card_proof"].toObject()["error"]=="E_COLLECTION_RECEIPT_REFERENCE","fractional reference dimensions rejected instead of truncating");
    badReference=receiptReference;badReference["frame_sha256"]="not-a-source-hash";
    check(relink::vision::detectCollectionLayout(frame,badReference)["selected_card_proof"].toObject()["error"]=="E_COLLECTION_RECEIPT_REFERENCE","receipt reference requires actual source identity");
    badReference=receiptReference;badReference["fields_bounds"]=QJsonArray{120,0,863,42};
    check(relink::vision::detectCollectionLayout(frame,badReference)["selected_card_proof"].toObject()["error"]=="E_COLLECTION_RECEIPT_REFERENCE","unrelated old field rectangle rejected");
    frame.pixels=selectedPixels;frame.frameId="synthetic:layout-profile";
    fill(1881,1159,7,9,180);
    const auto brightFlank=relink::vision::detectCollectionLayout(frame);
    check(brightFlank["scrollbar"].toObject()["track_bounds"].toArray().at(3)==899,"single bright outside flank cannot truncate the measured track");
    fill(1881,1159,7,9,80);
    fill(1850,1130,22,73,80);
    const auto clippedSide=relink::vision::detectCollectionLayout(frame);
    check(clippedSide["complete"]==true &&clippedSide["cards"].toArray().size()==8,"unproven clipped side preserves complete cards above it");
    const auto clippedCard=clippedSide["cards"].toArray().last().toObject();
    check(clippedCard["bounds_basis"]=="clipped_search_region" &&clippedCard["edges"].toObject()["right"]==false &&clippedCard["fields_bounds"].isNull(),"clipped search region never claims an observed missing side or fields");
    fill(1850,1130,12,73,84);fill(1850,1130,12,1,81);fill(1850,1131,12,1,82);
    const auto beforeRamp=frame.pixels;
    for(int yy=1080;yy<=1140;++yy)for(int xx=0;xx<frame.width;++xx){auto* pixel=reinterpret_cast<unsigned char*>(frame.pixels.data())+qint64(yy)*frame.strideBytes+xx*4;
        for(int c=0;c<3;++c)pixel[c]=static_cast<unsigned char>(std::min(255,int(pixel[c])+yy-1080));}
    const auto rampLayout=relink::vision::detectCollectionLayout(frame);
    check(rampLayout["complete"]==true &&rampLayout["cards"].toArray().size()==8,"actual dual-column bottom ridges survive a reversing scene gradient");
    frame.pixels=beforeRamp;
    const auto selectedBounds=layout["cards"].toArray()[0].toObject()["bounds"].toArray();
    fill(120,304,865,264,84);fill(120,304,865,1,81);fill(120,305,865,1,82);
    fill(120,566,865,2,99);fill(120,566,3,2,180);fill(982,566,3,2,160);
    const auto unselectedLayout=relink::vision::detectCollectionLayout(frame);
    check(unselectedLayout["complete"]==true,"unselected synthetic layout still enumerates");
    const auto plainBounds=unselectedLayout["cards"].toArray()[0].toObject()["bounds"].toArray();
    check(std::abs(selectedBounds[0].toInt()-plainBounds[0].toInt())<=3
        &&std::abs(selectedBounds[0].toInt()+selectedBounds[2].toInt()-plainBounds[0].toInt()-plainBounds[2].toInt())<=3,"actual side pixels remain within observed selection expansion");
    check(selectedBounds[0].toInt()==120 &&plainBounds[0].toInt()==120,"side coordinate is real pixel not offset of a wide gradient kernel");
    fill(985,304,2,264,245);
    const auto leftNeighbor=relink::vision::detectCollectionLayout(frame);
    check(leftNeighbor["complete"]==true &&leftNeighbor["cards"].toArray()[1].toObject()["bounds"]==unselectedLayout["cards"].toArray()[1].toObject()["bounds"],"left outside background also excludes adjacent selected stroke");
    fill(985,304,2,264,80);
    fill(996,304,2,264,245);
    const auto brightNeighbor=relink::vision::detectCollectionLayout(frame);
    check(brightNeighbor["complete"]==true,"selected neighbor leaves original gray card enumerable");
    const auto neighborBounds=brightNeighbor["cards"].toArray()[0].toObject()["bounds"].toArray();
    check(neighborBounds==plainBounds,"right outside background samples exclude adjacent selected stroke");
    fill(1877,304,3,899,104);fill(1877,310,3,62,180);
    const auto scrolled=relink::vision::detectCollectionLayout(frame);
    check(scrolled["scrollbar"].toObject()["thumb_bounds"].toArray().at(1)==310,"scrollbar pixel motion is six not frame hash motion");
    // Removing the third bottom separator must not merge a complete row and
    // its partial successor into one fabricated tall partial card.
    fill(120,1114,865,2,84);fill(997,1114,865,2,84);
    const auto missedSeparator=relink::vision::detectCollectionLayout(frame);
    check(missedSeparator["complete"]==false &&missedSeparator["error"]=="E_COLLECTION_PARTIAL_SPANS_ROWS","missed separator blocks complete enumeration");
    fill(120,1114,865,2,99);fill(120,1114,3,2,180);fill(982,1114,3,2,160);
    fill(997,1114,865,2,99);fill(997,1114,3,2,180);fill(1859,1114,3,2,160);
    fill(120,1130,865,73,80);fill(997,1130,865,73,80);
    const auto unknownTail=relink::vision::detectCollectionLayout(frame);
    check(unknownTail["complete"]==false,"unexplained tail never becomes completed enumeration");
    // Independent synthetic pixels reproduce the probe01 ranking conflict:
    // a real paired tail fill begins at y983, while a stronger right-column
    // interior texture creates a y1010 candidate whose left refinement fails.
    // These are test pixels, not a stored or reconstructed game screenshot.
    frame.pixels=QByteArray(frame.validBytes,char(80));frame.frameId="synthetic:tail-interior-peak";
    fill(1877,304,3,899,104);fill(1877,312,3,70,180);
    for(int column=0;column<2;++column){const int x=column?997:120;
        const int starts[]={280,434,708,983},ends[]={418,693,970,1202};
        for(int row=0;row<4;++row){
            fill(x,starts[row],865,ends[row]-starts[row]+1,84);
            if(row>0){fill(x,starts[row],865,1,81);fill(x,starts[row]+1,865,1,82);}
            if(row<3){fill(x,ends[row]-1,865,2,99);fill(x,ends[row]-1,3,2,180);fill(x+862,ends[row]-1,3,2,160);}
        }
    }
    fill(1277,1013,400,190,90);
    const auto tailConflictPixels=frame.pixels;
    const auto tailConflict=relink::vision::detectCollectionLayout(frame);
    check(tailConflict["complete"]==true &&tailConflict["cards"].toArray().size()==8,
        "stronger one-column interior tail peak cannot erase genuine paired observed edge");
    const auto tailSelection=tailConflict["edge_evidence"].toObject()["tail_top_selection"].toObject();
    check(tailSelection["accepted"]==true &&tailSelection["selected_top_y"].toArray()==QJsonArray{983,983}
        &&tailSelection["thresholds_changed"]==false &&tailSelection["maximum_column_alignment_pixels"]==3,
        "tail ranking uses both actual same-frame edges without lowering thresholds or extending tolerance");
    bool rejectedInterior=false;
    for(const auto& value:tailSelection["candidates"].toArray()){const auto candidate=value.toObject();
        if(candidate["peak_y"]==1010)rejectedInterior=candidate["accepted_for_ranking"]==false
            &&candidate["rejection_reason"]=="left_top_unobserved"
            &&candidate["refined_top_y"].toArray()==QJsonArray{-1,1013};
    }
    check(rejectedInterior,"diagnostic records exact rejected interior peak and missing-column reason");
    // A stronger texture may be present in BOTH columns. The current-frame
    // complete rows supply the pitch; the real weaker edge supplies its own
    // pixels. No expected coordinate is filled in from the measured pitch.
    fill(400,1013,400,190,90);
    const auto pairedTexture=relink::vision::detectCollectionLayout(frame);
    const auto pairedSelection=pairedTexture["edge_evidence"].toObject()["tail_top_selection"].toObject();
    check(pairedTexture["complete"]==true &&pairedSelection["selected_top_y"].toArray()==QJsonArray{983,983},
        "strong paired interior texture cannot replace the independently observed pitch-consistent tail top");
    check(pairedSelection["same_frame_pitch"].toObject()["proven"]==true
        &&pairedSelection["same_frame_pitch"].toObject()["tolerance_pixels"]==3,
        "tail candidate filtering uses observed complete-row pitches with the unchanged three-pixel bound");
    bool pairedTextureRejected=false;
    for(const auto& value:pairedSelection["candidates"].toArray()){
        const auto candidate=value.toObject();
        if(candidate["refined_top_y"].toArray()==QJsonArray{1013,1013})
            pairedTextureRejected=pairedTextureRejected ||(candidate["accepted_for_ranking"]==false
                &&candidate["rejection_reason"]=="observed_inter_row_gap_mismatch");
    }
    check(pairedTextureRejected,"paired texture rejection retains the actually measured inconsistent coordinates");
    fill(120,973,865,230,80);fill(997,973,865,230,80);
    fill(400,1013,400,190,90);fill(1277,1013,400,190,90);
    const auto textureWithoutEdge=relink::vision::detectCollectionLayout(frame);
    check(textureWithoutEdge["complete"]==false &&textureWithoutEdge["cards"].toArray().isEmpty()
        &&textureWithoutEdge["edge_evidence"].toObject()["tail_top_selection"].toObject()["accepted"]==false,
        "pitch evidence never supplies an absent genuine tail edge when only the internal texture remains");
    frame.pixels=tailConflictPixels;
    const auto tailCards=tailConflict["cards"].toArray();
    if(tailCards.size()==8)for(int index=6;index<8;++index){const auto card=tailCards[index].toObject();
        check(card["bounds"].toArray()[1]==983 &&card["fields_bounds"].isNull()
            &&card["edges"].toObject()["top"]==true &&card["edges"].toObject()["bottom"]==false,
            "observed tail remains partial and never receives invented fields");
    }
    fill(120,983,865,220,80);
    const auto missingTailPeer=relink::vision::detectCollectionLayout(frame);
    check(missingTailPeer["complete"]==false &&missingTailPeer["cards"].toArray().isEmpty()
        &&missingTailPeer["edge_evidence"].toObject()["tail_top_selection"].toObject()["accepted"]==false,
        "one-column true tail plus interior texture cannot fabricate the missing peer edge");
    frame.pixels=tailConflictPixels;
    // Sharp single-step edges isolate the alignment contract from the
    // multi-pixel antialias rise used by the positive fixture above.
    fill(120,983,865,220,84);
    fill(997,983,865,220,80);
    fill(997,988,865,215,84);
    fill(1277,1013,400,190,90);
    const auto misalignedTail=relink::vision::detectCollectionLayout(frame);
    check(misalignedTail["complete"]==false &&misalignedTail["cards"].toArray().isEmpty(),
        "five-pixel tail top mismatch does not widen the three-pixel joint-edge contract");
    bool rejectedMisalignment=false;
    for(const auto& value:misalignedTail["edge_evidence"].toObject()["tail_top_selection"].toObject()["candidates"].toArray())
        rejectedMisalignment=rejectedMisalignment ||value.toObject()["rejection_reason"]=="column_tops_misaligned";
    check(rejectedMisalignment,"diagnostic distinguishes misaligned measured tops from an absent top");
    frame.dpiX=96;
    check(relink::vision::collectionLayoutProfile(frame)["error"]=="E_COLLECTION_PROFILE_FRAME","uncalibrated dpi rejected");
    frame.dpiX=144;frame.validBytes--;
    check(relink::vision::collectionLayoutProfile(frame)["error"]=="E_COLLECTION_PROFILE_FRAME","invalid pixel buffer rejected");
    if(argc>2 &&QString::fromLocal8Bit(argv[2])=="--white-top-replay"){
        QFile file(QString::fromLocal8Bit(argv[1]));
        check(file.open(QIODevice::ReadOnly),"independent current-border numerical recording opens");
        const auto steps=QJsonDocument::fromJson(file.readAll()).object()["steps"].toArray();
        check(steps.size()==3,"independent border probe contains three actual captures");
        for(const auto& step:steps){const auto profile=step.toObject()["result"].toObject()["collection_layout_profile"].toObject();
            const auto original=profile;
            const auto replay=relink::vision::collectionLayoutEdgeCandidates(profile);
            const auto rows=replay["rows"].toArray();
            check(rows.size()==4,"real white-top correction retains all full and clipped rows");
            if(rows.size()==4)check(rows[1].toObject()["cells"].toArray()[0].toObject()["top"]==541,
                "recorded long white ridge at 541 is used instead of weak scene texture at 538");
            check(profile==original &&replay["frame_id"]==profile["frame_id"]
                &&replay["frame_sha256"]==profile["frame_sha256"] &&replay["actions_enabled"]==false,
                "numerical profile remains unchanged and never grants a selected-card input lease");
        }
    }
    else if(argc>2 &&QString::fromLocal8Bit(argv[2])=="--selection-gap-replay"){
        QFile file(QString::fromLocal8Bit(argv[1]));
        check(file.open(QIODevice::ReadOnly),"run03 full numerical signal-profile recording opens");
        const auto steps=QJsonDocument::fromJson(file.readAll()).object()["steps"].toArray();
        const auto attempts=steps.isEmpty()?QJsonArray{}:steps.last().toObject()["capture_attempts"].toArray();
        check(attempts.size()==3,"run03 contains three actual failed selection observations");
        const QVector<QJsonArray> expected{{1093,1093},{1093,1092},{1092,1092}};
        for(int i=0;i<attempts.size();++i){
            const auto packet=attempts[i].toObject()["result"].toObject();
            const auto profile=packet["collection_layout_profile"].toObject();
            check(profile["schema"]=="collection-layout-profile-v1"
                &&packet["collection_layout"].toObject()["error"]=="E_COLLECTION_ROW_EDGES",
                "run03 regression uses the original complete same-frame numerical profile rather than guessed coordinates");
            const auto replay=relink::vision::collectionLayoutEdgeCandidates(profile);
            const auto rows=replay["rows"].toArray();
            const auto selection=replay["tail_top_selection"].toObject();
            check(rows.size()==4 &&selection["accepted"]==true
                &&i<expected.size() &&selection["selected_top_y"].toArray()==expected[i],
                "all three actual selected-outline frames recover their observed genuine tail pair");
            if(rows.size()==4){
                const auto full=rows[1].toObject()["cells"].toArray();
                check(full[0].toObject()["top"]==(i==0?538:541) &&full[1].toObject()["top"]==543,
                    "only a stripe observed at both ends is preferred; first transitional missing endpoint keeps prior evidence");
                for(const auto& value:rows.last().toObject()["cells"].toArray())
                    check(value.toObject()["top_edge"]==true &&value.toObject()["bottom_edge"]==false,
                        "recovered tail is still clipped and has no invented lower edge");
            }
            const auto spacing=selection["same_frame_pitch"].toObject();
            check(spacing["proven"]==true &&spacing["tolerance_pixels"]==3
                &&spacing["basis"]=="same_frame_independent_complete_row_gaps_and_bottom_intervals",
                "selection expansion is handled by actual per-column gaps with the original three-pixel bound");
            bool falseTextureRejected=false;
            for(const auto& value:selection["candidates"].toArray()){
                const auto candidate=value.toObject();
                if(candidate["refined_top_y"].toArray()==QJsonArray{1118,1118})
                    falseTextureRejected=falseTextureRejected ||(candidate["accepted_for_ranking"]==false
                        &&candidate["rejection_reason"]=="observed_inter_row_gap_mismatch");
            }
            check(falseTextureRejected,"run03 paired internal texture remains rejected despite selected-top expansion");
            for(int count:{1,2}){
                auto negative=profile;auto bands=negative["row_bands"].toArray();
                for(int c=0;c<count;++c){auto band=bands[c].toObject();auto rowSignals=band["rows"].toArray();
                    const auto flat=rowSignals[1083-280].toArray()[5];
                    for(int y=1083;y<=1103;++y){auto signal=rowSignals[y-280].toArray();signal[5]=flat;rowSignals[y-280]=signal;}
                    band["rows"]=rowSignals;bands[c]=band;
                }
                negative["row_bands"]=bands;
                const auto rejected=relink::vision::collectionLayoutEdgeCandidates(negative);
                check(rejected["rows"].toArray().isEmpty() &&rejected["complete"]==false
                    &&rejected["actions_enabled"]==false,"removing one or both genuine measured tail rises does not manufacture a replacement edge");
            }
            check(replay["frame_id"]==profile["frame_id"] &&replay["frame_sha256"]==profile["frame_sha256"],
                "full numerical signal replay remains bound to the unchanged recorded frame");
            std::printf("RUN03_SELECTION_PROFILE=%d; full_signal_profile=true; numerical_only=true; selection=%s\n",
                i,QJsonDocument(selection).toJson(QJsonDocument::Compact).constData());
        }
    }
    else if(argc>2 &&QString::fromLocal8Bit(argv[2])=="--tail-pitch-replay"){
        QFile file(QString::fromLocal8Bit(argv[1]));
        check(file.open(QIODevice::ReadOnly),"AS Val recorded numerical candidate file opens");
        const auto document=QJsonDocument::fromJson(file.readAll()).object();
        const auto steps=document["steps"].toArray();int scroll=-1;
        for(int i=0;i<steps.size();++i)if(steps[i].toObject()["kind"]=="scroll_visible_list")scroll=i;
        check(scroll>0 &&scroll+1<steps.size(),"AS Val replay includes actual before and after scroll observations");
        if(scroll>0 &&scroll+1<steps.size())for(int side=0;side<2;++side){
            const auto packet=steps[scroll+(side?1:-1)].toObject()["result"].toObject();
            const auto layout=packet["collection_layout"].toObject();
            const auto evidence=layout["edge_evidence"].toObject();
            const auto oldSelection=evidence["tail_top_selection"].toObject();
            const auto rows=evidence["rows"].toArray(),candidates=oldSelection["candidates"].toArray();
            const auto originalRows=rows,originalCandidates=candidates;
            const auto selected=relink::vision::detail::selectMeasuredTailTop(rows,candidates);
            const QJsonArray wanted=side?QJsonArray{1093,1092}:QJsonArray{1130,1130};
            const QJsonArray wrong=side?QJsonArray{1118,1117}:QJsonArray{1155,1156};
            check(oldSelection["selected_top_y"].toArray()==wrong,"frozen native trace records the actual stronger wrong peak");
            check(selected["accepted"]==true &&selected["selected_top_y"].toArray()==wanted,
                "actual native candidate trace chooses the independently observed true pair rather than extrapolating pitch");
            check(selected["same_frame_pitch"].toObject()["proven"]==true
                &&selected["thresholds_changed"]==false &&selected["actions_enabled"]==false,
                "numerical trace replay retains strict thresholds and does not become an actionable layout");
            bool rejected=false;QJsonArray withoutReal,missingLeft;
            for(const auto& value:selected["candidates"].toArray()){
                auto candidate=value.toObject();const auto measured=candidate["refined_top_y"].toArray();
                if(measured==wrong)rejected=rejected ||(candidate["accepted_for_ranking"]==false
                    &&candidate["rejection_reason"]=="observed_inter_row_gap_mismatch");
                if(measured!=wanted)withoutReal.append(candidate);
                else candidate["refined_top_y"]=QJsonArray{-1,measured[1]};
                missingLeft.append(candidate);
            }
            check(rejected,"real paired internal texture is rejected by observed complete-row pitch, not weaker OCR or wider bounds");
            check(relink::vision::detail::selectMeasuredTailTop(rows,withoutReal)["accepted"]==false,
                "removing all genuine observed candidates never manufactures the expected tail coordinate");
            check(relink::vision::detail::selectMeasuredTailTop(rows,missingLeft)["accepted"]==false,
                "one missing actual column cannot be supplied by pitch or by its peer");
            check(rows==originalRows &&candidates==originalCandidates,"recorded candidate replay leaves all source measurements unchanged");
            std::printf("ASVAL_TAIL_TRACE_SIDE=%d; source_frame=%s; numerical_candidates_only=true; full_pixel_profile=false; selection=%s\n",
                side,layout["frame_id"].toString().toUtf8().constData(),QJsonDocument(selected).toJson(QJsonDocument::Compact).constData());
        }
    }
    else if(argc>1){
        const bool partialRowReplay=argc>2 &&QString::fromLocal8Bit(argv[2])=="--partial-row-replay";
        const bool shortTailReplay=argc>2 &&QString::fromLocal8Bit(argv[2])=="--short-tail-replay";
        const bool tailTopReplay=argc>2 &&QString::fromLocal8Bit(argv[2])=="--tail-top-replay";
        QFile file(QString::fromLocal8Bit(argv[1]));
        check(file.open(QIODevice::ReadOnly),"real numerical profile replay file opens");
        const auto document=QJsonDocument::fromJson(file.readAll()).object();
        auto steps=document["steps"].toArray();
        if(document["schema"]=="collection-layout-profile-v1")steps.append(QJsonObject{{"result",QJsonObject{{"collection_layout_profile",document}}}});
        if(tailTopReplay){
            QJsonArray failedAttempts;
            for(const auto& step:steps)for(const auto& attempt:step.toObject()["capture_attempts"].toArray()){
                const auto result=attempt.toObject()["result"].toObject();
                if(result["collection_layout"].toObject()["error"]=="E_COLLECTION_CARD_HORIZONTAL_EDGE"
                    &&result.contains("collection_layout_profile"))failedAttempts.append(QJsonObject{{"result",result}});
            }
            steps=failedAttempts;
            check(steps.size()==3,"probe01 replay retains all three separate failed-frame measurements");
        }
        check(!steps.isEmpty(),"numerical replay contains actual frames");
        const bool dynamicProbe=steps.size()<20;
        QVector<int> indices;
        for(int i=0;i<steps.size();++i)if(steps[i].toObject()["result"].toObject().contains("collection_layout_profile"))indices.append(i);
        const QVector<int> thumbs=dynamicProbe?QVector<int>(indices.size(),tailTopReplay?312:(partialRowReplay?360:(shortTailReplay?348:304))):QVector<int>{304,310,316,304};
        QJsonArray firstTrack;
        for(int i=0;i<indices.size();++i){const auto profile=steps[indices[i]].toObject()["result"].toObject()["collection_layout_profile"].toObject();
            const auto replay=relink::vision::collectionLayoutEdgeCandidates(profile);
            const auto scrollbar=replay["scrollbar_candidate"].toObject();
            check(!replay.contains("error"),"real signal replay detects scrollbar");
            if(i==0)firstTrack=scrollbar["track_bounds"].toArray();
            check(scrollbar["track_bounds"].toArray()==firstTrack,"real full track rectangle stays identical across scroll");
            check(scrollbar["thumb_bounds"].toArray().at(2)==firstTrack.at(2),"real thumb center strip width stays identical");
            check(scrollbar["track_bounds"].toArray().at(1)==304 &&scrollbar["track_bounds"].toArray().at(3)==899,"real stable track");
            check(scrollbar["thumb_bounds"].toArray().at(1)==thumbs[i] &&scrollbar["thumb_bounds"].toArray().at(3)==(tailTopReplay?70:(shortTailReplay?61:62)),"real observed thumb delta");
            check(replay["rows"].toArray().size()==(partialRowReplay?5:4),"real replay detects observed full and clipped rows");
            check(replay["complete"]==false &&replay["actions_enabled"]==false,"numeric replay alone never enables action");
            if(tailTopReplay){
                const auto selection=replay["tail_top_selection"].toObject();
                check(selection["accepted"]==true &&selection["selected_top_y"].toArray()==QJsonArray{983,983},
                    "all three actual probe01 profiles independently recover the observed paired y983 tail edge");
                bool rejectedFalsePeak=false;
                for(const auto& value:selection["candidates"].toArray()){const auto candidate=value.toObject();
                    if(candidate["peak_y"]==1010)rejectedFalsePeak=candidate["accepted_for_ranking"]==false
                        &&candidate["rejection_reason"]=="left_top_unobserved";
                }
                check(rejectedFalsePeak,"actual profile logs reject the stronger y1010 texture instead of fabricating left top");
                for(const auto& value:replay["rows"].toArray().last().toObject()["cells"].toArray()){
                    const auto cell=value.toObject();
                    check(cell["top"]==983 &&cell["top_edge"]==true &&cell["bottom_edge"]==false,
                        "actual profile output retains observed partial tail without invented lower edge");
                }
                check(replay["frame_id"]==profile["frame_id"] &&replay["frame_sha256"]==profile["frame_sha256"],
                    "tail replay diagnostics retain exact original frame identity");
            }
            if(partialRowReplay){
                const auto unchanged=profile;
                const auto observedRows=replay["rows"].toArray();
                const auto hasMissingBottom=[](const QJsonObject& evidence){
                    int matches=0;
                    for(const auto& value:evidence["rows"].toArray())for(const auto& v:value.toObject()["cells"].toArray()){
                        const auto cell=v.toObject();
                        if(cell["top_edge"]==true &&cell["bottom_edge"]==true &&cell["top"]==908
                            &&cell["bottom"].toInt()>=1167 &&cell["bottom"].toInt()<=1168)++matches;
                    }
                    return matches==2;
                };
                check(hasMissingBottom(replay),"actual run09 both columns independently recover their measured lower ridge");
                if(observedRows.size()==5){
                    for(const auto& value:observedRows.last().toObject()["cells"].toArray()){
                        const auto cell=value.toObject();
                        check(cell["top"]==1183 &&cell["bottom"]==1202 &&cell["top_edge"]==true
                            &&cell["bottom_edge"]==false,"actual run09 tiny next row stays clipped with an unproven bottom");
                    }
                    const auto selected=observedRows[2].toObject()["cells"].toArray();
                    check(selected[0].toObject()["top"]==632 &&selected[0].toObject()["bottom"]==895
                        &&selected[1].toObject()["top"]==634 &&selected[1].toObject()["bottom"]==893,
                        "run09 selected row remains unchanged rather than expanding selection tolerance");
                }
                const auto editRows=[](QJsonObject p,int column,const auto& edit){
                    auto bands=p["row_bands"].toArray();auto band=bands[column].toObject();auto rows=band["rows"].toArray();
                    for(int i=0;i<rows.size();++i){auto row=rows[i].toArray();edit(row);rows[i]=row;}
                    band["rows"]=rows;bands[column]=band;p["row_bands"]=bands;return p;
                };
                const auto rejectBottom=[&](const QJsonObject& negative,const char* name){
                    const auto result=relink::vision::collectionLayoutEdgeCandidates(negative);
                    check(!hasMissingBottom(result),name);
                    check(result["complete"]==false &&result["actions_enabled"]==false,"negative numerical replay never enables input");
                };
                auto negative=editRows(profile,0,[](QJsonArray& row){
                    if(row[0].toInt()>=1163 &&row[0].toInt()<=1172)row[5]=0;
                });
                rejectBottom(negative,"strict right peer cannot invent missing left wide-band ridge");
                negative=editRows(profile,0,[](QJsonArray& row){
                    if(row[0].toInt()>=1163 &&row[0].toInt()<=1172)row[4]=0;
                });
                rejectBottom(negative,"strict right peer cannot invent a missing left right-endpoint ridge");
                negative=editRows(profile,0,[](QJsonArray& row){
                    if(row[0].toInt()>=1166 &&row[0].toInt()<=1169)row[2]=10000;
                });
                rejectBottom(negative,"new variance spike is not explained by the measured neighboring background");
                negative=profile;
                for(int c=0;c<2;++c)negative=editRows(negative,c,[](QJsonArray& row){
                    if(row[0].toInt()>=1159 &&row[0].toInt()<=1175)row[2]=10000;
                });
                rejectBottom(negative,"two secondary witnesses cannot replace one strict primary ridge");
                negative=profile;
                auto vertical=negative["right_edge_vertical_profiles"].toArray();
                for(int j=0;j<vertical.size();++j){auto signal=vertical[j].toObject();if(signal["x"]!=1866)continue;
                    auto means=signal["means"].toArray();for(int y=1169;y<=1174;++y)means[y-280]=0;
                    signal["means"]=means;vertical[j]=signal;
                }
                negative["right_edge_vertical_profiles"]=vertical;
                rejectBottom(negative,"inward fill transition rejects a top-like paired ridge");
                negative=profile;
                for(int c=0;c<2;++c)negative=editRows(negative,c,[](QJsonArray& row){
                    if(row[0].toInt()>=1178 &&row[0].toInt()<=1202)row[5]=0;
                });
                check(relink::vision::collectionLayoutEdgeCandidates(negative)["rows"].toArray().isEmpty(),
                    "missing clipped-tail fill steps do not become a fabricated completed 35px empty tail");
                negative=profile;
                for(int c=0;c<2;++c)negative=editRows(negative,c,[](QJsonArray& row){
                    if(row[0].toInt()>=1173 &&row[0].toInt()<=1202)row[2]=10000;
                });
                negative=editRows(negative,0,[](QJsonArray& row){
                    if(row[0].toInt()>=1178 &&row[0].toInt()<=1202)row[5]=0;
                });
                check(relink::vision::collectionLayoutEdgeCandidates(negative)["rows"].toArray().isEmpty(),
                    "single high-variance tail rise does not satisfy paired scene-relative top evidence");
                check(profile==unchanged &&replay["frame_id"]==profile["frame_id"]
                    &&replay["frame_sha256"]==profile["frame_sha256"],"profile replay remains bound to unchanged source measurements");
            }
            if(shortTailReplay){
                const auto original=profile;
                const auto proof=replay["tail_evidence"].toObject();
                check(proof["accepted"]==true &&proof["gap_pixels"]==16 &&proof["maximum_gap_pixels"]==16
                    &&proof["no_sustained_fill_rise"]==true,"run10 16px tail is backed by bounded same-frame gap measurements");
                const auto measured=proof["observed_inter_row_gaps"].toArray();
                check(measured.size()==2,"short tail requires independent evidence from both columns");
                for(const auto& v:measured){const auto c=v.toObject();
                    check(c["intervals"].toArray().size()>=2 &&c["largest_measured_distance"]==16,
                        "each column already exhibits the accepted 16px row separation");
                }
                const auto observedRows=replay["rows"].toArray();
                if(observedRows.size()==4)for(const auto& v:observedRows.last().toObject()["cells"].toArray()){
                    const auto cell=v.toObject();
                    check(cell["top"]==927 &&cell["bottom"]==1186 &&cell["top_edge"]==true &&cell["bottom_edge"]==true,
                        "last full run10 row is measured without expanding it into the gap or inventing a clipped successor");
                }
                const auto editRows=[](QJsonObject p,int column,const auto& edit){
                    auto bands=p["row_bands"].toArray();auto band=bands[column].toObject();auto rows=band["rows"].toArray();
                    const auto before=rows;
                    for(int j=0;j<rows.size();++j){auto row=rows[j].toArray();edit(row,before);rows[j]=row;}
                    band["rows"]=rows;bands[column]=band;p["row_bands"]=bands;return p;
                };
                const auto rejectGap=[&](const QJsonObject& negative,const char* error,const char* name){
                    const auto candidate=relink::vision::collectionLayoutEdgeCandidates(negative);
                    check(candidate["rows"].toArray().isEmpty() &&candidate["tail_evidence"].toObject()["error"]==error,name);
                    check(candidate["complete"]==false &&candidate["actions_enabled"]==false,"negative short-tail evidence never authorizes input");
                };
                auto negative=profile;
                for(int c=0;c<2;++c)negative=editRows(negative,c,[](QJsonArray& row,const QJsonArray& before){
                    const int y=row[0].toInt();if(y<1180 ||y>1191)return;
                    const auto shifted=before[y+1-280].toArray();for(int j=1;j<8;++j)row[j]=shifted[j];
                });
                rejectGap(negative,"E_COLLECTION_TAIL_GAP_BOUND","17px unproven tail cannot pass the short-gap exception");
                negative=profile;
                for(int c=0;c<2;++c)negative=editRows(negative,c,[](QJsonArray& row,const QJsonArray& before){
                    const int y=row[0].toInt();if(y<371 ||y>383)return;
                    const auto shifted=before[y+2-280].toArray();for(int j=1;j<8;++j)row[j]=shifted[j];
                });
                rejectGap(negative,"E_COLLECTION_TAIL_GAP_NOT_CORROBORATED","16px tail needs actual equal-sized current-frame gaps rather than a fixed permitted height");
                for(int column=0;column<2;++column)for(const int start:{1194,1198,1200}){
                    negative=editRows(profile,column,[start](QJsonArray& row,const QJsonArray&){
                        const int y=row[0].toInt();if(y>=start &&y<=1202)row[5]=row[5].toDouble()+1.0;
                    });
                    rejectGap(negative,"E_COLLECTION_TAIL_GAP_FILL_RISE","even a one-column low-contrast sustained fill step is not an empty inter-row gap");
                }
                negative=editRows(profile,0,[](QJsonArray& row,const QJsonArray&){
                    if(row[0]==1198)row[5]=row[5].toDouble()+1.0;
                });
                const auto impulse=relink::vision::collectionLayoutEdgeCandidates(negative);
                check(impulse["tail_evidence"].toObject()["accepted"]==true,
                    "one transient sample without sustained fill does not invent an additional card row");
                check(profile==original &&replay["frame_id"]==profile["frame_id"]
                    &&replay["frame_sha256"]==profile["frame_sha256"],"short gap replay keeps original numeric frame binding and does not mutate inputs");
            }
            std::printf("REAL_PROFILE_STEP=%d; numerical_only=true; edge_candidates=%s\n",indices[i],QJsonDocument(replay).toJson(QJsonDocument::Compact).constData());
        }
    }
    std::printf("COLLECTION_LAYOUT_TESTS=%s; assertions=%d; failures=%d; synthetic_input=true; image_file_writes=0\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
