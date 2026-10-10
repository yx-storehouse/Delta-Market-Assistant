#include "application/vision/catalog_filter_reader.h"
#include "application/runtime/catalog_filter_plan.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <limits>
#include <iostream>
using namespace relink::vision;
using namespace relink::runtime;
using relink::runtime::observation::FrameEnvelope;
namespace {
void paint(FrameEnvelope& f,QRect rect,int value){
 for(int y=rect.top();y<=rect.bottom();++y)for(int x=rect.left();x<=rect.right();++x){
  auto* p=reinterpret_cast<unsigned char*>(f.pixels.data()+qint64(y)*f.strideBytes+x*4);
  p[0]=p[1]=p[2]=static_cast<unsigned char>(value);p[3]=255;
 }
}
QJsonObject word(QString text,int x,int y,int width,int height=24){return {{"text",text},{"x",x},{"y",y},{"width",width},{"height",height}};}
QJsonObject bind(QJsonObject o,const FrameEnvelope& f){
 o["frame_id"]=f.frameId;o["frame_sha256"]=QString::fromLatin1(QCryptographicHash::hash(f.pixels,QCryptographicHash::Sha256).toHex());return o;
}
}
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);int count=0,failed=0;
 auto check=[&](bool ok,const char* name){++count;failed+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<'\n';};
 FrameEnvelope f;f.frameId="frame:test";f.width=2560;f.height=1440;f.dpiX=f.dpiY=144;
 f.strideBytes=f.width*4;f.validBytes=qint64(f.strideBytes)*f.height;f.pixels=QByteArray(f.validBytes,char(20));
 const QPoint locations[]={{754,534},{1073,534},{754,630},{1073,630},{1392,630},{1711,630}};
 for(const auto& at:locations){QRect r(at,QSize(36,36));paint(f,r,55);paint(f,r.adjusted(3,3,-3,-3),20);}
 QJsonArray words{word(QStringLiteral("赛季"),540,434,48),word(QStringLiteral("品阶"),540,638,48),
  word(QStringLiteral("枪种"),540,734,48),word(QStringLiteral("确认"),1255,993,50),
  word(QStringLiteral("全部赛季"),779,436,82,20)};
 const QStringList labels{QStringLiteral("已拥有"),QStringLiteral("未拥有"),QStringLiteral("传说品阶"),QStringLiteral("史诗品阶"),QStringLiteral("稀有品阶"),QStringLiteral("普通品阶")};
 for(int i=0;i<6;++i)words.append(word(labels[i],locations[i].x()+52,locations[i].y()+8,84,20));
 QJsonObject o{{"width",2560},{"height",1440},{"coverage","full_client"},{"coordinate_space","client_physical_px"},{"words",words}};o=bind(o,f);
 const auto initial=readCatalogFilter(f,o);
 check(initial.complete && initial.validPage,"all_six_boxes_and_season_observed");
 check(initial.seasonLabel==QStringLiteral("全部赛季"),"season_is_actual_text_not_default");
 for(const auto& box:initial.boxes)check(box.state==CheckState::Unchecked,"positive_empty_border_evidence");
 check(initial.toJson()["same_frame"].toBool() && !initial.toJson()["actions_enabled"].toBool(),"pure_same_frame_no_actions");
 auto invalid=o;invalid["coverage"]="roi";check(!readCatalogFilter(f,invalid).complete,"partial_ocr_is_not_page_evidence");
 invalid=o;invalid["frame_id"]="different";check(readCatalogFilter(f,invalid).reason=="E_FILTER_FRAME_BINDING","cross_frame_ocr_rejected");
 invalid=o;invalid["frame_sha256"]="stale";check(readCatalogFilter(f,invalid).reason=="E_FILTER_FRAME_BINDING","pixel_hash_mismatch_rejected");
 auto pixels=f;pixels.pixels.chop(1);check(!readCatalogFilter(pixels,o).complete,"truncated_pixels_rejected");
 pixels=f;pixels.validBytes--;check(!readCatalogFilter(pixels,o).complete,"invalid_byte_count_rejected");
 pixels=f;pixels.pixelFormat="RGBA8";check(!readCatalogFilter(pixels,o).complete,"pixel_format_rejected");
 pixels=f;pixels.strideBytes=1;check(!readCatalogFilter(pixels,o).complete,"stride_rejected");
 pixels=f;pixels.dpiX=96;check(readCatalogFilter(pixels,o).reason=="E_FILTER_GEOMETRY_UNCALIBRATED","unsupported_dpi_not_scaled_by_guess");
 pixels=f;pixels.frameId.clear();check(!readCatalogFilter(pixels,o).complete,"missing_frame_id_rejected");
 auto without=words;without.removeAt(4);invalid=o;invalid["words"]=without;check(!readCatalogFilter(f,invalid).complete,"missing_season_not_all_seasons");
 without=words;without.removeAt(5);invalid=o;invalid["words"]=without;check(readCatalogFilter(f,invalid).boxes["owned"].state==CheckState::Unknown,"missing_label_not_unchecked");
 without=words;auto low=without[5].toObject();low["score"]=.69;without[5]=low;invalid=o;invalid["words"]=without;
 check(readCatalogFilter(f,invalid).boxes["owned"].state==CheckState::Unknown,"low_score_label_not_upgraded");
 without=words;without.append(words[5]);invalid=o;invalid["words"]=without;check(!readCatalogFilter(f,invalid).complete,"overlapping_duplicate_label_rejected");
 without=words;without[5]=word(QStringLiteral("未拥有"),806,542,84,20);invalid=o;invalid["words"]=without;
 check(readCatalogFilter(f,invalid).boxes["owned"].state==CheckState::Unknown,"owned_not_substring_of_unowned");
 without=words;without.removeAt(4);for(int i=0;i<4;++i)without.append(word(QStringLiteral("全部赛季").mid(i,1),779+21*i,436,19,20));
 invalid=o;invalid["words"]=without;check(readCatalogFilter(f,invalid).seasonLabel==QStringLiteral("全部赛季"),"adjacent_split_season_label");
 without=words;without[4]=word(QStringLiteral("棱镜攻势S2"),779,436,145,20);invalid=o;invalid["words"]=without;
 check(readCatalogFilter(f,invalid).seasonLabel==QStringLiteral("棱镜攻势S2"),"selected_season_text_preserved");
 // Season re-read is missing-field-only and accepts actual same-frame OCR,
 // never an expected configuration label. These words/pixels are synthetic.
 auto missingSeason=o;auto seasonWords=words;seasonWords.removeAt(4);missingSeason["words"]=seasonWords;
 const auto seasonRegion=catalogSeasonLabelRefinementRegion(f,missingSeason);
 check(seasonRegion==QRect(770,426,270,42),"missing season requests exact calibrated text-only ROI excluding arrow");
 check(catalogSeasonLabelRefinementRegion(f,o).isEmpty(),"already observed season never triggers opportunistic replacement");
 auto local=bind(QJsonObject{{"width",2560},{"height",1440},{"coverage","roi"},
     {"coordinate_space","client_physical_px"},{"same_frame",true},
     {"words",QJsonArray{word(QStringLiteral("气象感应"),779,436,86,20)}}},f);
 QJsonObject seasonAudit;
 const auto merged=refineCatalogSeasonLabel(f,missingSeason,local,seasonRegion,&seasonAudit);
 const auto afterSeason=readCatalogFilter(f,merged);
 check(afterSeason.complete &&afterSeason.seasonLabel==QStringLiteral("气象感应"),"actual ROI season words flow through original reader");
 check(seasonAudit["replacement_applied"]==true &&seasonAudit["same_frame"]==true
     &&seasonAudit["words"]==local["words"] &&seasonAudit["bounds"].toArray()==QJsonArray{770,426,270,42}
     &&seasonAudit["frame_id"]==f.frameId &&seasonAudit["frame_sha256"]==missingSeason["frame_sha256"],
     "season refinement preserves actual word boxes frame hash and ROI audit");
 check(seasonAudit["expected_text_supplied"]==false &&seasonAudit["actions_enabled"]==false
     &&seasonAudit["image_file_writes"]==0,"season helper has no target text input actions or file writes");
 for(const auto& id:initial.boxes.keys())check(afterSeason.boxes[id].measurement==initial.boxes[id].measurement
     &&afterSeason.boxes[id].state==initial.boxes[id].state,"season re-read leaves real checkbox evidence unchanged");
 auto otherLocal=local;otherLocal["words"]=QJsonArray{word(QStringLiteral("不同实际赛季"),779,436,120,20)};
 const auto other=refineCatalogSeasonLabel(f,missingSeason,otherLocal,seasonRegion,&seasonAudit);
 check(readCatalogFilter(f,other).seasonLabel==QStringLiteral("不同实际赛季"),"reader preserves another actual label instead of substituting requested season");
 auto splitLocal=local;QJsonArray split;
 for(int i=0;i<4;++i)split.append(word(QStringLiteral("气象感应").mid(i,1),779+22*i,436,20,20));
 splitLocal["words"]=split;
 check(readCatalogFilter(f,refineCatalogSeasonLabel(f,missingSeason,splitLocal,seasonRegion)).seasonLabel==QStringLiteral("气象感应"),
     "adjacent split actual season tokens use unchanged reader joining rules");
 const auto rejectLocal=[&](const QJsonObject& bad,const char* name){QJsonObject audit;
     check(refineCatalogSeasonLabel(f,missingSeason,bad,seasonRegion,&audit)==missingSeason
         &&audit["replacement_applied"]==false,name);};
 auto badLocal=local;badLocal["frame_id"]="stale";rejectLocal(badLocal,"cross-frame season words rejected");
 badLocal=local;badLocal["frame_sha256"]=QString(64,'0');rejectLocal(badLocal,"cross-hash season words rejected");
 badLocal=local;badLocal["same_frame"]=false;rejectLocal(badLocal,"failed OCR is not merged as same-frame success");
 badLocal=local;badLocal["coverage"]="full_client";rejectLocal(badLocal,"season refinement requires bounded ROI observation");
 badLocal=local;badLocal["width"]=1920;rejectLocal(badLocal,"wrong source dimensions rejected");
 badLocal=local;badLocal["coordinate_space"]="rotated";rejectLocal(badLocal,"unmapped season coordinates rejected");
 badLocal=local;badLocal["words"]=QJsonArray{};rejectLocal(badLocal,"empty retry cannot fill missing season");
 badLocal=local;badLocal["words"]=QJsonArray{word(QStringLiteral("气象感应"),1020,436,86,20)};
 rejectLocal(badLocal,"word extending into right-arrow area rejected even with center near label");
 badLocal=local;auto lowSeason=word(QStringLiteral("气象感应"),779,436,86,20);lowSeason["score"]=.69;
 badLocal["words"]=QJsonArray{lowSeason};rejectLocal(badLocal,"season low confidence is not promoted by refinement");
 badLocal=local;badLocal["words"]=QJsonArray{word(QStringLiteral("气象"),779,430,42,16),word(QStringLiteral("感应"),824,449,42,16)};
 rejectLocal(badLocal,"different baselines do not form one season name");
 badLocal=local;badLocal["words"]=QJsonArray{word(QStringLiteral("气象"),779,436,42,20),word(QStringLiteral("感应"),970,436,42,20)};
 rejectLocal(badLocal,"widely separated words cannot complete a season");
 badLocal=local;auto duplicate=local["words"].toArray();duplicate.append(duplicate.first());badLocal["words"]=duplicate;
 rejectLocal(badLocal,"overlapping duplicate season tokens rejected");
 check(refineCatalogSeasonLabel(f,o,local,seasonRegion)==o,"already known label is never replaced");
 check(refineCatalogSeasonLabel(f,missingSeason,local,seasonRegion.translated(1,0))==missingSeason,"different refinement ROI rejected");
 pixels=f;pixels.dpiX=96;check(catalogSeasonLabelRefinementRegion(pixels,missingSeason).isEmpty(),"unsupported DPI cannot trigger narrow season OCR");
 auto wrongPage=missingSeason;wrongPage["words"]=QJsonArray{};
 check(catalogSeasonLabelRefinementRegion(f,wrongPage).isEmpty(),"missing catalog page anchors cannot trigger season OCR");
 auto missingCheckbox=missingSeason;auto missingCheckboxWords=seasonWords;missingCheckboxWords.removeAt(4);missingCheckbox["words"]=missingCheckboxWords;
 const auto incomplete=readCatalogFilter(f,refineCatalogSeasonLabel(f,missingCheckbox,local,seasonRegion));
 check(!incomplete.complete &&incomplete.seasonLabel==QStringLiteral("气象感应") &&incomplete.boxes["owned"].state==CheckState::Unknown,
     "season success does not bypass original checkbox and label completeness checks");
 check(missingSeason["words"].toArray()==seasonWords &&local["words"].toArray().size()==1,
     "season refinement never mutates input observation or raw ROI words");
 const QRect box(locations[0],QSize(36,36));
 for(int fill:{0,20,60,120,255}){pixels=f;paint(pixels,box,fill);
  check(measureFilterCheckbox(pixels,box,"test").state==CheckState::Unknown,"flat_region_not_a_checkbox");}
 pixels=f;paint(pixels,box.adjusted(6,6,-6,-6),90);check(measureFilterCheckbox(pixels,box,"test").state==CheckState::Unknown,"unexpected_fill_not_selected_by_guess");
 pixels=f;paint(pixels,QRect(box.x(),box.y(),box.width(),3),20);check(measureFilterCheckbox(pixels,box,"test").state==CheckState::Unknown,"missing_border_rejected");
 check(measureFilterCheckbox(f,QRect(-1,1,36,36),"test").state==CheckState::Unknown,"out_of_frame_rectangle");
 check(measureFilterCheckbox(f,QRect(1,1,4,4),"test").state==CheckState::Unknown,"tiny_region_rejected");
 check(measureFilterCheckbox(f,QRect(1,1,128,16),"test").reason=="E_FILTER_PIXELS","non_square_no_empty_sample_division");
 invalid=o;invalid["coordinate_space"]="windows_ocr_rotated";check(!readCatalogFilter(f,invalid).complete,"unmapped_coordinates_rejected");
 auto signal=initial.boxes["owned"].measurement;
 signal["interior_mean"]=168;signal["interior_stddev"]=45;signal["center_mean"]=193;signal["center_stddev"]=1;
 signal["bright_fraction"]=.87;signal["border_means"]=QJsonArray{67,67,72,65};
 check(classifyFilterCheckboxMeasurement(signal)==CheckState::Checked,"white_inset_selected_signal");
 for(const auto key:{"interior_mean","center_mean","center_stddev","sample_count","border_means"}){
  auto missing=signal;missing.remove(key);check(classifyFilterCheckboxMeasurement(missing)==CheckState::Unknown,"missing_measurement_rejected");}
 for(double n:{-1.,256.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
  auto bad=signal;bad["center_mean"]=n;check(classifyFilterCheckboxMeasurement(bad)==CheckState::Unknown,"invalid_measurement_number");}
 for(const auto& mutation:QList<QPair<QString,double>>{{"green_fraction",.1},{"bright_fraction",1.},{"center_stddev",20.},{"interior_stddev",0.},{"sample_count",10.5}}){
  auto bad=signal;bad[mutation.first]=mutation.second;check(classifyFilterCheckboxMeasurement(bad)==CheckState::Unknown,"unexpected_style_stays_unknown");}
 auto badBorder=signal;badBorder["border_means"]=QJsonArray{67,67,20,65};check(classifyFilterCheckboxMeasurement(badBorder)==CheckState::Unknown,"selected_requires_all_four_edges");
 {
  // Live 2026-10-09 (hotkey run 20261009-141700): the pointer rested on the
  // ticked 已拥有 after the season was chosen; only its border glowed.
  QJsonObject hovered{{"interior_mean",174.2734159779612},{"interior_stddev",43.75229066958887},{"center_mean",198.54036458333346},
   {"center_stddev",0.9620052149946706},{"bright_fraction",0.8677685950413223},{"green_fraction",0},{"sample_count",484},
   {"border_means",QJsonArray{105.68518518518533,104.35493827160492,105.85493827160492,103.16358024691355}}};
  check(classifyFilterCheckboxMeasurement(hovered)==CheckState::Checked,"hovered_ticked_box_read_by_its_inset");
  QJsonObject hoveredEmpty{{"interior_mean",26.8},{"interior_stddev",0.3},{"center_mean",26.9},{"center_stddev",0.2},
   {"bright_fraction",0},{"green_fraction",0},{"sample_count",484},{"border_means",QJsonArray{105,104,106,103}}};
  check(classifyFilterCheckboxMeasurement(hoveredEmpty)==CheckState::Unchecked,"glowing_empty_box_stays_unchecked");
  // Recorded under the pointer (8 runs): an empty box barely glows.
  QJsonObject recordedEmpty{{"interior_mean",25},{"interior_stddev",0.02},{"center_mean",25},{"center_stddev",0.02},
   {"bright_fraction",0},{"green_fraction",0},{"sample_count",484},{"border_means",QJsonArray{68.3,67.6,72.6,65.6}}};
  check(classifyFilterCheckboxMeasurement(recordedEmpty)==CheckState::Unchecked,"recorded_hovered_empty_box_unchecked");
  auto washed=hovered;washed["border_means"]=QJsonArray{170,168,171,169};
  check(classifyFilterCheckboxMeasurement(washed)==CheckState::Unknown,"border_as_bright_as_center_is_not_a_tick");
  auto dim=hovered;dim["border_means"]=QJsonArray{106,106,106,106};dim["center_mean"]=185;
  check(classifyFilterCheckboxMeasurement(dim)==CheckState::Unknown,"glowing_border_with_dim_center_stays_unknown");
  auto edge=hovered;edge["border_means"]=QJsonArray{120,118,119,117};edge["center_mean"]=200;
  check(classifyFilterCheckboxMeasurement(edge)==CheckState::Checked,"border_glow_up_to_120_accepted");
  edge["border_means"]=QJsonArray{121,118,119,117};
  check(classifyFilterCheckboxMeasurement(edge)==CheckState::Unknown,"border_glow_above_120_unknown");
  auto flat=hovered;flat["interior_stddev"]=2;flat["bright_fraction"]=1;
  check(classifyFilterCheckboxMeasurement(flat)==CheckState::Unknown,"flat_bright_square_still_not_a_tick");
  auto midAnimation=hovered;midAnimation["center_mean"]=110;
  check(classifyFilterCheckboxMeasurement(midAnimation)==CheckState::Unknown,"half_drawn_inset_stays_unknown");
 }
 pixels=f;paint(pixels,box,65);paint(pixels,box.adjusted(5,5,-5,-5),25);paint(pixels,box.adjusted(8,8,-8,-8),193);
 check(measureFilterCheckbox(pixels,box,"selected").state==CheckState::Checked,"synthetic_selected_raster_uses_real_pixel_sampler");
 check(bind(o,f)["frame_sha256"]==o["frame_sha256"],"reader_never_mutates_original_pixels");
 CatalogFilterTarget t{"snapshot:test",QStringLiteral("全部赛季"),false,false,{}};
 auto p=planCatalogFilter(initial,t);check(p.valid && p.alreadyMatches && p.steps.isEmpty(),"S11_preserve_matching_filter");
 check(!p.toJson()["actions_enabled"].toBool() && !p.toJson()["target_grouping_inferred"].toBool(),"plan_is_not_executor_or_task_grouper");
 t.seasonLabel=QStringLiteral("棱镜攻势S2");t.unowned=true;t.grades={"epic","rare"};p=planCatalogFilter(initial,t);
 check(p.steps.size()==1 && p.steps[0].toObject()["source_step"]=="S10","season_first_no_stale_ownership_or_grade_actions");
 t.seasonLabel=initial.seasonLabel;p=planCatalogFilter(initial,t);
 check(p.steps.size()==1 && p.steps[0].toObject()["source_step"]=="S12","ownership_before_grades");
 t.unowned=false;p=planCatalogFilter(initial,t);
 check(p.steps.size()==2 && p.steps[0].toObject()["field"]=="epic" && p.steps[1].toObject()["field"]=="rare","S14_multiple_grades_ordered");
 for(const auto& step:p.steps)check(step.toObject()["readback_required"].toBool() && !step.toObject()["execute"].toBool(),"each_step_requires_new_readback");
 auto observed=initial;observed.boxes["legendary"].state=CheckState::Checked;observed.boxes["epic"].state=CheckState::Checked;
 p=planCatalogFilter(observed,t);check(p.steps.size()==4 && p.steps[0].toObject()["source_step"]=="S13"
  && p.steps[1].toObject()["source_step"]=="S13" && p.steps[2].toObject()["source_step"]=="S14","S13_clear_residual_set_before_S14_select");
 observed.boxes["legendary"].state=CheckState::Unchecked;observed.boxes["rare"].state=CheckState::Checked;
 check(planCatalogFilter(observed,t).alreadyMatches,"already_correct_multiselect_not_toggled");
 observed.boxes["rare"].state=CheckState::Unknown;check(!planCatalogFilter(observed,t).valid,"incomplete_pixels_block_difference_plan");
 t.grades.insert("unknown");check(!planCatalogFilter(initial,t).valid,"unknown_grade_target_rejected");t.grades.clear();
 t.snapshotId.clear();check(!planCatalogFilter(initial,t).valid,"no_snapshot_no_target_plan");
 if(argc>1){QFile file(QString::fromLocal8Bit(argv[1]));check(file.open(QIODevice::ReadOnly),"live_signal_fixture_open");
  const auto cases=QJsonDocument::fromJson(file.readAll()).object()["cases"].toArray();check(!cases.isEmpty(),"live_signal_cases_present");
  int checked=0,unchecked=0;
  for(const auto& v:cases){const auto c=v.toObject();const auto actual=classifyFilterCheckboxMeasurement(c["measurement"].toObject());
   check(toString(actual)==c["expected_state"].toString(),"frozen_live_signal_replay_not_new_pixel_capture");
   checked+=actual==CheckState::Checked;unchecked+=actual==CheckState::Unchecked;}
  check(checked>=2 && unchecked>=6,"live_signal_fixture_contains_both_styles");
 }
 std::cout<<"CATALOG_FILTER_TESTS="<<(failed?"FAIL":"PASS")<<"; assertions="<<count<<"; failures="<<failed<<"; live_capture=false\n";return failed?1:0;
}
