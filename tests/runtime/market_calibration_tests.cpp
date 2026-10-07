#include "application/vision/skin_page_classifier.h"
#include "application/runtime/startup_observer.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <iostream>
using namespace relink::vision;
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);int count=0,failed=0;
 auto check=[&](bool ok,const QString& name){++count;failed+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<name.toStdString()<<'\n';};
 QFile file(argc>1?QString::fromLocal8Bit(argv[1]):QString());check(file.open(QIODevice::ReadOnly),"fixture_open");
 const auto cases=QJsonDocument::fromJson(file.readAll()).object().value("cases").toArray();check(cases.size()==6,"six_live_label_projections_present");
 QHash<QString,QJsonObject> fixtures;
 for(const auto& v:cases){const auto item=v.toObject(),page=item.value("observation").toObject();const auto r=classifySkinPage(page);
  fixtures.insert(item.value("id").toString(),page);
  check(r.validInput && toString(r.page)==item.value("expected_page").toString(),item.value("id").toString()+":"+QString::fromUtf8(QJsonDocument(r.toJson()).toJson(QJsonDocument::Compact)));
  auto projection=projectMarketAnchorDiagnostics(page);check(projection["coverage"]=="anchor_projection" && !classifySkinPage(projection).validInput,"debug_projection_is_not_full_frame");
  check(!r.toJson()["actions_enabled"].toBool()&&!r.toJson()["live_calibrated"].toBool(),"fixture_does_not_authorize_actions_or_certify_all_screens");
  for(double factor:{.5,1.5,2.0}){auto scaled=page;auto words=scaled["words"].toArray();
   for(int i=0;i<words.size();++i){auto w=words[i].toObject();for(const char* key:{"x","y","width","height"})w[key]=w[key].toDouble()*factor;words[i]=w;}
   scaled["words"]=words;scaled["width"]=page["width"].toDouble()*factor;scaled["height"]=page["height"].toDouble()*factor;
   check(classifySkinPage(scaled).page==r.page,"physical_scale_invariant");}
  auto low=page;auto lowWords=low["words"].toArray();for(int i=0;i<lowWords.size();++i){auto w=lowWords[i].toObject();w["score"]=.69;lowWords[i]=w;}low["words"]=lowWords;
  check(classifySkinPage(low).page==SkinPage::Unknown,"local_join_does_not_upgrade_low_scores");
  auto roi=page;roi["coverage"]="roi";check(!classifySkinPage(roi).validInput,"partial_coverage_not_page_proof");
  auto polluted=page;auto list=polluted["words"].toArray();list.append(QJsonObject{{"text","ACCOUNT_SECRET_123"},{"x",800},{"y",500},{"width",150},{"height",20},{"secret","private"}});polluted["words"]=list;
  check(!QJsonDocument(projectMarketAnchorDiagnostics(polluted)).toJson().contains("SECRET"),"diagnostic_text_allowlist_excludes_unrelated_private_tokens");
 }
 auto home=fixtures["skin_home"];auto words=home["words"].toArray();
 words.append(QJsonObject{{"text","SCENE"},{"x",735},{"y",322},{"width",1},{"height",20}});home["words"]=words;
 check(classifySkinPage(home).page==SkinPage::SkinHome,"unrelated_scene_row_token_does_not_break_local_exact_label");
 for(const auto& key:QStringList{"mandel","skin_home","empty_1","catalog_filter"}){
  auto missing=fixtures[key];auto original=missing["words"].toArray();QJsonArray remaining;
  QString required=key=="mandel"?QStringLiteral("当"):key=="skin_home"?QStringLiteral("我"):key=="empty_1"?QStringLiteral("暂"):QStringLiteral("确");
  for(const auto& w:original)if(!w.toObject()["text"].toString().contains(required))remaining.append(w);
  missing["words"]=remaining;check(classifySkinPage(missing).page==SkinPage::Unknown,"missing_independent_anchor_remains_unknown_"+key);
 }
 auto conflict=fixtures["skin_home"];words=conflict["words"].toArray();for(const auto& w:fixtures["catalog_filter"]["words"].toArray())words.append(w);conflict["words"]=words;
 check(classifySkinPage(conflict).reason=="E_PAGE_AMBIGUOUS","incompatible_live_page_projections_are_not_chosen_arbitrarily");
 auto budget=fixtures["skin_home"];words={};for(int i=0;i<300;++i)words.append(QJsonObject{{"text",QStringLiteral("价")},{"x",800},{"y",500},{"width",10},{"height",10}});budget["words"]=words;
 auto limited=projectMarketAnchorDiagnostics(budget);check(limited["words"].toArray().size()==256&&limited["truncated"].toBool(),"debug_word_limit_is_explicit");
 relink::runtime::StartupObserver route;relink::runtime::RuntimeContext context{"replay","live-projection-replay","test-clock","startup",0,1};check(route.start(context,0),"start_recorded_route_replay");
 int index=0;for(const auto& key:QStringList{"skin_home","empty_1","empty_2","home_after_empty","catalog_filter"}){++index;
  check(route.observe({QString::number(index),context,index*100-1,index*100,fixtures[key]},index*100),"replay_accepts_fresh_label_projection_"+key);}
 check(route.snapshot().phase==relink::runtime::StartupPhase::FilterObserved,"original_empty_return_home_filter_sequence_replayed");
 std::cout<<"MARKET_CALIBRATION_TESTS="<<(failed?"FAIL":"PASS")<<"; assertions="<<count<<"; failures="<<failed<<"; live_capture=false\n";return failed?1:0;
}
