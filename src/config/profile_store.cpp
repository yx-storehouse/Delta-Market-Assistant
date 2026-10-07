#include "profile_store.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <limits>

namespace relink::config {
namespace {

ProfileStoreError failure(const QString& code, const QString& message)
{
    return {code, message};
}

bool confirmed(const ReviewChoices& choices)
{
    return !choices.profileName.trimmed().isEmpty()
        && !choices.quantum.trimmed().isEmpty()
        && choices.confirmProducts
        && choices.confirmPriceRange
        && choices.confirmTaxonomy
        && choices.preserveUnknownColumns
        && choices.keepQuantityUnbound;
}

QString profileId(const QString& name, const QString& sourceHash)
{
    const QByteArray seed = (name.trimmed() + QChar(0x1f) + sourceHash).toUtf8();
    return QStringLiteral("profile-")
        + QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Sha256).toHex().left(24));
}

bool hasError(const QJsonObject& preview)
{
    if (previewHasErrors(preview)) return true;
    for (const auto& value : preview.value(QStringLiteral("rows")).toArray()) {
        const auto row = value.toObject();
        if (row.value(QStringLiteral("status")).toString() == QStringLiteral("invalid")) return true;
    }
    return false;
}

bool isSha256Hex(const QString& value)
{
    if (value.size() != 64) return false;
    for (const QChar ch : value) {
        const bool digit = ch >= QLatin1Char('0') && ch <= QLatin1Char('9');
        const bool lowerHex = ch >= QLatin1Char('a') && ch <= QLatin1Char('f');
        if (!digit && !lowerHex) return false;
    }
    return true;
}

} // namespace

ProfileStoreResult ProfileStore::materializePreview(const QJsonObject& preview,
                                                    const ReviewChoices& choices,
                                                    const QJsonObject& existing)
{
    if (preview.value(QStringLiteral("kind")).toString() != QStringLiteral("LegacyImportPreview"))
        return failure(QStringLiteral("PREVIEW_INVALID"), QStringLiteral("preview kind is invalid"));
    if (preview.value(QStringLiteral("committable")).toBool(true))
        return failure(QStringLiteral("PREVIEW_NOT_READ_ONLY"), QStringLiteral("preview must be committable=false"));
    if (hasError(preview))
        return failure(QStringLiteral("PREVIEW_HAS_ERRORS"), QStringLiteral("preview contains blocking diagnostics"));
    if (!confirmed(choices))
        return failure(QStringLiteral("REVIEW_INCOMPLETE"), QStringLiteral("all review confirmations are required"));

    const QString sourceHash = preview.value(QStringLiteral("source_sha256")).toString();
    if (!isSha256Hex(sourceHash))
        return failure(QStringLiteral("SOURCE_HASH_INVALID"), QStringLiteral("source SHA-256 must be 64 lowercase hexadecimal characters"));

    const QString id = profileId(choices.profileName, sourceHash);
    const auto oldProfile = existing.value(QStringLiteral("profile")).toObject();
    int revision = 1;
    if (oldProfile.value(QStringLiteral("id")).toString() == id) {
        const int oldRevision = oldProfile.value(QStringLiteral("revision")).toInt(0);
        if (oldRevision < 1 || oldRevision == std::numeric_limits<int>::max())
            return failure(QStringLiteral("REVISION_INVALID"), QStringLiteral("existing profile revision cannot be incremented"));
        revision = oldRevision + 1;
    }

    QJsonArray rules;
    for (const auto& value : preview.value(QStringLiteral("rows")).toArray()) {
        const auto row = value.toObject();
        auto candidate = row.value(QStringLiteral("candidate")).toObject();
        if (candidate.isEmpty()) continue;
        candidate.insert(QStringLiteral("enabled"), false);
        candidate.insert(QStringLiteral("review_required"), false);
        candidate.insert(QStringLiteral("activation_required"), true);
        rules.append(candidate);
    }

    const auto extensions = preview.value(QStringLiteral("extensions")).toObject();
    const QJsonObject document{
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("kind"), QStringLiteral("ConfigV2")},
        {QStringLiteral("profile"), QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), choices.profileName.trimmed()},
            {QStringLiteral("revision"), revision}}},
        {QStringLiteral("source"), QJsonObject{
            {QStringLiteral("format"), preview.value(QStringLiteral("source_format"))},
            {QStringLiteral("encoding"), preview.value(QStringLiteral("encoding"))},
            {QStringLiteral("sha256"), sourceHash}}},
        {QStringLiteral("run_settings"), extensions.value(QStringLiteral("x-ui_run_settings"))},
        {QStringLiteral("rules"), rules},
        {QStringLiteral("review_decisions"), QJsonObject{
            {QStringLiteral("products"), choices.confirmProducts},
            {QStringLiteral("price_range"), choices.confirmPriceRange},
            {QStringLiteral("taxonomy"), choices.confirmTaxonomy},
            {QStringLiteral("preserve_unknown_columns"), choices.preserveUnknownColumns},
            {QStringLiteral("keep_quantity_unbound"), choices.keepQuantityUnbound},
            {QStringLiteral("unit_quantum"), choices.quantum}}},
        {QStringLiteral("extensions"), QJsonObject{
            {QStringLiteral("x-target_mode"), extensions.value(QStringLiteral("x-target_mode")).toString(QStringLiteral("configuration"))},
            {QStringLiteral("x-observation_source"), extensions.value(QStringLiteral("x-observation_source")).toString(QStringLiteral("configuration_metadata"))},
            {QStringLiteral("x-catalog"), extensions.value(QStringLiteral("x-catalog")).toArray()},
            {QStringLiteral("x-id_map"), extensions.value(QStringLiteral("x-id_map")).toObject()},
            {QStringLiteral("x-legacy_unknown_fields"), extensions.value(QStringLiteral("x-legacy_unknown_fields")).toObject()},
            {QStringLiteral("x-live_market_data"), false},
            {QStringLiteral("x-import_preview_committable"), false},
            {QStringLiteral("x-activation_required"), true}}}
    };
    return ProfileCommitResult{document, id, revision, false};
}

bool ProfileStore::load(const QString& path, QJsonObject& document, QString* error)
{
    if (error) error->clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject() || doc.object().value(QStringLiteral("schema_version")).toInt() != 2
        || doc.object().value(QStringLiteral("kind")).toString() != QStringLiteral("ConfigV2")) {
        if (error) *error = QStringLiteral("invalid ConfigV2 document");
        return false;
    }
    document = doc.object();
    return true;
}

bool ProfileStore::commitPreview(const QJsonObject& preview,
                                 const ReviewChoices& choices,
                                 const QString& path,
                                 QString* error,
                                 ProfileCommitResult* committed)
{
    if (error) error->clear();
    QJsonObject existing;
    if (QFile::exists(path) && !load(path, existing, error)) return false;
    const auto materialized = materializePreview(preview, choices, existing);
    if (const auto* failureValue = std::get_if<ProfileStoreError>(&materialized)) {
        if (error) *error = failureValue->code + QStringLiteral(": ") + failureValue->message;
        return false;
    }
    auto result = std::get<ProfileCommitResult>(materialized);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    const QByteArray bytes = QJsonDocument(result.document).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    result.committed = true;
    if (committed) *committed = result;
    return true;
}

} // namespace relink::config
