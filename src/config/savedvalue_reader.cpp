#include "savedvalue_reader.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QMap>
#include <QSet>
#include <QStringDecoder>
#include <cmath>
#include <stdexcept>

namespace relink::config {
namespace {
class Reader {
public:
    explicit Reader(const QByteArray& input):b(input){}
    qsizetype pos=0;int nodes=0;
    QJsonArray rootEntries;
    QJsonArray noncanonicalPrefixes;
    QJsonValue value(int depth=0){
        if(depth>12 || ++nodes>20000)fail("E_SAVEDVALUE_COMPLEXITY");
        const auto start=pos;const unsigned tag=byte();
        if(tag<=1)return bool(tag);
        const unsigned kind=tag&63,width=1u<<(tag>>6);
        if(kind!=2 && kind!=3 && kind!=4 && kind!=7)fail("E_SAVEDVALUE_TAG");
        quint64 n=0;for(unsigned i=0;i<width;++i)n|=quint64(byte())<<(i*8);
        if(n>9007199254740991ULL)fail("E_SAVEDVALUE_INTEGER_RANGE");
        unsigned minimal=1;while(minimal<8 && n>=(quint64(1)<<(minimal*8)))minimal*=2;
        if(width!=minimal)noncanonicalPrefixes.append(QJsonObject{{"offset",start},{"kind",int(kind)},{"width",int(width)}});
        if(kind==4)return qint64(n);
        if(kind==7){
            if(n>65536 || n>quint64(b.size()-pos))fail("E_SAVEDVALUE_STRING_LENGTH");
            const auto bytes=b.mid(pos,qsizetype(n));pos+=qsizetype(n);
            QStringDecoder decoder(QStringDecoder::Utf8);const QString text=decoder(bytes);
            if(decoder.hasError() || text.contains(QChar(0)))fail("E_SAVEDVALUE_UTF8");
            return text;
        }
        if(n>5000)fail("E_SAVEDVALUE_CONTAINER_LENGTH");
        if(kind==3){QJsonArray out;for(quint64 i=0;i<n;++i)out.append(value(depth+1));return out;}
        QJsonObject out;
        for(quint64 i=0;i<n;++i){const auto key=value(depth+1);
            if(!key.isString() || key.toString().isEmpty() || out.contains(key.toString()))fail("E_SAVEDVALUE_KEY");
            const auto item=value(depth+1);out[key.toString()]=item;
            if(depth==0)rootEntries.append(QJsonArray{key,item});
        }
        return out;
    }
private:
    const QByteArray& b;
    unsigned byte(){if(pos>=b.size())fail("E_SAVEDVALUE_TRUNCATED");return static_cast<unsigned char>(b[pos++]);}
    [[noreturn]] void fail(const char* reason){throw std::runtime_error(reason);}
};
bool whole(const QJsonValue& value,qint64 max){return value.isDouble() && std::isfinite(value.toDouble())
    && value.toDouble()>=0 && value.toDouble()<=max && std::floor(value.toDouble())==value.toDouble();}
QString normalizedName(QString value){value.remove(QRegularExpression(QStringLiteral("\\s+")));return value;}
}
QJsonObject decodeSavedValue(const QByteArray& bytes){
    QJsonObject result{{"valid",false},{"source_sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())}};
    if(bytes.isEmpty() || bytes.size()>8*1024*1024){result["error"]="E_SAVEDVALUE_SIZE";return result;}
    Reader r(bytes);
    try{
        const auto root=r.value();
        if(!root.isObject()){result["error"]="E_SAVEDVALUE_ROOT";return result;}
        if(r.pos!=bytes.size()){result["error"]="E_SAVEDVALUE_TRAILING_DATA";return result;}
        result["root"]=root;result["root_entries"]=r.rootEntries;result["noncanonical_prefixes"]=r.noncanonicalPrefixes;
        result["valid"]=true;result["consumed_bytes"]=r.pos;result["nodes"]=r.nodes;
    }catch(const std::exception& e){result["error"]=QString::fromLatin1(e.what());result["error_offset"]=r.pos;}
    return result;
}
QJsonObject savedValueCollectionSnapshot(const QByteArray& bytes,const QJsonObject& dictionary){
    const auto decoded=decodeSavedValue(bytes);
    QJsonObject out{{"schema","savedvalue-collection-snapshot-v1"},{"valid",false},{"ready",false},
        {"source_sha256",decoded["source_sha256"]},{"mode","collect_only"},{"purchase_phase_enabled",false}};
    if(!decoded["valid"].toBool()){out["error"]=decoded["error"];out["error_offset"]=decoded["error_offset"];return out;}
    if(dictionary["schema"]!="relink-savedvalue-dictionary-v1" || !dictionary["products"].isObject() || !dictionary["conditions"].isObject()){
        out["error"]="E_SAVEDVALUE_DICTIONARY";return out;}
    const auto root=decoded["root"].toObject();
    const auto taskPairs=root[QStringLiteral("自动收藏-任务配置")];
    if(!taskPairs.isArray() || taskPairs.toArray().size()>3500){out["error"]="E_SAVEDVALUE_TASK_SECTION";return out;}
    QMap<int,QJsonObject> indexed;QSet<QString> seen;
    const QStringList fields{QStringLiteral("启用"),QStringLiteral("成色设置栏"),QStringLiteral("最低价格设置栏"),QStringLiteral("最高价格设置栏"),
        QStringLiteral("枪名设置栏"),QStringLiteral("磨损度设置栏"),QStringLiteral("限量设置栏")};
    const QRegularExpression keyPattern(QStringLiteral("^(.+)_([0-9]{1,3})$"));
    for(const auto& v:taskPairs.toArray()){
        const auto pair=v.toArray();
        if(pair.size()!=2 || !pair[0].isString()){out["error"]="E_SAVEDVALUE_TASK_PAIR";return out;}
        const auto key=pair[0].toString();const auto match=keyPattern.match(key);
        if(!match.hasMatch() || seen.contains(key) || !fields.contains(match.captured(1)) || match.captured(2).toInt()>499
            || match.captured(2)!=QString::number(match.captured(2).toInt())){out["error"]="E_SAVEDVALUE_TASK_KEY";return out;}
        seen.insert(key);indexed[match.captured(2).toInt()][match.captured(1)]=pair[1];
    }
    QJsonArray rows,blocked;int enabled=0;
    const auto products=dictionary["products"].toObject(),conditions=dictionary["conditions"].toObject();
    for(auto it=indexed.cbegin();it!=indexed.cend();++it){
        const auto raw=it.value();
        if(raw.size()!=7 || !raw[QStringLiteral("启用")].isBool()){out["error"]="E_SAVEDVALUE_TASK_FIELDS";return out;}
        for(const auto& field:fields){if(field==QStringLiteral("启用"))continue;
            if(!whole(raw[field],9007199254740991LL)){out["error"]="E_SAVEDVALUE_TASK_NUMBER";return out;}}
        if(raw[QStringLiteral("最低价格设置栏")].toDouble()>raw[QStringLiteral("最高价格设置栏")].toDouble()){
            out["error"]="E_SAVEDVALUE_PRICE_RANGE";return out;}
        const auto productId=QString::number(raw[QStringLiteral("枪名设置栏")].toInteger());
        const auto conditionId=QString::number(raw[QStringLiteral("成色设置栏")].toInteger());
        const bool active=raw[QStringLiteral("启用")].toBool();enabled+=active;
        const auto product=products[productId].toObject();
        QJsonObject row{{"row_index",it.key()},{"enabled",active},{"source_product_id",productId},{"source_condition_id",conditionId},
            {"source_fields",raw},{"price_min",QString::number(raw[QStringLiteral("最低价格设置栏")].toInteger())},
            {"price_max",QString::number(raw[QStringLiteral("最高价格设置栏")].toInteger())},
            {"max_wear",QString::number(raw[QStringLiteral("磨损度设置栏")].toInteger())},
            {"limit_raw",raw[QStringLiteral("限量设置栏")]},{"ownership","any"},{"grade","any"}};
        if(!product.isEmpty() && conditions[conditionId].isString()){
            row["display_name"]=product["display_name"];row["product_name"]=normalizedName(product["name"].toString());
            row["season_label"]=normalizedName(product["season_label"].toString());row["condition_label"]=conditions[conditionId];
            row["dictionary_resolved"]=!row["product_name"].toString().isEmpty() && !row["season_label"].toString().isEmpty();
        }else row["dictionary_resolved"]=false;
        if(active && !row["dictionary_resolved"].toBool())blocked.append(it.key());
        rows.append(row);
    }
    out["valid"]=true;out["ready"]=enabled>0 && blocked.isEmpty();out["rows"]=rows;out["row_count"]=rows.size();
    out["enabled_count"]=enabled;out["unresolved_rows"]=blocked;
    auto other=root;other.remove(QStringLiteral("自动收藏-任务配置"));out["inactive_settings_metadata"]=other;
    out["decoded_bytes"]=decoded["consumed_bytes"];out["dictionary_source_sha256"]=dictionary["source_span_sha256"];
    return out;
}
} // namespace relink::config
