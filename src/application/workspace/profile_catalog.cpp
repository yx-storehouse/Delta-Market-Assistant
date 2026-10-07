#include "profile_catalog.h"

#include "config/profile_store.h"
#include "business/value_types.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

namespace relink::workspace {
namespace {
QString s(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString(); }
}

QString ProfileCatalog::builtinProfileId() { return QStringLiteral("builtin-synthetic-fixture"); }

QJsonObject ProfileCatalog::builtinDocument()
{
    return {{QStringLiteral("schema_version"), 2}, {QStringLiteral("kind"), QStringLiteral("ConfigV2")},
        {QStringLiteral("profile"), QJsonObject{{QStringLiteral("id"), builtinProfileId()},
            {QStringLiteral("revision"), 1}, {QStringLiteral("name"), QStringLiteral("内置合成回放 · 八步验收")}}},
        {QStringLiteral("source"), QJsonObject{{QStringLiteral("format"), QStringLiteral("synthetic_fixture")}}},
        {QStringLiteral("extensions"), QJsonObject{{QStringLiteral("x-live_market_data"), false},
            {QStringLiteral("x-built-in-fixture"), true}}}};
}

bool ProfileCatalog::validateDocument(const QJsonObject& doc, QString* error)
{
    auto fail = [&](const QString& value) { if (error) *error = value; return false; };
    const auto p = doc.value(QStringLiteral("profile")).toObject();
    static const QRegularExpression idPattern(QStringLiteral("^profile-[0-9a-f]{24}$"));
    if (!idPattern.match(s(p, "id")).hasMatch() || p.value(QStringLiteral("revision")).toInt() < 1 || s(p, "name").trimmed().isEmpty())
        return fail(QStringLiteral("profile identity is invalid"));
    const auto source = doc.value(QStringLiteral("source")).toObject();
    static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{64}$"));
    if (!hashPattern.match(s(source,"sha256")).hasMatch()
        || !QStringList{QStringLiteral("schema_v1"),QStringLiteral("bbzps_13")}.contains(s(source,"format"))
        || !QStringList{QStringLiteral("UTF-8"),QStringLiteral("UTF-8-BOM"),QStringLiteral("UTF-16LE-BOM"),QStringLiteral("GB18030-explicit")}.contains(s(source,"encoding")))
        return fail(QStringLiteral("profile source metadata is invalid"));
    const QByteArray identity = (s(p,"name").trimmed() + QChar(0x1f) + s(source,"sha256")).toUtf8();
    if (s(p,"id") != QStringLiteral("profile-") + QString::fromLatin1(QCryptographicHash::hash(identity,QCryptographicHash::Sha256).toHex().left(24)))
        return fail(QStringLiteral("profile identity hash does not match source"));
    const auto extensions = doc.value(QStringLiteral("extensions")).toObject();
    if (!extensions.value(QStringLiteral("x-activation_required")).toBool()
        || extensions.value(QStringLiteral("x-live_market_data")).toBool(true)
        || !doc.value(QStringLiteral("rules")).isArray()) return fail(QStringLiteral("profile activation or rules metadata is invalid"));
    const auto decisions = doc.value(QStringLiteral("review_decisions")).toObject();
    const QString quantumText=s(decisions,"unit_quantum").trimmed();
    static const QRegularExpression quantumPattern(QStringLiteral("^(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?$"));
    if (!quantumPattern.match(quantumText).hasMatch()) return fail(QStringLiteral("review quantum is invalid"));
    const auto quantum=business::parseDecimal(QStringView(quantumText));
    const auto* quantumValue=std::get_if<business::DecimalValue>(&quantum);
    if (!quantumValue || !business::decimalIsPositive(*quantumValue)) return fail(QStringLiteral("review quantum must be positive"));
    for (const auto* key : {"products", "price_range", "taxonomy", "preserve_unknown_columns", "keep_quantity_unbound"})
        if (!decisions.value(QLatin1String(key)).toBool()) return fail(QStringLiteral("review decision is incomplete"));
    for (const auto& value : doc.value(QStringLiteral("rules")).toArray()) {
        const auto r = value.toObject();
        if (r.value(QStringLiteral("enabled")).toBool(true) || r.value(QStringLiteral("review_required")).toBool(true)
            || !r.value(QStringLiteral("activation_required")).toBool()) return fail(QStringLiteral("saved rules must remain disabled and activation-gated"));
    }
    return true;
}

ProfileCatalogResult ProfileCatalog::scan(const QString& rootPath, const QString& selectedProfileId) const
{
    ProfileCatalogResult result;
    const auto builtin = builtinDocument();
    const auto b = builtin.value(QStringLiteral("profile")).toObject();
    result.rows = {{s(b,"id"),s(b,"name"),QStringLiteral("synthetic_fixture"),{},QStringLiteral("SyntheticOnly"),{},1,true,
                    s(b,"id") == selectedProfileId,false}};
    result.documents.insert(builtinProfileId(), builtin);
    const QDir dir(QDir(rootPath).filePath(QStringLiteral("profiles")));
    for (const auto& info : dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        QJsonObject doc; QString message;
        if (!config::ProfileStore::load(info.absoluteFilePath(), doc, &message)) {
            result.error = QStringLiteral("PROFILE_LOAD_FAILED: ") + message; return result;
        }
        QString validationError;
        if (!validateDocument(doc, &validationError)) {
            result.error = QStringLiteral("PROFILE_INVALID: ") + info.fileName();
            if (!validationError.isEmpty()) result.error += QStringLiteral(" (") + validationError + QLatin1Char(')');
            return result;
        }
        const auto profile = doc.value(QStringLiteral("profile")).toObject();
        const auto source = doc.value(QStringLiteral("source")).toObject();
        const QString id = s(profile,"id");
        if (info.fileName() != id + QStringLiteral(".json") || result.documents.contains(id)) {
            result.error = QStringLiteral("PROFILE_ID_CONFLICT"); return result;
        }
        result.documents.insert(id,doc);
        result.rows.push_back({id,s(profile,"name"),s(source,"format"),s(source,"sha256"),
            QStringLiteral("ReviewedDisabled"),info.absoluteFilePath(),profile.value(QStringLiteral("revision")).toInt(),false,
            id == selectedProfileId,true});
    }
    result.ok = true;
    return result;
}

bool ProfileCatalog::materializeAndValidate(const QJsonObject& preview,
                                            const config::ReviewChoices& choices,
                                            QJsonObject* document,
                                            QString* error) const
{
    const QString quantumText = choices.quantum.trimmed();
    static const QRegularExpression quantumPattern(QStringLiteral("^(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?$"));
    if (!quantumPattern.match(quantumText).hasMatch()) { if (error) *error = QStringLiteral("QUANTUM_INVALID: positive plain decimal required"); return false; }
    const auto quantum=business::parseDecimal(QStringView(quantumText));
    const auto* quantumValue=std::get_if<business::DecimalValue>(&quantum);
    if (!quantumValue || !business::decimalIsPositive(*quantumValue)) { if (error) *error = QStringLiteral("QUANTUM_INVALID: positive plain decimal required"); return false; }
    const auto result=config::ProfileStore::materializePreview(preview,choices);
    if (const auto* failure=std::get_if<config::ProfileStoreError>(&result)) { if (error) *error = failure->code+QStringLiteral(": ")+failure->message; return false; }
    const auto candidate = std::get<config::ProfileCommitResult>(result).document;
    QString validationError;
    if (!validateDocument(candidate, &validationError)) { if (error) *error = QStringLiteral("PROFILE_INVALID: review source or document is invalid"); return false; }
    if (document) *document = candidate;
    return true;
}

} // namespace relink::workspace
