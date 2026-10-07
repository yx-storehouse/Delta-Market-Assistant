#include "config/profile_store.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>

#include <limits>

using namespace relink::config;

namespace {
QJsonObject makePreview() {
    return preview13Columns(QByteArrayLiteral("AUG Demo|any|unowned|orange|S|none|230|600|0.399|rare|default|7|9\n"));
}
ReviewChoices makeChoices() {
    ReviewChoices value;
    value.profileName = QStringLiteral("PR08 Demo Profile");
    value.confirmProducts = true;
    value.confirmPriceRange = true;
    value.confirmTaxonomy = true;
    value.preserveUnknownColumns = true;
    value.keepQuantityUnbound = true;
    return value;
}
bool check(bool value, const char* message) {
    if (value) return true;
    QTextStream(stderr) << "profile_store_tests: FAIL: " << message << Qt::endl;
    return false;
}
QString errorCode(const ProfileStoreResult& result) {
    if (const auto* error = std::get_if<ProfileStoreError>(&result)) return error->code;
    return {};
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    bool ok = true;
    const auto preview = makePreview();
    const auto choices = makeChoices();
    const auto staged = ProfileStore::materializePreview(preview, choices);
    ok &= check(std::holds_alternative<ProfileCommitResult>(staged), "preview materializes");
    if (!ok) return 1;
    const auto first = std::get<ProfileCommitResult>(staged);
    ok &= check(first.document.value(QStringLiteral("schema_version")).toInt() == 2, "v2 schema");
    ok &= check(first.document.value(QStringLiteral("kind")).toString() == QStringLiteral("ConfigV2"), "v2 kind");
    ok &= check(first.profileRevision == 1 && !first.committed, "first revision is staged");
    ok &= check(first.document.value(QStringLiteral("source")).toObject().value(QStringLiteral("sha256"))
                    == preview.value(QStringLiteral("source_sha256")), "source hash preserved");
    ok &= check(first.document.value(QStringLiteral("extensions")).toObject().value(QStringLiteral("x-activation_required")).toBool(), "activation remains explicit");
    const auto rules = first.document.value(QStringLiteral("rules")).toArray();
    if (!rules.isEmpty()) {
        ok &= check(!rules.first().toObject().value(QStringLiteral("enabled")).toBool(), "reviewed rule is not auto-enabled");
        ok &= check(!rules.first().toObject().value(QStringLiteral("review_required")).toBool(), "reviewed rule clears review_required");
    }
    QJsonObject actualPreview = preview;
    const QJsonObject actualCatalogEntry{
        {QStringLiteral("product_id"), QStringLiteral("catalog-10602")},
        {QStringLiteral("extensions"), QJsonObject{{QStringLiteral("x-menuColor"), QStringLiteral("purple")}}}};
    QJsonObject actualExtensions{
        {QStringLiteral("x-target_mode"), QStringLiteral("configuration")},
        {QStringLiteral("x-observation_source"), QStringLiteral("configuration_metadata")},
        {QStringLiteral("x-catalog"), QJsonArray{actualCatalogEntry}}};
    actualPreview.insert(QStringLiteral("extensions"), actualExtensions);
    const auto actualStaged = ProfileStore::materializePreview(actualPreview, choices);
    ok &= check(std::holds_alternative<ProfileCommitResult>(actualStaged), "non-demo configuration stages");
    if (const auto* actual = std::get_if<ProfileCommitResult>(&actualStaged)) {
        const auto ext = actual->document.value(QStringLiteral("extensions")).toObject();
        ok &= check(ext.value(QStringLiteral("x-target_mode")) == actualExtensions.value(QStringLiteral("x-target_mode"))
            && ext.value(QStringLiteral("x-observation_source")) == actualExtensions.value(QStringLiteral("x-observation_source"))
            && ext.value(QStringLiteral("x-catalog")) == actualExtensions.value(QStringLiteral("x-catalog")),
            "profile export retains real catalogue and configuration source without synthetic relabeling");
    }
    ReviewChoices incomplete = choices;
    incomplete.confirmTaxonomy = false;
    const auto incompleteResult = ProfileStore::materializePreview(preview, incomplete);
    ok &= check(errorCode(incompleteResult) == QStringLiteral("REVIEW_INCOMPLETE"), "incomplete review rejected");
    ReviewChoices blankName = choices;
    blankName.profileName = QStringLiteral("  ");
    ok &= check(errorCode(ProfileStore::materializePreview(preview, blankName)) == QStringLiteral("REVIEW_INCOMPLETE"), "blank profile name rejected");
    ReviewChoices blankQuantum = choices;
    blankQuantum.quantum = QStringLiteral("  ");
    ok &= check(errorCode(ProfileStore::materializePreview(preview, blankQuantum)) == QStringLiteral("REVIEW_INCOMPLETE"), "blank quantum rejected");

    QJsonObject wrongKind = preview;
    wrongKind.insert(QStringLiteral("kind"), QStringLiteral("ConfigV2"));
    ok &= check(errorCode(ProfileStore::materializePreview(wrongKind, choices)) == QStringLiteral("PREVIEW_INVALID"), "wrong preview kind rejected");
    QJsonObject withError = preview;
    withError.insert(QStringLiteral("diagnostics"), QJsonArray{QJsonObject{{QStringLiteral("severity"), QStringLiteral("error")}}});
    ok &= check(errorCode(ProfileStore::materializePreview(withError, choices)) == QStringLiteral("PREVIEW_HAS_ERRORS"), "blocking diagnostics rejected");
    QJsonObject badHash = preview;
    badHash.insert(QStringLiteral("source_sha256"), QString(64, QLatin1Char('g')));
    ok &= check(errorCode(ProfileStore::materializePreview(badHash, choices)) == QStringLiteral("SOURCE_HASH_INVALID"), "non-hex hash rejected");
    QJsonObject upperHash = preview;
    upperHash.insert(QStringLiteral("source_sha256"), preview.value(QStringLiteral("source_sha256")).toString().toUpper());
    ok &= check(errorCode(ProfileStore::materializePreview(upperHash, choices)) == QStringLiteral("SOURCE_HASH_INVALID"), "uppercase hash rejected");
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("profile.json"));
    QString error;
    ProfileCommitResult committed;
    ok &= check(ProfileStore::commitPreview(preview, choices, path, &error, &committed), "atomic commit succeeds");
    ok &= check(error.isEmpty() && committed.committed && QFile::exists(path), "commit result and file");
    QJsonObject loaded;
    ok &= check(ProfileStore::load(path, loaded, &error), "committed v2 loads");
    ok &= check(loaded.value(QStringLiteral("profile")).toObject().value(QStringLiteral("revision")).toInt() == 1, "loaded revision is one");
    const auto second = ProfileStore::materializePreview(preview, choices, loaded);
    ok &= check(std::holds_alternative<ProfileCommitResult>(second) && std::get<ProfileCommitResult>(second).profileRevision == 2, "same profile increments revision");
    ProfileCommitResult recommitted;
    ok &= check(ProfileStore::commitPreview(preview, choices, path, &error, &recommitted), "second atomic commit succeeds");
    ok &= check(recommitted.committed && recommitted.profileRevision == 2, "second commit publishes revision two");
    QJsonObject reloaded;
    ok &= check(ProfileStore::load(path, reloaded, &error)
                    && reloaded.value(QStringLiteral("profile")).toObject().value(QStringLiteral("revision")).toInt() == 2,
                "second commit is complete ConfigV2 JSON");
    QJsonObject overflowExisting{{QStringLiteral("profile"), QJsonObject{
        {QStringLiteral("id"), first.profileId},
        {QStringLiteral("revision"), std::numeric_limits<int>::max()}}}};
    ok &= check(errorCode(ProfileStore::materializePreview(preview, choices, overflowExisting)) == QStringLiteral("REVISION_INVALID"), "revision overflow rejected");
    QJsonObject differentProfileExisting{{QStringLiteral("profile"), QJsonObject{
        {QStringLiteral("id"), QStringLiteral("profile-other")},
        {QStringLiteral("revision"), 99}}}};
    const auto reset = ProfileStore::materializePreview(preview, choices, differentProfileExisting);
    ok &= check(std::holds_alternative<ProfileCommitResult>(reset) && std::get<ProfileCommitResult>(reset).profileRevision == 1, "different profile resets revision");
    const QString invalidPath = directory.filePath(QStringLiteral("invalid.json"));
    QFile invalid(invalidPath);
    invalid.open(QIODevice::WriteOnly);
    invalid.write(QByteArrayLiteral("{\"not\":\"config\"}"));
    invalid.close();
    QFile beforeFile(invalidPath);
    beforeFile.open(QIODevice::ReadOnly);
    const QByteArray before = beforeFile.readAll();
    beforeFile.close();
    ok &= check(!ProfileStore::commitPreview(preview, choices, invalidPath, &error), "invalid existing target rejected");
    QFile afterFile(invalidPath);
    afterFile.open(QIODevice::ReadOnly);
    const QByteArray after = afterFile.readAll();
    afterFile.close();
    ok &= check(before == after, "failed commit leaves target unchanged");
    QJsonObject untouched;
    untouched.insert(QStringLiteral("sentinel"), QStringLiteral("keep"));
    ok &= check(!ProfileStore::load(invalidPath, untouched, &error) && untouched.value(QStringLiteral("sentinel")).toString() == QStringLiteral("keep"), "invalid load does not mutate output");
    QJsonObject malformed = preview;
    malformed.insert(QStringLiteral("committable"), true);
    ok &= check(errorCode(ProfileStore::materializePreview(malformed, choices)) == QStringLiteral("PREVIEW_NOT_READ_ONLY"), "committable preview rejected");
    const QString noParentPath = directory.filePath(QStringLiteral("missing-parent/profile.json"));
    ok &= check(!ProfileStore::commitPreview(preview, choices, noParentPath, &error), "missing parent commit rejected");
    ok &= check(!QFile::exists(noParentPath), "failed missing-parent commit leaves no target");
    if (!ok) return 1;
    QTextStream(stdout) << "profile_store_tests: PASS" << Qt::endl;
    return 0;
}
