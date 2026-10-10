#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QRect>
#include <QRectF>
#include <QVector>
#include <algorithm>
#include <cmath>

namespace relink::vision {
// This only decides whether to re-read actual pixels. It does not parse an
// amount, repair OCR characters, use a configured price, or accept a candidate.
inline bool collectionPriceWordsAreNumeric(const QJsonArray& words) {
    static const QRegularExpression token(QStringLiteral("^[0-9,.]+$"));
    static const QRegularExpression digit(QStringLiteral("[0-9]"));
    bool hasDigit=false;
    for(const auto& value:words){
        if(!value.isObject())return false;
        const auto text=value.toObject()["text"].toString().normalized(QString::NormalizationForm_KC).trimmed();
        if(text.isEmpty() || !token.match(text).hasMatch())return false;
        hasDigit=hasDigit ||digit.match(text).hasMatch();
    }
    return hasDigit;
}

// Keep the candidate parser's exact Chinese prefix + observed ASCII grade.
// This predicate only gates OCR refinement; it never infers a configured grade.
inline bool collectionConditionWordsAreLiteral(const QJsonArray& words) {
    if(words.isEmpty() ||words.size()>16)return false;
    QVector<QJsonObject> ordered;
    for(const auto& value:words){
        if(!value.isObject())return false;
        const auto word=value.toObject();
        if(!word["text"].isString() ||word["text"].toString().isEmpty())return false;
        for(const auto* name:{"x","y","width","height"})
            if(!word[name].isDouble() ||!std::isfinite(word[name].toDouble()))return false;
        if(word["width"].toDouble()<=0 ||word["height"].toDouble()<=0)return false;
        ordered.append(word);
    }
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){
        return a["x"].toDouble()<b["x"].toDouble();});
    QString text;for(const auto& word:ordered)text+=word["text"].toString();
    static const QRegularExpression literal(QStringLiteral("\\A成色[sSaAbBcC]\\z"));
    return literal.match(text).hasMatch();
}

// run05/000086 measured a 0.468 px wide "|" at the condition ROI's left
// edge before the independently observed 成/色/C glyphs. Plan a fresh OCR of
// actual same-frame pixels with that two-pixel edge strip excluded. Returning
// a rectangle does NOT accept the remaining old words or repair any text.
// The caller must record both attempts and accept only the new full literal.
inline QRect collectionConditionRefinementRegion(const QJsonArray& words,const QRect& conditionBounds) {
    if(conditionBounds.isEmpty() ||conditionBounds.x()<0 ||conditionBounds.y()<0
        ||conditionBounds.width()<=4 ||words.isEmpty() ||words.size()>16
        ||collectionConditionWordsAreLiteral(words))return {};
    const QRectF outer(conditionBounds);
    const QRect reread=conditionBounds.adjusted(2,0,0,0);
    int edgeCount=0;
    QJsonArray retained;
    for(const auto& value:words){
        if(!value.isObject())return {};
        const auto word=value.toObject();
        if(!word["text"].isString() ||word["text"].toString().isEmpty())return {};
        for(const auto* name:{"x","y","width","height"})
            if(!word[name].isDouble() ||!std::isfinite(word[name].toDouble()))return {};
        const QRectF box(word["x"].toDouble(),word["y"].toDouble(),
            word["width"].toDouble(),word["height"].toDouble());
        if(box.width()<=0 ||box.height()<=0 ||!outer.contains(box))return {};
        if(word["text"].toString()==QStringLiteral("|")){
            // Only the actual recorded kind of detached thin left-edge line;
            // internal bars, letters I/l and multiple lines are not repaired.
            if(++edgeCount!=1 ||box.left()>conditionBounds.x()+1
                ||box.width()>1 ||box.height()<8*box.width())return {};
        }else{
            // Keep two physical pixels before every retained observed glyph;
            // a crop that may touch a real glyph is not proposed.
            if(box.left()<reread.x()+2 ||!QRectF(reread).contains(box))return {};
            retained.append(word);
        }
    }
    if(edgeCount!=1 ||!collectionConditionWordsAreLiteral(retained))return {};
    return reread;
}
} // namespace relink::vision
