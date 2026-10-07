#include "config/v1_adapter.h"

#include "business/value_types.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <cmath>
#include <limits>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace relink::config {
namespace {

using relink::business::DecimalParsePolicy;
using relink::business::DecimalValue;
using relink::business::parseDecimal;

constexpr qint64 kMaxBytes = 8 * 1024 * 1024;
constexpr int kMaxRows = 10000;
constexpr int kMaxLineChars = 65536;

QJsonValue nullValue() { return QJsonValue(QJsonValue::Null); }

QJsonObject diagnostic(const QString& severity, const QString& code,
                      int line = -1, int column = -1,
                      const QString& field = {},
                      const QString& message = {}) {
    QJsonObject result{{QStringLiteral("severity"), severity},
                       {QStringLiteral("code"), code},
                       {QStringLiteral("line"), line > 0 ? QJsonValue(line) : nullValue()},
                       {QStringLiteral("column"), column > 0 ? QJsonValue(column) : nullValue()},
                       {QStringLiteral("field"), field.isEmpty() ? nullValue() : QJsonValue(field)},
                       {QStringLiteral("message"), message.isEmpty() ? code : message}};
    return result;
}

void appendDiagnostic(QJsonArray& list, const QString& severity, const QString& code,
                      int line = -1, int column = -1,
                      const QString& field = {}, const QString& message = {}) {
    if (list.size() < 1000)
        list.append(diagnostic(severity, code, line, column, field, message));
}

QJsonObject decimalJson(const DecimalValue& value) {
    return {{QStringLiteral("unscaled"), value.unscaled},
            {QStringLiteral("scale"), static_cast<int>(value.scale)}};
}

QJsonObject moneyJson(const DecimalValue& value, const QString& unit = {}) {
    return {{QStringLiteral("value"), decimalJson(value)},
            {QStringLiteral("unit"), unit.isEmpty() ? nullValue() : QJsonValue(unit)}};
}

QString stableId(const QString& oldId, const QString& kind, int index,
                 QSet<QString>& used) {
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$"));
    QString candidate = oldId;
    if (!valid.match(candidate).hasMatch() || candidate.isEmpty())
        candidate = QStringLiteral("legacy-%1-%2").arg(kind).arg(index + 1);
    const QString base = candidate;
    int suffix = 2;
    while (used.contains(candidate)) candidate = base + QStringLiteral("-") + QString::number(suffix++);
    used.insert(candidate);
    return candidate;
}

QJsonValue redactedCopy(const QJsonValue& value, const QString& key = {}) {
    const QString lower = key.toLower();
    if (lower.contains(QStringLiteral("token")) || lower.contains(QStringLiteral("password")) ||
        lower.contains(QStringLiteral("secret")) || lower.contains(QStringLiteral("credential")))
        return QStringLiteral("<redacted>");
    if (value.isArray()) {
        QJsonArray result;
        for (const auto& item : value.toArray()) result.append(redactedCopy(item));
        return result;
    }
    if (value.isObject()) {
        QJsonObject result;
        const auto object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
            result.insert(it.key(), redactedCopy(it.value(), it.key()));
        return result;
    }
    return value;
}

QString jsonToken(const QJsonValue& value) {
    if (value.isString()) return value.toString();
    if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isNull() || value.isUndefined()) return QStringLiteral("null");
    if (value.isDouble()) return QString::number(value.toDouble(), 'g', 17);
    return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
}

QString normalizedEncoding(const QByteArray& bytes, QByteArray& payload, QString& encoding,
                          QJsonArray& diagnostics, const QString& requested = {}) {
    payload = bytes;
    if (bytes.startsWith("\xEF\xBB\xBF")) {
        payload = bytes.mid(3);
        encoding = QStringLiteral("UTF-8-BOM");
        return QString::fromUtf8(payload);
    }
    if (bytes.startsWith("\xFF\xFE")) {
        payload = bytes.mid(2);
        encoding = QStringLiteral("UTF-16LE-BOM");
        if (payload.size() % 2 != 0) {
            appendDiagnostic(diagnostics, QStringLiteral("error"), QStringLiteral("BAD_ENCODING"), -1, -1, {}, QStringLiteral("UTF-16LE payload has an odd byte count"));
            return {};
        }
        const auto* chars = reinterpret_cast<const char16_t*>(payload.constData());
        return QString::fromUtf16(chars, payload.size() / 2);
    }
    if (requested == QStringLiteral("GB18030")) {
        encoding = QStringLiteral("GB18030-explicit");
#ifdef Q_OS_WIN
        const int size = MultiByteToWideChar(54936 /* CP54936 */, MB_ERR_INVALID_CHARS,
                                             payload.constData(), payload.size(), nullptr, 0);
        if (size <= 0) {
            appendDiagnostic(diagnostics, QStringLiteral("error"), QStringLiteral("BAD_ENCODING"), -1, -1, {}, QStringLiteral("input is not valid GB18030"));
            return {};
        }
        QVector<wchar_t> wide(size);
        MultiByteToWideChar(54936, MB_ERR_INVALID_CHARS, payload.constData(), payload.size(),
                            wide.data(), size);
        return QString::fromWCharArray(wide.constData(), size);
#else
        // Non-Windows builds do not ship a Qt text codec in this target. Preserve the
        // explicit encoding marker and use the platform decoder as a deterministic
        // fallback for test fixtures.
        const QString result = QString::fromLocal8Bit(payload);
        if (result.contains(QChar::ReplacementCharacter))
            appendDiagnostic(diagnostics, QStringLiteral("error"), QStringLiteral("BAD_ENCODING"), -1, -1, {}, QStringLiteral("input is not valid GB18030"));
        return result;
#endif
    }
    encoding = QStringLiteral("UTF-8");
    const QString text = QString::fromUtf8(payload);
    if (text.contains(QChar::ReplacementCharacter))
        appendDiagnostic(diagnostics, QStringLiteral("error"), QStringLiteral("BAD_ENCODING"), -1, -1, {}, QStringLiteral("input is not valid UTF-8"));
    return text;
}

bool decimalFromNumber(const QJsonValue& json, int maxScale, DecimalValue& output,
                      bool& normalized, QString& raw) {
    if (!json.isDouble()) return false;
    const double number = json.toDouble();
    if (!std::isfinite(number) || number < 0.0) return false;
    raw = QString::number(number, 'g', 17);
    const auto direct = parseDecimal(QStringView(raw), DecimalParsePolicy{24, maxScale});
    if (std::holds_alternative<DecimalValue>(direct)) {
        output = std::get<DecimalValue>(direct);
        return true;
    }
    // A schema-v1 JSON number has already passed through a binary double. Accept
    // only the familiar representational tail (e.g. 0.30000000000000004) when
    // rounding to the legacy field precision changes no intended value.
    const QString fixed = QString::number(number, 'f', maxScale);
    bool ok = false;
    const double rounded = fixed.toDouble(&ok);
    if (ok && std::fabs(number - rounded) <= 1e-9) {
        const auto roundedDecimal = parseDecimal(QStringView(fixed), DecimalParsePolicy{24, maxScale});
        if (std::holds_alternative<DecimalValue>(roundedDecimal)) {
            output = std::get<DecimalValue>(roundedDecimal);
            normalized = true;
            return true;
        }
    }
    return false;
}

bool decimalFromText(QString text, int maxScale, DecimalValue& output,
                     bool& groupingInvalid) {
    text = text.trimmed();
    groupingInvalid = false;
    if (text.contains(QLatin1Char(','))) {
        static const QRegularExpression grouped(QStringLiteral("^[1-9][0-9]{0,2}(?:,[0-9]{3})+(?:\\.[0-9]+)?$"));
        if (!grouped.match(text).hasMatch()) {
            groupingInvalid = true;
            return false;
        }
        text.remove(QLatin1Char(','));
    }
    const auto parsed = parseDecimal(QStringView(text), DecimalParsePolicy{24, maxScale});
    if (!std::holds_alternative<DecimalValue>(parsed)) return false;
    output = std::get<DecimalValue>(parsed);
    return true;
}

QString stringField(const QJsonObject& object, const QString& key) {
    const auto value = object.value(key);
    return value.isString() ? value.toString() : QString{};
}

QJsonObject selectorAny() { return {{QStringLiteral("op"), QStringLiteral("any")}}; }
QJsonObject selectorEq(const QString& value) {
    return {{QStringLiteral("op"), QStringLiteral("eq")}, {QStringLiteral("value"), value}};
}

QJsonObject defaultRunSettings() {
    return {
        {QStringLiteral("profile"), QStringLiteral("S11新赛季1103")},
        {QStringLiteral("hotkey"), QStringLiteral("F2")},
        {QStringLiteral("purchaseDelayMs"), 830}, {QStringLiteral("dynamicDelay"), false},
        {QStringLiteral("queueFullTrigger"), 1}, {QStringLiteral("queueFullStepMs"), 1.0},
        {QStringLiteral("publicityTrigger"), 1}, {QStringLiteral("publicityStepMs"), 1.0},
        {QStringLiteral("burstClick"), true}, {QStringLiteral("clickIntervalMs"), 10},
        {QStringLiteral("limitOrange"), 10}, {QStringLiteral("limitPurple"), 10}, {QStringLiteral("limitBlue"), 10},
        {QStringLiteral("refreshPage"), false}, {QStringLiteral("skipLotteryPage"), true},
        {QStringLiteral("skipSuccessPage"), true}, {QStringLiteral("autoCollect"), true},
        {QStringLiteral("collectOsd"), false}, {QStringLiteral("scheduleEnabled"), false},
        {QStringLiteral("scheduleStart"), QStringLiteral("09:00")}, {QStringLiteral("scheduleStop"), QStringLiteral("23:00")}
    };
}

bool validRunSetting(const QString& key, const QJsonValue& value) {
    const auto integerIn = [&](qint64 minimum, qint64 maximum) {
        if (!value.isDouble()) return false;
        const double number = value.toDouble();
        return std::isfinite(number) && std::floor(number) == number && number >= minimum && number <= maximum;
    };
    const auto decimalIn = [&](double minimum, double maximum, int decimalPlaces) {
        if (!value.isDouble()) return false;
        const double number = value.toDouble();
        if (!std::isfinite(number) || number < minimum || number > maximum) return false;
        const QString fixed = QString::number(number, 'f', decimalPlaces);
        bool ok = false;
        const double rounded = fixed.toDouble(&ok);
        return ok && std::fabs(number - rounded) <= 1e-9;
    };
    const auto boolean = [&]() { return value.isBool(); };
    if (key == QStringLiteral("profile")) return value.isString() && !value.toString().isEmpty() && value.toString().size() <= 40;
    if (key == QStringLiteral("hotkey")) return value.isString() && QRegularExpression(QStringLiteral("^F([1-9]|1[0-2])$")).match(value.toString()).hasMatch();
    if (key == QStringLiteral("purchaseDelayMs")) return integerIn(0, 60000);
    if (key == QStringLiteral("dynamicDelay") || key == QStringLiteral("burstClick") ||
        key == QStringLiteral("refreshPage") || key == QStringLiteral("skipLotteryPage") ||
        key == QStringLiteral("skipSuccessPage") || key == QStringLiteral("autoCollect") ||
        key == QStringLiteral("collectOsd") || key == QStringLiteral("scheduleEnabled")) return boolean();
    if (key == QStringLiteral("queueFullTrigger") || key == QStringLiteral("publicityTrigger")) return integerIn(1, 9999);
    if (key == QStringLiteral("queueFullStepMs") || key == QStringLiteral("publicityStepMs")) return decimalIn(0.0, 1000.0, 1);
    if (key == QStringLiteral("clickIntervalMs")) return integerIn(1, 10000);
    if (key == QStringLiteral("limitOrange") || key == QStringLiteral("limitPurple") || key == QStringLiteral("limitBlue")) return integerIn(0, 9999);
    if (key == QStringLiteral("scheduleStart") || key == QStringLiteral("scheduleStop"))
        return value.isString() && QRegularExpression(QStringLiteral("^([01][0-9]|2[0-3]):[0-5][0-9]$")).match(value.toString()).hasMatch();
    return false;
}

void appendReview(QJsonArray& diagnostics, QStringList& codes, const QString& code,
                  const QString& field, int line, const QString& message) {
    if (!codes.contains(code)) codes.append(code);
    appendDiagnostic(diagnostics, QStringLiteral("review"), code, line, -1, field, message);
}

QJsonObject legacyRaw(const QString& sourceKind, const QString& hash, int line,
                      const QStringList& columns, const QJsonObject& unknown) {
    QJsonArray cols; for (const auto& c : columns) cols.append(c);
    return {{QStringLiteral("source_kind"), sourceKind}, {QStringLiteral("source_sha256"), hash},
            {QStringLiteral("source_line"), line > 0 ? QJsonValue(line) : nullValue()},
            {QStringLiteral("raw_line"), QString{}}, {QStringLiteral("columns"), cols},
            {QStringLiteral("unknown_json"), unknown}};
}

QJsonObject taskCandidate(const QString& taskId, const QString& name,
                          const QString& productId, const QString& hash, int line,
                          const QStringList& reviewCodes,
                          const QJsonObject& filters,
                          const QJsonObject& priceRange,
                          const QJsonValue& maxWear,
                          const QString& requestedSort,
                          const QJsonValue& quantity,
                          const QStringList& columns,
                          const QJsonObject& unknown,
                          const QString& productName,
                          bool synthetic = false) {
    QJsonArray codes; for (const auto& c : reviewCodes) codes.append(c);
    QJsonObject extensions{{QStringLiteral("x-product_name"), productName}};
    return {{QStringLiteral("task_id"), taskId}, {QStringLiteral("revision"), 1},
            {QStringLiteral("name"), name.isEmpty() ? taskId : name},
            {QStringLiteral("product_ref"), productId.isEmpty() ? nullValue() : QJsonValue(productId)},
            {QStringLiteral("enabled"), false}, {QStringLiteral("review_required"), true},
            {QStringLiteral("review_codes"), codes}, {QStringLiteral("filters"), filters},
            {QStringLiteral("price_range"), priceRange.isEmpty() ? nullValue() : QJsonValue(priceRange)},
            {QStringLiteral("max_wear"), maxWear}, {QStringLiteral("requested_sort"), requestedSort},
            {QStringLiteral("quantity_candidate"), quantity},
            {QStringLiteral("quantity_semantics"), quantity.isNull() ? QStringLiteral("none")
                : synthetic ? QStringLiteral("demo_count_only") : QStringLiteral("unreviewed")},
            {QStringLiteral("legacy_raw"), legacyRaw(QStringLiteral("legacy_v1"), hash, line, columns, unknown)},
            {QStringLiteral("extensions"), extensions}};
}

QJsonObject basePreview(const QString& format, const QString& hash, qint64 bytes,
                        const QString& encoding, const QJsonArray& rows,
                        const QJsonArray& diagnostics, const QJsonObject& extensions) {
    return {{QStringLiteral("kind"), QStringLiteral("LegacyImportPreview")},
            {QStringLiteral("source_format"), format}, {QStringLiteral("source_sha256"), hash},
            {QStringLiteral("source_bytes"), bytes}, {QStringLiteral("encoding"), encoding},
            {QStringLiteral("rows"), rows}, {QStringLiteral("diagnostics"), diagnostics},
            {QStringLiteral("committable"), false}, {QStringLiteral("extensions"), extensions}};
}

QJsonObject makeFilters(const QString& season, const QString& ownership, const QString& grade,
                        const QString& condition, const QString& publicity, const QString& rarity) {
    auto selector = [](const QString& value) { return value.isEmpty() ? selectorAny() : selectorEq(value); };
    return {{QStringLiteral("season"), selector(season)}, {QStringLiteral("ownership"), selector(ownership)},
            {QStringLiteral("grade"), selector(grade)}, {QStringLiteral("condition"), selector(condition)},
            {QStringLiteral("publicity"), selector(publicity)}, {QStringLiteral("rarity"), selector(rarity)}};
}

bool unlimitedToken(const QString& value) {
    const QString v = value.trimmed();
    return v.isEmpty() || v == QStringLiteral("不限") || v == QStringLiteral("全系列") ||
           v.compare(QStringLiteral("any"), Qt::CaseInsensitive) == 0 || v == QStringLiteral("*");
}

QString conditionToken(const QString& value) {
    const QString v = value.trimmed();
    for (const auto& token : {QStringLiteral("S"), QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")})
        if (v == token || v.endsWith(token)) return token;
    return {};
}

QString mapOwnership(const QString& value) {
    const QString v = value.trimmed();
    if (v == QStringLiteral("owned") || v == QStringLiteral("已拥有")) return QStringLiteral("owned");
    if (v == QStringLiteral("unowned") || v == QStringLiteral("未拥有")) return QStringLiteral("unowned");
    return {};
}

QString mapPublicity(const QString& value) {
    const QString v = value.trimmed();
    if (v == QStringLiteral("active")) return QStringLiteral("active");
    if (v == QStringLiteral("ended")) return QStringLiteral("ended");
    if (v == QStringLiteral("none")) return QStringLiteral("none");
    return {};
}

QString mapSort(const QString& value) {
    const QString v = value.trimmed();
    if (unlimitedToken(v) || v == QStringLiteral("默认排序") || v == QStringLiteral("default")) return QStringLiteral("default");
    if (v == QStringLiteral("价格升序") || v == QStringLiteral("price_asc")) return QStringLiteral("price_asc");
    if (v == QStringLiteral("价格降序") || v == QStringLiteral("price_desc")) return QStringLiteral("price_desc");
    if (v == QStringLiteral("稀有度升序") || v == QStringLiteral("rarity_asc")) return QStringLiteral("rarity_asc");
    if (v == QStringLiteral("稀有度降序") || v == QStringLiteral("rarity_desc")) return QStringLiteral("rarity_desc");
    return QStringLiteral("unknown");
}

} // namespace

QJsonObject previewV1(const QByteArray& bytes) {
    QJsonArray rootDiagnostics;
    if (bytes.size() > kMaxBytes) {
        appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("FILE_TOO_LARGE"), -1, -1, {}, QStringLiteral("input exceeds 8 MiB"));
        return basePreview(QStringLiteral("schema_v1"), sourceHash(bytes), bytes.size(), QStringLiteral("UTF-8"), {}, rootDiagnostics, {});
    }

    QByteArray payload;
    QString encoding;
    const QString text = normalizedEncoding(bytes, payload, encoding, rootDiagnostics);
    if (rootDiagnostics.size() > 0 && rootDiagnostics.at(0).toObject().value(QStringLiteral("severity")).toString() == QStringLiteral("error"))
        return basePreview(QStringLiteral("schema_v1"), sourceHash(bytes), bytes.size(), encoding, {}, rootDiagnostics, {});

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("JSON_INVALID"), -1, -1, {}, parseError.errorString());
        return basePreview(QStringLiteral("schema_v1"), sourceHash(bytes), bytes.size(), encoding, {}, rootDiagnostics, {});
    }
    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("schema_version")).isDouble() || root.value(QStringLiteral("schema_version")).toDouble() != 1.0) {
        appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("VERSION_UNSUPPORTED"), -1, -1, QStringLiteral("/schema_version"), QStringLiteral("schema_version must be 1"));
        return basePreview(QStringLiteral("schema_v1"), sourceHash(bytes), bytes.size(), encoding, {}, rootDiagnostics, {});
    }
    if (!root.value(QStringLiteral("demo")).isBool()) {
        appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("V1_DEMO_FLAG_INVALID"), -1, -1, QStringLiteral("/demo"), QStringLiteral("schema v1 demo must be a boolean"));
        return basePreview(QStringLiteral("schema_v1"), sourceHash(bytes), bytes.size(), encoding, {}, rootDiagnostics, {});
    }
    if (!root.value(QStringLiteral("skins")).isArray() || !root.value(QStringLiteral("tasks")).isArray()) {
        appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("SCHEMA_INVALID"), -1, -1, {}, QStringLiteral("skins and tasks must be arrays"));
        return basePreview(QStringLiteral("schema_v1"), sourceHash(bytes), bytes.size(), encoding, {}, rootDiagnostics, {});
    }

    const bool synthetic = root.value(QStringLiteral("demo")).toBool();
    const QString hash = sourceHash(bytes);
    const auto skins = root.value(QStringLiteral("skins")).toArray();
    const auto tasks = root.value(QStringLiteral("tasks")).toArray();
    if (skins.size() > kMaxRows || tasks.size() > kMaxRows) {
        appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("TOO_MANY_ROWS"), -1, -1, {}, QStringLiteral("v1 arrays exceed 10000 rows"));
        return basePreview(QStringLiteral("schema_v1"), hash, bytes.size(), encoding, {}, rootDiagnostics, {});
    }

    QSet<QString> usedSkinIds, usedTaskIds;
    QJsonObject skinMap, taskMap, catalog;
    QJsonArray catalogArray;
    for (int i = 0; i < skins.size(); ++i) {
        if (!skins.at(i).isObject()) {
            appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("SCHEMA_INVALID"), i + 1, -1, QStringLiteral("/skins"), QStringLiteral("skin row must be an object"));
            continue;
        }
        const QJsonObject skin = skins.at(i).toObject();
        const QString oldId = stringField(skin, QStringLiteral("id"));
        if (!oldId.isEmpty() && skinMap.contains(oldId))
            appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("DUPLICATE_ID"), i + 1, -1, QStringLiteral("/skins/%1/id").arg(i), QStringLiteral("duplicate legacy skin id"));
        const QString id = stableId(oldId, QStringLiteral("skin"), i, usedSkinIds);
        skinMap.insert(oldId, id);
        QJsonObject skinExtensions{{QStringLiteral("x-followed"), skin.value(QStringLiteral("followed")).toBool()},
                                   {QStringLiteral("x-condition"), stringField(skin, QStringLiteral("condition"))}};
        for (const auto& key : {QStringLiteral("catalogProductId"), QStringLiteral("menuColor"),
                                QStringLiteral("variant"), QStringLiteral("skinSeries"), QStringLiteral("dataSource")}) {
            if (!skin.contains(key)) continue;
            if (!skin.value(key).isString() || skin.value(key).toString().size() > 4096)
                appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("SKIN_METADATA_INVALID"),
                                 i + 1, -1, QStringLiteral("/skins/%1/%2").arg(i).arg(key),
                                 QStringLiteral("skin metadata must be text of at most 4096 characters"));
            else skinExtensions.insert(QStringLiteral("x-") + key, skin.value(key));
        }
        for (const auto& key : {QStringLiteral("priceKnown"), QStringLiteral("wearKnown"), QStringLiteral("changeKnown")}) {
            if (!skin.contains(key)) continue;
            if (!skin.value(key).isBool())
                appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("SKIN_METADATA_INVALID"),
                                 i + 1, -1, QStringLiteral("/skins/%1/%2").arg(i).arg(key),
                                 QStringLiteral("market field availability must be boolean"));
            else skinExtensions.insert(QStringLiteral("x-") + key, skin.value(key));
        }
        // Saved market values are metadata, never fresh observations; preserve
        // them and their availability flags for lossless configuration migration.
        QJsonObject marketMetadata;
        for (const auto& key : {QStringLiteral("wear"), QStringLiteral("price"), QStringLiteral("change")})
            if (skin.contains(key)) marketMetadata.insert(key, skin.value(key));
        skinExtensions.insert(QStringLiteral("x-saved-market-metadata"), marketMetadata);
        static const QRegularExpression seasonPattern(QStringLiteral("^S[1-9][0-9]*(?=\\s|$)"));
        const auto seasonMatch = seasonPattern.match(stringField(skin, QStringLiteral("series")));
        const QString collectionSeries = stringField(skin, QStringLiteral("skinSeries"));
        QJsonObject entry{
            {QStringLiteral("product_id"), id}, {QStringLiteral("name"), stringField(skin, QStringLiteral("name")).isEmpty() ? id : stringField(skin, QStringLiteral("name"))},
            {QStringLiteral("series"), !collectionSeries.isEmpty() ? QJsonValue(collectionSeries)
                : skin.contains(QStringLiteral("series")) ? QJsonValue(stringField(skin, QStringLiteral("series"))) : nullValue()},
            {QStringLiteral("season"), seasonMatch.hasMatch() ? QJsonValue(seasonMatch.captured()) : nullValue()},
            {QStringLiteral("grade"), nullValue()},
            {QStringLiteral("rarity"), skin.contains(QStringLiteral("rarity")) ? QJsonValue(stringField(skin, QStringLiteral("rarity"))) : nullValue()},
            {QStringLiteral("aliases"), QJsonArray{}}, {QStringLiteral("source_kind"), QStringLiteral("legacy_v1")},
            {QStringLiteral("extensions"), skinExtensions}
        };
        catalogArray.append(entry);
        catalog.insert(oldId, entry);
    }

    QJsonArray rows;
    for (int i = 0; i < tasks.size(); ++i) {
        const int line = i + 1;
        QJsonObject row{{QStringLiteral("line"), line}, {QStringLiteral("raw_line"), QString{}},
                        {QStringLiteral("columns"), QJsonArray{}}, {QStringLiteral("status"), QStringLiteral("review")},
                        {QStringLiteral("candidate"), nullValue()}, {QStringLiteral("diagnostics"), QJsonArray{}}};
        QJsonArray diagnostics;
        QStringList reviewCodes;
        if (!tasks.at(i).isObject()) {
            appendDiagnostic(diagnostics, QStringLiteral("error"), QStringLiteral("SCHEMA_INVALID"), line, -1, {}, QStringLiteral("task row must be an object"));
            row.insert(QStringLiteral("status"), QStringLiteral("invalid"));
            row.insert(QStringLiteral("diagnostics"), diagnostics);
            rows.append(row);
            continue;
        }
        const QJsonObject task = tasks.at(i).toObject();
        const QString oldTaskId = stringField(task, QStringLiteral("id"));
        if (!oldTaskId.isEmpty() && taskMap.contains(oldTaskId))
            appendDiagnostic(diagnostics, QStringLiteral("error"), QStringLiteral("DUPLICATE_ID"), line, -1, QStringLiteral("/tasks/%1/id").arg(i), QStringLiteral("duplicate legacy task id"));
        const QString taskId = stableId(oldTaskId, QStringLiteral("task"), i, usedTaskIds);
        taskMap.insert(oldTaskId, taskId);
        const QString oldSkinId = stringField(task, QStringLiteral("skinId"));
        const QString productId = skinMap.value(oldSkinId).toString();
        const QString productName = catalog.value(oldSkinId).toObject().value(QStringLiteral("name")).toString();
        appendReview(diagnostics, reviewCodes, QStringLiteral("PRICE_MAPPING_UNREVIEWED"), QStringLiteral("price_range"), line, QStringLiteral("legacy price values require review"));
        appendReview(diagnostics, reviewCodes, QStringLiteral("UNIT_UNREVIEWED"), QStringLiteral("price_range"), line, QStringLiteral("legacy schema has no reviewed unit"));
        appendReview(diagnostics, reviewCodes, QStringLiteral("PRODUCT_UNRESOLVED"), QStringLiteral("product_ref"), line, QStringLiteral("legacy skin identity needs catalog confirmation"));
        appendReview(diagnostics, reviewCodes, QStringLiteral("TAXONOMY_UNREVIEWED"), QStringLiteral("filters"), line, QStringLiteral("legacy taxonomy needs confirmation"));

        const QString condition = stringField(task, QStringLiteral("condition"));
        const QString conditionValue = unlimitedToken(condition) ? QString{} : conditionToken(condition);
        if (!condition.isEmpty() && conditionValue.isEmpty()) appendReview(diagnostics, reviewCodes, QStringLiteral("TAXONOMY_UNREVIEWED"), QStringLiteral("filters.condition"), line, QStringLiteral("unknown condition token"));

        QJsonObject priceRange;
        DecimalValue minPrice, maxPrice;
        QString rawMin, rawMax;
        bool normalizedMin = false, normalizedMax = false;
        bool minOk = decimalFromNumber(task.value(QStringLiteral("minPrice")), 2, minPrice, normalizedMin, rawMin);
        bool maxOk = decimalFromNumber(task.value(QStringLiteral("maxPrice")), 2, maxPrice, normalizedMax, rawMax);
        if (minOk && maxOk && relink::business::compareDecimal(minPrice, maxPrice) == relink::business::Ordering::Greater) {
            appendDiagnostic(diagnostics, QStringLiteral("error"), QStringLiteral("PRICE_RANGE_REVERSED"), line, 7, QStringLiteral("/minPrice"), QStringLiteral("minimum price exceeds maximum"));
        } else if (minOk && maxOk) {
            priceRange = {{QStringLiteral("min"), moneyJson(minPrice)}, {QStringLiteral("max"), moneyJson(maxPrice)}};
        } else {
            appendDiagnostic(diagnostics, QStringLiteral("error"), QStringLiteral("V1_PRECISION_INVALID"), line, 7, QStringLiteral("/minPrice"), QStringLiteral("legacy price must have at most two decimal places"));
        }
        if (normalizedMin || normalizedMax) appendDiagnostic(diagnostics, QStringLiteral("info"), QStringLiteral("V1_FLOAT_NOISE_NORMALIZED"), line, 7, QStringLiteral("/minPrice"), QStringLiteral("binary floating-point tail normalized to legacy precision"));

        QJsonValue maxWear = nullValue();
        if (task.contains(QStringLiteral("maxWear")) && !task.value(QStringLiteral("maxWear")).isNull()) {
            DecimalValue wear; QString rawWear; bool normalizedWear = false;
            if (decimalFromNumber(task.value(QStringLiteral("maxWear")), 6, wear, normalizedWear, rawWear)) {
                maxWear = decimalJson(wear);
                if (normalizedWear) appendDiagnostic(diagnostics, QStringLiteral("info"), QStringLiteral("V1_FLOAT_NOISE_NORMALIZED"), line, 9, QStringLiteral("/maxWear"), QStringLiteral("binary floating-point tail normalized to legacy precision"));
            } else {
                appendDiagnostic(diagnostics, QStringLiteral("error"), QStringLiteral("V1_PRECISION_INVALID"), line, 9, QStringLiteral("/maxWear"), QStringLiteral("legacy wear must have at most six decimal places"));
            }
        }
        const auto quantity = task.value(QStringLiteral("quantity"));
        QJsonValue quantityCandidate = nullValue();
        if (quantity.isDouble() && quantity.toInt() >= 1 && quantity.toInt() <= 9999 && std::floor(quantity.toDouble()) == quantity.toDouble()) quantityCandidate = quantity.toInt();
        QJsonObject unknown;
        for (auto it = task.constBegin(); it != task.constEnd(); ++it)
            if (!QStringList{QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("skinId"), QStringLiteral("minPrice"), QStringLiteral("maxPrice"), QStringLiteral("maxWear"), QStringLiteral("quantity"), QStringLiteral("enabled"), QStringLiteral("condition")}.contains(it.key()))
                unknown.insert(QStringLiteral("/tasks/%1/%2").arg(i).arg(it.key()), redactedCopy(it.value(), it.key()));
        row.insert(QStringLiteral("candidate"), taskCandidate(taskId, stringField(task, QStringLiteral("name")), {}, hash, line, reviewCodes,
                                                               makeFilters({}, {}, {}, conditionValue, {}, {}), priceRange, maxWear,
                                                               QStringLiteral("default"), quantityCandidate, {}, unknown, productName, synthetic));
        if (!minOk || !maxOk || (minOk && maxOk && relink::business::compareDecimal(minPrice, maxPrice) == relink::business::Ordering::Greater)) row.insert(QStringLiteral("status"), QStringLiteral("invalid"));
        row.insert(QStringLiteral("diagnostics"), diagnostics);
        rows.append(row);
    }

    QJsonObject runSettings = defaultRunSettings();
    QJsonObject settingSources;
    for (auto it = runSettings.constBegin(); it != runSettings.constEnd(); ++it) settingSources.insert(it.key(), QStringLiteral("default"));
    QJsonObject rawTokens, legacyUnknown;
    const QJsonObject run = root.value(QStringLiteral("run_settings")).toObject();
    const QStringList recognized = runSettings.keys();
    for (auto it = run.constBegin(); it != run.constEnd(); ++it) {
        if (!recognized.contains(it.key())) { legacyUnknown.insert(QStringLiteral("/run_settings/") + it.key(), redactedCopy(it.value(), it.key())); continue; }
        const bool valid = validRunSetting(it.key(), it.value());
        if (valid) { runSettings.insert(it.key(), it.value()); settingSources.insert(it.key(), QStringLiteral("v1")); rawTokens.insert(QStringLiteral("/run_settings/") + it.key(), jsonToken(it.value())); }
        else {
            settingSources.insert(it.key(), QStringLiteral("invalid"));
            appendDiagnostic(rootDiagnostics, QStringLiteral("review"), QStringLiteral("RUN_SETTING_INVALID"), -1, -1, QStringLiteral("/run_settings/") + it.key(), QStringLiteral("invalid legacy run setting; default retained"));
        }
    }
    QJsonObject unknownRoot;
    for (auto it = root.constBegin(); it != root.constEnd(); ++it)
        if (!QStringList{QStringLiteral("schema_version"), QStringLiteral("demo"), QStringLiteral("source"), QStringLiteral("skins"), QStringLiteral("tasks"), QStringLiteral("run_settings")}.contains(it.key()))
            unknownRoot.insert(QStringLiteral("/") + it.key(), redactedCopy(it.value(), it.key()));

    QJsonObject extensions{
        {QStringLiteral("x-target_mode"), synthetic ? QStringLiteral("demo") : QStringLiteral("configuration")},
        {QStringLiteral("x-catalog"), catalogArray},
        {QStringLiteral("x-ui_run_settings"), runSettings},
        {QStringLiteral("x-ui_run_settings_sources"), settingSources},
        {QStringLiteral("x-ui_run_settings_raw_tokens"), rawTokens},
        {QStringLiteral("x-ui_run_settings_legacy_unknown"), legacyUnknown},
        {QStringLiteral("x-id_map"), QJsonObject{{QStringLiteral("skins"), skinMap}, {QStringLiteral("tasks"), taskMap}}},
        {QStringLiteral("x-legacy_unknown_fields"), unknownRoot},
        {QStringLiteral("x-observation_source"), synthetic ? QStringLiteral("synthetic_demo") : QStringLiteral("configuration_metadata")},
        {QStringLiteral("x-live_market_data"), false}
    };
    return basePreview(QStringLiteral("schema_v1"), hash, bytes.size(), encoding, rows, rootDiagnostics, extensions);
}

QJsonObject preview13Columns(const QByteArray& bytes, const ImportOptions& options) {
    QJsonArray rootDiagnostics;
    if (bytes.size() > kMaxBytes) {
        appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("FILE_TOO_LARGE"), -1, -1, {}, QStringLiteral("input exceeds 8 MiB"));
        return basePreview(QStringLiteral("bbzps_13"), sourceHash(bytes), bytes.size(), QStringLiteral("UTF-8"), {}, rootDiagnostics, {});
    }
    if (options.container != QStringLiteral("text") && options.container != QStringLiteral("ini")) {
        appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("CONTAINER_UNSUPPORTED"), -1, -1, {}, QStringLiteral("container must be text or ini"));
        return basePreview(QStringLiteral("bbzps_13"), sourceHash(bytes), bytes.size(), QStringLiteral("UTF-8"), {}, rootDiagnostics, {});
    }
    QByteArray payload; QString encoding;
    const QString text = normalizedEncoding(bytes, payload, encoding, rootDiagnostics, options.encoding);
    if (!rootDiagnostics.isEmpty() && rootDiagnostics.at(0).toObject().value(QStringLiteral("severity")).toString() == QStringLiteral("error"))
        return basePreview(QStringLiteral("bbzps_13"), sourceHash(bytes), bytes.size(), encoding, {}, rootDiagnostics, {});

    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    if (lines.size() > kMaxRows) {
        appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("TOO_MANY_ROWS"), -1, -1, {}, QStringLiteral("input exceeds 10000 rows"));
        return basePreview(QStringLiteral("bbzps_13"), sourceHash(bytes), bytes.size(), encoding, {}, rootDiagnostics, {});
    }
    if (options.container == QStringLiteral("ini")) {
        QStringList taskLines;
        bool inSection = false; int sectionLine = 0; bool tasksSeen = false; QSet<QString> keys;
        for (int i = 0; i < lines.size(); ++i) {
            QString line = lines.at(i); if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
            const QString trimmed = line.trimmed();
            if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))) { inSection = trimmed.mid(1, trimmed.size() - 2).compare(QStringLiteral("SkinTasks"), Qt::CaseInsensitive) == 0; sectionLine = i + 1; continue; }
            if (!inSection || trimmed.isEmpty() || trimmed.startsWith(QLatin1Char(';')) || trimmed.startsWith(QLatin1Char('#'))) continue;
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq < 0) {
                if (tasksSeen && !line.isEmpty() && (line.at(0).isSpace() || !taskLines.isEmpty())) {
                    taskLines.append(line.trimmed());
                } else {
                    appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("ORPHAN_CONTINUATION"), i + 1, -1, {}, QStringLiteral("INI line is not a key/value pair"));
                }
                continue;
            }
            const QString key = line.left(eq).trimmed();
            if (key.compare(QStringLiteral("tasks"), Qt::CaseInsensitive) != 0) { continue; }
            if (keys.contains(key.toLower())) { appendDiagnostic(rootDiagnostics, QStringLiteral("error"), QStringLiteral("DUPLICATE_KEY"), i + 1, eq + 1, key, QStringLiteral("duplicate INI key")); continue; }
            keys.insert(key.toLower());
            const QString value = line.mid(eq + 1);
            tasksSeen = true;
            taskLines += value.split(QRegularExpression(QStringLiteral("\\r?\\n")), Qt::KeepEmptyParts);
            Q_UNUSED(sectionLine);
        }
        lines = taskLines;
    }

    const QString hash = sourceHash(bytes);
    QJsonArray rows;
    for (int i = 0; i < lines.size(); ++i) {
        QString raw = lines.at(i); if (raw.endsWith(QLatin1Char('\r'))) raw.chop(1);
        if (raw.size() > kMaxLineChars) {
            QJsonArray d; appendDiagnostic(d, QStringLiteral("error"), QStringLiteral("LINE_TOO_LONG"), i + 1, -1, {}, QStringLiteral("line exceeds 65536 characters"));
            rows.append(QJsonObject{{QStringLiteral("line"), i + 1}, {QStringLiteral("raw_line"), raw.left(kMaxLineChars)}, {QStringLiteral("columns"), QJsonArray{}}, {QStringLiteral("status"), QStringLiteral("invalid")}, {QStringLiteral("candidate"), nullValue()}, {QStringLiteral("diagnostics"), d}});
            continue;
        }
        const QStringList columns = raw.split(QLatin1Char('|'), Qt::KeepEmptyParts);
        QJsonArray columnJson; for (const auto& c : columns) columnJson.append(c.trimmed());
        QJsonArray diagnostics; QStringList reviewCodes;
        QJsonObject row{{QStringLiteral("line"), i + 1}, {QStringLiteral("raw_line"), raw}, {QStringLiteral("columns"), columnJson}, {QStringLiteral("status"), QStringLiteral("review")}, {QStringLiteral("candidate"), nullValue()}, {QStringLiteral("diagnostics"), diagnostics}};
        if (columns.size() != 13) {
            appendDiagnostic(diagnostics, QStringLiteral("error"), QStringLiteral("COLUMN_COUNT"), i + 1, -1, {}, QStringLiteral("expected exactly 13 columns"));
            row.insert(QStringLiteral("status"), QStringLiteral("invalid")); row.insert(QStringLiteral("diagnostics"), diagnostics); rows.append(row); continue;
        }
        const QStringList c = [&](){ QStringList out; for (const auto& x : columns) out << x.trimmed(); return out; }();
        QString season = unlimitedToken(c[1]) ? QString{} : c[1];
        QString ownership = mapOwnership(c[2]); if (!unlimitedToken(c[2]) && ownership.isEmpty()) appendReview(diagnostics, reviewCodes, QStringLiteral("TAXONOMY_UNREVIEWED"), QStringLiteral("ownership"), i + 1, QStringLiteral("unknown ownership token"));
        QString grade = unlimitedToken(c[3]) ? QString{} : c[3];
        QString condition = unlimitedToken(c[4]) ? QString{} : conditionToken(c[4]); if (!unlimitedToken(c[4]) && condition.isEmpty()) appendReview(diagnostics, reviewCodes, QStringLiteral("TAXONOMY_UNREVIEWED"), QStringLiteral("condition"), i + 1, QStringLiteral("unknown condition token"));
        QString publicity = unlimitedToken(c[5]) ? QString{} : mapPublicity(c[5]); if (!unlimitedToken(c[5]) && publicity.isEmpty()) appendReview(diagnostics, reviewCodes, QStringLiteral("TAXONOMY_UNREVIEWED"), QStringLiteral("publicity"), i + 1, QStringLiteral("unknown publicity token"));
        QString rarity = unlimitedToken(c[9]) ? QString{} : c[9];
        if (!unlimitedToken(c[1]) || !unlimitedToken(c[3]) || !unlimitedToken(c[9])) appendReview(diagnostics, reviewCodes, QStringLiteral("TAXONOMY_UNREVIEWED"), QStringLiteral("filters"), i + 1, QStringLiteral("taxonomy value requires review"));
        appendReview(diagnostics, reviewCodes, QStringLiteral("PRICE_MAPPING_UNREVIEWED"), QStringLiteral("price_range"), i + 1, QStringLiteral("legacy price columns require review"));
        appendReview(diagnostics, reviewCodes, QStringLiteral("UNIT_UNREVIEWED"), QStringLiteral("price_range"), i + 1, QStringLiteral("legacy input does not identify a reviewed unit"));
        appendReview(diagnostics, reviewCodes, QStringLiteral("PRODUCT_UNRESOLVED"), QStringLiteral("product_ref"), i + 1, QStringLiteral("product name needs catalog confirmation"));
        if (!c[11].isEmpty() || !c[12].isEmpty()) appendReview(diagnostics, reviewCodes, QStringLiteral("UNKNOWN_TRAILING_COLUMNS"), QStringLiteral("columns"), i + 1, QStringLiteral("columns 12 and 13 are preserved as raw metadata"));

        DecimalValue minPrice, maxPrice, wear; bool minGroup = false, maxGroup = false, wearGroup = false;
        const bool minOk = decimalFromText(c[6], 2, minPrice, minGroup);
        const bool maxOk = decimalFromText(c[7], 2, maxPrice, maxGroup);
        bool wearOk = c[8].isEmpty() || decimalFromText(c[8], 6, wear, wearGroup);
        if (!minOk || !maxOk) appendDiagnostic(diagnostics, QStringLiteral("error"), (minGroup || maxGroup) ? QStringLiteral("DECIMAL_GROUPING_INVALID") : QStringLiteral("DECIMAL_INVALID"), i + 1, 7, QStringLiteral("price"), QStringLiteral("invalid legacy price"));
        if (minOk && maxOk && relink::business::compareDecimal(minPrice, maxPrice) == relink::business::Ordering::Greater) appendDiagnostic(diagnostics, QStringLiteral("error"), QStringLiteral("PRICE_RANGE_REVERSED"), i + 1, 7, QStringLiteral("price"), QStringLiteral("minimum price exceeds maximum"));
        if (!wearOk) appendDiagnostic(diagnostics, QStringLiteral("error"), wearGroup ? QStringLiteral("DECIMAL_GROUPING_INVALID") : QStringLiteral("DECIMAL_INVALID"), i + 1, 9, QStringLiteral("wear"), QStringLiteral("invalid legacy wear"));
        QJsonObject priceRange; if (minOk && maxOk) priceRange = {{QStringLiteral("min"), moneyJson(minPrice)}, {QStringLiteral("max"), moneyJson(maxPrice)}};
        const bool wearRangeValid = !wearOk || c[8].isEmpty() || relink::business::compareDecimal(wear, DecimalValue{QStringLiteral("100"), 0}) != relink::business::Ordering::Greater;
        if (!wearRangeValid)
            appendDiagnostic(diagnostics, QStringLiteral("error"), QStringLiteral("V1_PRECISION_INVALID"), i + 1, 9, QStringLiteral("wear"), QStringLiteral("wear exceeds 100"));
        QJsonValue maxWear = (wearOk && !c[8].isEmpty()) ? decimalJson(wear) : nullValue();
        const QString sort = mapSort(c[10]); if (sort == QStringLiteral("unknown")) appendReview(diagnostics, reviewCodes, QStringLiteral("TAXONOMY_UNREVIEWED"), QStringLiteral("requested_sort"), i + 1, QStringLiteral("unknown sort token"));
        QJsonObject unknown{{QStringLiteral("/columns/12"), c[11]}, {QStringLiteral("/columns/13"), c[12]}};
        row.insert(QStringLiteral("candidate"), taskCandidate(QStringLiteral("legacy-row-%1").arg(i + 1), c[0], {}, hash, i + 1, reviewCodes,
                                                               makeFilters(season, ownership, grade, condition, publicity, rarity), priceRange, maxWear,
                                                               sort, nullValue(), c, unknown, c[0]));
        if (!minOk || !maxOk || !wearOk || !wearRangeValid || (minOk && maxOk && relink::business::compareDecimal(minPrice, maxPrice) == relink::business::Ordering::Greater)) row.insert(QStringLiteral("status"), QStringLiteral("invalid"));
        row.insert(QStringLiteral("diagnostics"), diagnostics); rows.append(row);
    }
    // Duplicate raw rows are a review signal, never an implicit de-duplication.
    for (int i = 0; i < rows.size(); ++i) for (int j = i + 1; j < rows.size(); ++j) {
        if (rows.at(i).toObject().value(QStringLiteral("raw_line")) == rows.at(j).toObject().value(QStringLiteral("raw_line"))) {
            auto row = rows.at(j).toObject(); auto d = row.value(QStringLiteral("diagnostics")).toArray();
            appendDiagnostic(d, QStringLiteral("review"), QStringLiteral("POSSIBLE_DUPLICATE_ROW"), row.value(QStringLiteral("line")).toInt(), -1, {}, QStringLiteral("row has identical raw content"));
            row.insert(QStringLiteral("diagnostics"), d); rows.replace(j, row);
        }
    }
    return basePreview(QStringLiteral("bbzps_13"), hash, bytes.size(), encoding, rows, rootDiagnostics, {});
}

} // namespace relink::config
