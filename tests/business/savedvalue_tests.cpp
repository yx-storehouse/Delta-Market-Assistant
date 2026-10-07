#include "config/savedvalue_reader.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <iostream>
using namespace relink::config;
namespace {
QByteArray header(int kind,quint64 n){
 int exponent=0;while(exponent<3 && n>=(quint64(1)<<(8*(1<<exponent))))++exponent;
 QByteArray out(1,char(kind|(exponent<<6)));for(int i=0;i<(1<<exponent);++i)out.append(char((n>>(8*i))&255));return out;
}
QByteArray encode(const QJsonValue& v){
 if(v.isBool())return QByteArray(1,char(v.toBool()));
 if(v.isDouble())return header(4,quint64(v.toInteger()));
 if(v.isString()){const auto b=v.toString().toUtf8();return header(7,b.size())+b;}
 if(v.isArray()){const auto a=v.toArray();auto out=header(3,a.size());for(const auto& i:a)out+=encode(i);return out;}
 auto out=header(2,v.toObject().size());const auto o=v.toObject();for(auto i=o.begin();i!=o.end();++i)out+=encode(i.key())+encode(i.value());return out;
}
void appendObserved(QByteArray& out,const QJsonValue& v,const QMap<qsizetype,int>& widths){
 const auto prefix=[&](int kind,quint64 n){
  const int width=widths.value(out.size(),0);
  if(!width){out+=header(kind,n);return;}
  const int exponent=width==1?0:width==2?1:width==4?2:3;
  out.append(char(kind|(exponent<<6)));for(int i=0;i<width;++i)out.append(char((n>>(8*i))&255));
 };
 if(v.isBool()){out.append(char(v.toBool()));return;}
 if(v.isDouble()){prefix(4,quint64(v.toInteger()));return;}
 if(v.isString()){const auto bytes=v.toString().toUtf8();prefix(7,bytes.size());out+=bytes;return;}
 if(v.isArray()){const auto a=v.toArray();prefix(3,a.size());for(const auto& x:a)appendObserved(out,x,widths);return;}
 const auto object=v.toObject();prefix(2,object.size());for(auto i=object.begin();i!=object.end();++i){appendObserved(out,i.key(),widths);appendObserved(out,i.value(),widths);}
}
QJsonArray row(int index=0,int product=10602,bool enabled=true){
 QJsonArray pairs;
 const QJsonObject fields{{QStringLiteral("启用"),enabled},{QStringLiteral("成色设置栏"),0},{QStringLiteral("最低价格设置栏"),10},
  {QStringLiteral("最高价格设置栏"),600},{QStringLiteral("枪名设置栏"),product},{QStringLiteral("磨损度设置栏"),5},{QStringLiteral("限量设置栏"),0}};
 for(auto i=fields.begin();i!=fields.end();++i)pairs.append(QJsonArray{i.key()+"_"+QString::number(index),i.value()});return pairs;
}
QByteArray document(const QJsonArray& pairs){return encode(QJsonObject{{QStringLiteral("自动收藏-任务配置"),pairs}});}
}
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);int n=0,fail=0;auto check=[&](bool ok,const char* name){++n;fail+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<'\n';};
 QFile file(argc>1?QString::fromLocal8Bit(argv[1]):QString());check(file.open(QIODevice::ReadOnly),"catalog_open");
 const auto dictionary=QJsonDocument::fromJson(file.readAll()).object();
 auto b=document(row());auto decoded=decodeSavedValue(b);check(decoded["valid"].toBool() && decoded["consumed_bytes"].toInt()==b.size(),"entire_input_consumed");
 check(encode(decoded["root"])==b,"synthetic_byte_roundtrip");
 auto s=savedValueCollectionSnapshot(b,dictionary);check(s["ready"].toBool() && s["enabled_count"]==1,"enabled_rule_decoded");
 auto r=s["rows"].toArray()[0].toObject();check(r["product_name"]==QStringLiteral("AUG突击步枪-天命"),"real_id_dictionary_not_guess");
 check(r["season_label"]==QStringLiteral("棱镜攻势S2") && r["condition_label"]==QStringLiteral("成色S"),"condition_and_market_season_not_filename");
 check(r["price_min"]=="10" && r["price_max"]=="600" && r["max_wear"]=="5","named_numeric_fields_preserved");
 check(s["mode"]=="collect_only" && !s["purchase_phase_enabled"].toBool(),"collection_phase_separate_from_watchlist_purchase");
 check(r["ownership"]=="any" && r["grade"]=="any","unspecified_filters_not_invented_from_color");
 auto unknown=savedValueCollectionSnapshot(document(row(0,99999)),dictionary);check(unknown["valid"].toBool() && !unknown["ready"].toBool(),"unknown_enabled_id_blocks_readiness");
 unknown=savedValueCollectionSnapshot(document(row(0,99999,false)),dictionary);check(unknown["valid"].toBool() && !unknown["ready"].toBool() && unknown["unresolved_rows"].toArray().isEmpty(),"disabled_unknown_row_preserved_not_activated");
 auto pairs=row(10);for(const auto& v:row(2))pairs.append(v);s=savedValueCollectionSnapshot(document(pairs),dictionary);
 check(s["rows"].toArray()[0].toObject()["row_index"]==2 && s["rows"].toArray()[1].toObject()["row_index"]==10,"row_order_numeric_not_lexicographic");
 pairs=row();pairs.append(pairs[0]);check(!savedValueCollectionSnapshot(document(pairs),dictionary)["valid"].toBool(),"duplicate_task_field_rejected");
 pairs=row();pairs.removeAt(0);check(!savedValueCollectionSnapshot(document(pairs),dictionary)["valid"].toBool(),"missing_field_not_filled_from_demo");
 pairs=row();auto pair=pairs[0].toArray();pair[0]="unknown_0";pairs[0]=pair;check(!savedValueCollectionSnapshot(document(pairs),dictionary)["valid"].toBool(),"unknown_task_field_rejected");
 for(int i=0;i<b.size();++i)check(!decodeSavedValue(b.left(i))["valid"].toBool(),"every_truncated_prefix_rejected");
 check(!decodeSavedValue(b+char(0))["valid"].toBool(),"trailing_data_rejected");
 check(!decodeSavedValue(QByteArray::fromHex("02010701ff00"))["valid"].toBool(),"invalid_utf8_rejected");
 check(!decodeSavedValue(QByteArray::fromHex("020207016104010701610402"))["valid"].toBool(),"duplicate_map_key_rejected");
 check(!decodeSavedValue(QByteArray::fromHex("0201070161050000000000000000"))["valid"].toBool(),"unobserved_scalar_tag_not_guessed");
 check(!decodeSavedValue(QByteArray::fromHex("0201070161c4ffffffffffffffff"))["valid"].toBool(),"integer_precision_overflow_rejected");
 check(!decodeSavedValue(QByteArray(8*1024*1024+1,0))["valid"].toBool(),"input_budget");
 QByteArray deep;for(int i=0;i<20;++i)deep+=QByteArray::fromHex("0301");deep.append(char(1));check(!decodeSavedValue(deep)["valid"].toBool(),"recursion_budget");
 check(!savedValueCollectionSnapshot(b,{})["valid"].toBool(),"missing_dictionary_not_execute");
 const auto wideInput=QByteArray::fromHex("020107016144e600");const auto wide=decodeSavedValue(wideInput);
 const auto prefix=wide["noncanonical_prefixes"].toArray().at(0).toObject();
 check(wide["valid"].toBool() && wide["root"].toObject()["a"]==230 && prefix["offset"]==5 && prefix["width"]==2,"nonminimal_prefix_is_valid_and_recorded");
 QByteArray wideRebuilt;appendObserved(wideRebuilt,wide["root"],{{5,2}});
 check(wideRebuilt==wideInput,"nonminimal_prefix_exact_reencoding");
 for(const auto& id:QStringList{"10504","10505","10506","10602","10603","10604","11102","11103","11104"})
  check(dictionary["products"].toObject().contains(id),"all_nine_requested_product_ids_resolve");
 if(argc>2){QFile input(QString::fromLocal8Bit(argv[2]));check(input.open(QIODevice::ReadOnly),"provided_file_open");const auto original=input.readAll();
  const auto actual=savedValueCollectionSnapshot(original,dictionary);check(actual["valid"].toBool() && actual["ready"].toBool(),"provided_file_resolved");
  check(actual["row_count"]==50 && actual["enabled_count"]==21 && actual["decoded_bytes"]==8926,"provided_file_exact_counts");
  const auto parsed=decodeSavedValue(original);const auto entries=parsed["root_entries"].toArray();
  QMap<qsizetype,int> widths;
  for(const auto& v:parsed["noncanonical_prefixes"].toArray()){const auto item=v.toObject();widths.insert(item["offset"].toInteger(),item["width"].toInt());}
  check(widths.size()==12 && widths.value(3556)==2,"provided_nonminimal_integer_widths_preserved");
  auto rebuilt=header(2,entries.size());
  for(const auto& entry:entries){const auto pair=entry.toArray();appendObserved(rebuilt,pair[0],widths);appendObserved(rebuilt,pair[1],widths);}
  check(rebuilt==original,"provided_file_exact_roundtrip_no_runtime_needed");
 }
 std::cout<<"SAVEDVALUE_TESTS="<<(fail?"FAIL":"PASS")<<"; assertions="<<n<<"; failures="<<fail<<"; specimen_executed=false\n";return fail?1:0;
}
