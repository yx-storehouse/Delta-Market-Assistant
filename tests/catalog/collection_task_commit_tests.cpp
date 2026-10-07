#include "application/collection_task_commit.h"
#include "application/catalog_configuration.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QTemporaryDir>
#include <iostream>

using namespace relink::application;
namespace {
QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray snapshot(const AppState& state) {
    return QJsonDocument(encodeV1Config(state.skins, state.tasks, state.run)).toJson();
}
QByteArray prefix(int kind, quint64 n) {
    int exponent = 0;
    while (exponent < 3 && n >= (quint64(1) << (8 * (1 << exponent)))) ++exponent;
    QByteArray out(1, char(kind | (exponent << 6)));
    for (int i = 0; i < (1 << exponent); ++i) out.append(char((n >> (8 * i)) & 255));
    return out;
}
QByteArray encode(const QJsonValue& value) {
    if (value.isBool()) return QByteArray(1, char(value.toBool()));
    if (value.isDouble()) return prefix(4, quint64(value.toInteger()));
    if (value.isString()) { const auto b = value.toString().toUtf8(); return prefix(7, b.size()) + b; }
    if (value.isArray()) {
        const auto a = value.toArray(); auto out = prefix(3, a.size());
        for (const auto& item : a) out += encode(item);
        return out;
    }
    const auto object = value.toObject(); auto out = prefix(2, object.size());
    for (auto it = object.begin(); it != object.end(); ++it) out += encode(it.key()) + encode(it.value());
    return out;
}
QByteArray fixture(int maximum = 600) {
    const QJsonObject fields{{QStringLiteral("启用"), true}, {QStringLiteral("成色设置栏"), 0},
        {QStringLiteral("最低价格设置栏"), 10}, {QStringLiteral("最高价格设置栏"), maximum},
        {QStringLiteral("枪名设置栏"), 10602}, {QStringLiteral("磨损度设置栏"), 5},
        {QStringLiteral("限量设置栏"), 0}};
    QJsonArray pairs;
    for (auto it = fields.begin(); it != fields.end(); ++it)
        pairs.append(QJsonArray{it.key() + QStringLiteral("_0"), it.value()});
    return encode(QJsonObject{{QStringLiteral("自动收藏-任务配置"), pairs}});
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int assertions = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++assertions; failures += !ok;
        std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
    };
    const auto dictionary = QJsonDocument::fromJson(read(argc > 1 ? QString::fromLocal8Bit(argv[1])
        : QStringLiteral("docs/business_rebuild/implementation/domain/relink_0925_catalog.json"))).object();
    relink::catalog::Catalog catalog;
    QString error;
    check(relink::catalog::parseCatalog(read(argc > 2 ? QString::fromLocal8Bit(argv[2])
        : QStringLiteral("src/assets/catalog/skins.json")), &catalog, &error), "catalog_loaded");
    QTemporaryDir temporary;
    check(temporary.isValid(), "isolated_temporary_directory");
    if (!temporary.isValid()) return 1;
    AppState state;
    check(applyCatalogConfiguration(state, catalog, nullptr, &error), "real_state_projected");
    state.tasks.append({"existing", QStringLiteral("原有收藏任务"), catalogSkinId("10603"),
        19, 432, 4, 3, false, QStringLiteral("用户暂停"), QStringLiteral("成色B")});
    state.skins[0].followed = true;
    state.run.purchaseDelayMs = 1999;
    state.configPath = temporary.filePath(QStringLiteral("config.json"));
    const auto original = snapshot(state) + QByteArray("\n \t\n");
    check(write(state.configPath, original), "original_config_with_exact_whitespace_written");
    const QString existingBackup = state.configPath + QStringLiteral(".before-task-import.bak");
    check(write(existingBackup, "older backup sentinel"), "existing_backup_sentinel_created");
    int notificationsCount = 0;
    QObject::connect(&state, &AppState::changed, [&] { ++notificationsCount; });
    CollectionTaskImportPreview report;
    const QByteArray source = fixture();
    check(commitCollectionTaskImport(source, dictionary, catalog, state, &report, &error), "commit_succeeds");
    check(report.addedCount == 1 && report.duplicateCount == 0 && state.tasks.size() == 2
          && notificationsCount == 1 && state.logs.size() == 1, "publishes_once_after_success");
    check(read(existingBackup) == "older backup sentinel"
          && read(state.configPath + QStringLiteral(".before-task-import-1.bak")) == original,
          "backup_is_exact_original_and_does_not_overwrite_existing_backup");
    check(read(state.configPath) == snapshot(state), "disk_matches_committed_configuration");
    check(state.tasks[0].status == QStringLiteral("用户暂停") && state.tasks[0].minPrice == 19
          && state.tasks[0].maxPrice == 432 && state.skins[0].followed
          && state.run.purchaseDelayMs == 1999 && state.tasks[1].quantity == 0,
          "existing_values_and_unlimited_source_survive_commit");
    AppState reopened;
    check(reopened.loadFrom(state.configPath, &error) && snapshot(reopened) == snapshot(state),
          "committed_file_reopens_with_all_import_metadata");
    check(!QFile::exists(state.configPath + QStringLiteral(".import.lock")), "lock_released_after_success");

    // A duplicate is a true no-op, not a request to save unrelated edits.
    const auto noOpDisk = read(state.configPath) + QByteArray("\n \n");
    check(write(state.configPath, noOpDisk), "duplicate_test_disk_sentinel_written");
    state.tasks[1].name = QStringLiteral("导入后的本地编辑");
    state.tasks[1].maxPrice = 555;
    const auto noOpState = snapshot(state);
    const int noOpLogs = state.logs.size();
    const QStringList noOpFiles = QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden);
    check(commitCollectionTaskImport(source, dictionary, catalog, state, &report, &error)
          && report.addedCount == 0 && report.duplicateCount == 1, "duplicate_commit_reports_noop");
    check(read(state.configPath) == noOpDisk && snapshot(state) == noOpState
          && notificationsCount == 1 && state.logs.size() == noOpLogs
          && QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden) == noOpFiles,
          "duplicate_does_not_write_backup_emit_or_save_unrelated_edits");

    const auto reject = [&](const QByteArray& input, const char* label) {
        const auto memory = snapshot(state);
        const auto disk = read(state.configPath);
        const auto path = state.configPath;
        const int logs = state.logs.size(), notifications = notificationsCount;
        CollectionTaskImportPreview untouched; untouched.rowCount = 321;
        check(!commitCollectionTaskImport(input, dictionary, catalog, state, &untouched, &error)
              && !error.isEmpty() && snapshot(state) == memory && read(state.configPath) == disk
              && state.configPath == path && logs == state.logs.size() && notificationsCount == notifications
              && untouched.rowCount == 321, label);
    };
    reject(source.left(source.size() - 1), "invalid_input_changes_neither_disk_nor_memory");
    {
        QLockFile other(state.configPath + QStringLiteral(".import.lock"));
        other.setStaleLockTime(0);
        check(other.tryLock(0), "other_import_holds_lock");
        const auto files = QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden);
        reject(fixture(601), "lock_conflict_leaves_disk_memory_logs_and_report_unchanged");
        check(QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden) == files,
              "lock_conflict_creates_no_backup");
    }
    const QString originalPath = state.configPath;
    state.configPath.clear();
    reject(fixture(601), "empty_destination_does_not_publish");
    state.configPath = temporary.filePath(QStringLiteral("directory-destination"));
    check(QDir().mkpath(state.configPath)
          && write(QDir(state.configPath).filePath(QStringLiteral("sentinel")), "directory content"),
          "create_unwritable_as_file_destination");
    reject(fixture(601), "atomic_save_failure_never_publishes_memory");
    check(read(QDir(state.configPath).filePath(QStringLiteral("sentinel"))) == "directory content"
          && !QFile::exists(state.configPath + QStringLiteral(".import.lock")),
          "failed_save_preserves_existing_directory_and_releases_lock");
    const QString blockingParent = temporary.filePath(QStringLiteral("parent-is-a-file"));
    check(write(blockingParent, "parent sentinel"), "parent_file_blocks_directory_creation");
    state.configPath = blockingParent + QStringLiteral("/config.json");
    reject(fixture(601), "parent_creation_failure_does_not_publish");
    check(read(blockingParent) == "parent sentinel", "parent_file_preserved");
    state.configPath = originalPath;
    const auto beforeSecond = read(state.configPath);
    check(commitCollectionTaskImport(fixture(601), dictionary, catalog, state, &report, &error)
          && report.addedCount == 1 && state.tasks.size() == 3 && state.tasks[1].maxPrice == 555,
          "subsequent_distinct_source_appends_and_keeps_edited_import");
    check(read(state.configPath + QStringLiteral(".before-task-import-2.bak")) == beforeSecond
          && read(state.configPath) == snapshot(state), "second_backup_captures_exact_previous_disk");

    AppState fresh;
    check(applyCatalogConfiguration(fresh, catalog, nullptr, &error), "fresh_catalog_projected");
    fresh.configPath = temporary.filePath(QStringLiteral("new-parent/new-config.json"));
    check(commitCollectionTaskImport(source, dictionary, catalog, fresh, &report, &error)
          && read(fresh.configPath) == snapshot(fresh) && fresh.tasks.size() == 1,
          "new_destination_created_and_saved");

    if (argc > 3) {
        const QString sourcePath = QString::fromLocal8Bit(argv[3]);
        const auto realSource = read(sourcePath);
        const auto sourceHash = QCryptographicHash::hash(realSource, QCryptographicHash::Sha256);
        AppState actual;
        check(applyCatalogConfiguration(actual, catalog, nullptr, &error), "actual_catalog_projected");
        actual.configPath = temporary.filePath(QStringLiteral("actual-config.json"));
        check(commitCollectionTaskImport(realSource, dictionary, catalog, actual, &report, &error)
              && report.rowCount == 50 && report.enabledCount == 21
              && report.distinctProducts == 9 && report.totalDistinctProducts == 13
              && actual.tasks.size() == 50, "actual_50_rows_21_enabled_9_active_13_total_committed");
        AppState loaded;
        check(loaded.loadFrom(actual.configPath, &error)
              && commitCollectionTaskImport(realSource, dictionary, catalog, loaded, &report, &error)
              && report.duplicateCount == 50 && report.addedCount == 0
              && snapshot(loaded) == read(actual.configPath), "actual_reopened_commit_deduplicates_without_write");
        check(QCryptographicHash::hash(read(sourcePath), QCryptographicHash::Sha256) == sourceHash,
              "actual_source_sha256_unchanged");
    }
    std::cout << "COLLECTION_TASK_COMMIT_TESTS=" << (failures ? "FAIL" : "PASS")
              << "; assertions=" << assertions << "; failures=" << failures
              << "; game_input_sent=false\n";
    return failures ? 1 : 0;
}
