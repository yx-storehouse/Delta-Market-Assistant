#include "application/collection_task_import.h"
#include "application/catalog_configuration.h"
#include "config/savedvalue_reader.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <iostream>

using namespace relink::application;
namespace {
QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QByteArray stateBytes(const AppState& state) {
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
QJsonArray row(int index, int product, int condition = 0, bool enabled = true,
               int minPrice = 10, int maxPrice = 600, int wear = 5, int quantity = 0) {
    const QJsonObject fields{{QStringLiteral("启用"), enabled}, {QStringLiteral("成色设置栏"), condition},
        {QStringLiteral("最低价格设置栏"), minPrice}, {QStringLiteral("最高价格设置栏"), maxPrice},
        {QStringLiteral("枪名设置栏"), product}, {QStringLiteral("磨损度设置栏"), wear},
        {QStringLiteral("限量设置栏"), quantity}};
    QJsonArray result;
    for (auto it = fields.begin(); it != fields.end(); ++it)
        result.append(QJsonArray{it.key() + QLatin1Char('_') + QString::number(index), it.value()});
    return result;
}
QByteArray input(const QVector<QJsonArray>& rows) {
    QJsonArray pairs;
    for (const auto& row : rows) for (const auto& pair : row) pairs.append(pair);
    return encode(QJsonObject{{QStringLiteral("自动收藏-任务配置"), pairs},
        {QStringLiteral("购买延迟"), QJsonArray{QJsonArray{QStringLiteral("购买延迟"), 838}}}});
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
    AppState state;
    check(applyCatalogConfiguration(state, catalog, nullptr, &error), "real_catalog_projected");
    state.skins[0].followed = true;
    state.tasks.append({"user-task", QStringLiteral("保留我的条件"), catalogSkinId("10602"),
        23, 321, 4, 7, false, QStringLiteral("用户暂停"), QStringLiteral("成色B")});
    state.run.purchaseDelayMs = 1473;
    const auto original = stateBytes(state);
    const auto bytes = input({row(10, 11102, 3, false, 17, 432, 9, 3),
                              row(2, 10602), row(3, 10603, 4, true, 20, 580, 4)});
    CollectionTaskImportPreview preview;
    int changed = 0;
    QObject::connect(&state, &AppState::changed, [&] { ++changed; });
    check(previewCollectionTaskImport(bytes, dictionary, catalog, state, &preview, &error), "preview_accepts_resolved_data");
    check(stateBytes(state) == original && changed == 0, "preview_and_cancel_leave_state_unchanged");
    check(preview.rowCount == 3 && preview.enabledCount == 2 && preview.distinctProducts == 2
          && preview.totalDistinctProducts == 3 && preview.addedCount == 3 && preview.duplicateCount == 0,
          "preview_counts_enabled_and_total_products_separately");
    check(preview.rows[0].task.importSource.row == 2 && preview.rows[1].task.importSource.row == 3
          && preview.rows[2].task.importSource.row == 10, "preserves_numeric_source_row_order");
    check(preview.rows[2].task.minPrice == 17 && preview.rows[2].task.maxPrice == 432
          && preview.rows[2].task.maxWear == 9 && preview.rows[2].task.quantity == 3
          && !preview.rows[2].task.enabled && preview.rows[2].task.condition == QStringLiteral("成色C"),
          "disabled_row_keeps_all_original_parameters");
    check(preview.rows[0].task.quantity == 0 && preview.rows[1].task.condition == QStringLiteral("仅磨损"),
          "zero_limit_and_wear_only_condition_preserved");
    check(preview.rows[0].menuColor == "purple" && preview.rows[0].variantLabel.isEmpty()
          && state.skins[0].rarity == QStringLiteral("待核对"), "menu_color_is_not_guessed_quality_or_condition");
    check(preview.rows[2].seasonLabel == QStringLiteral("疾风魅影"), "s11_source_spelling_not_renamed");
    check(preview.inactiveSettingsMetadata.contains(QStringLiteral("购买延迟"))
          && state.run.purchaseDelayMs == 1473, "purchase_settings_metadata_only");
    check(applyCollectionTaskImport(bytes, dictionary, catalog, state, &preview, &error), "apply_appends_atomically");
    check(changed == 1 && state.tasks.size() == 4 && state.tasks[0].status == QStringLiteral("用户暂停")
          && state.skins[0].followed && state.run.purchaseDelayMs == 1473,
          "existing_tasks_follows_runtime_status_and_settings_preserved");
    check(state.tasks[1].id == collectionImportTaskId(preview.sourceSha256, 2)
          && state.tasks[1].importSource.sourceSha256 == QString::fromLatin1(
              QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()), "stable_content_and_row_identity");
    state.tasks[1].maxPrice = 555;
    state.tasks[1].enabled = false;
    state.tasks[1].name = QStringLiteral("我编辑过的任务");
    const auto edited = stateBytes(state);
    check(applyCollectionTaskImport(bytes, dictionary, catalog, state, &preview, &error)
          && preview.addedCount == 0 && preview.duplicateCount == 3 && changed == 1
          && stateBytes(state) == edited, "repeat_import_deduplicates_and_preserves_user_edits");
    QTemporaryDir temporary;
    const auto savePath = temporary.filePath(QStringLiteral("config.json"));
    AppState reloaded;
    check(state.saveTo(savePath, &error) && reloaded.loadFrom(savePath, &error)
          && stateBytes(reloaded) == edited, "source_provenance_and_zero_limit_roundtrip");
    check(applyCollectionTaskImport(bytes, dictionary, catalog, reloaded, &preview, &error)
          && preview.addedCount == 0 && preview.duplicateCount == 3 && reloaded.tasks.size() == 4,
          "reopened_configuration_keeps_dedup_identity");
    const auto reject = [&](const QByteArray& data, const QJsonObject& dict,
                            const relink::catalog::Catalog& cat, const char* label) {
        CollectionTaskImportPreview sentinel;
        sentinel.sourceSha256 = "sentinel"; sentinel.rowCount = 777;
        const auto before = stateBytes(state);
        check(!previewCollectionTaskImport(data, dict, cat, state, &sentinel, &error)
              && !error.isEmpty() && sentinel.sourceSha256 == "sentinel" && sentinel.rowCount == 777
              && stateBytes(state) == before && changed == 1, label);
    };
    reject(input({row(0, 10602), row(1, 99999)}), dictionary, catalog, "unknown_product_rejects_entire_batch");
    reject(input({row(0, 10602), row(1, 99999, 0, false)}), dictionary, catalog,
           "disabled_unknown_product_rejects_entire_batch");
    {
        const auto before = stateBytes(state);
        CollectionTaskImportPreview report;
        report.rowCount = 123;
        check(!applyCollectionTaskImport(input({row(0, 10602), row(1, 99999)}), dictionary,
                                        catalog, state, &report, &error)
              && stateBytes(state) == before && changed == 1 && report.rowCount == 123,
              "apply_rejects_entire_batch_without_notification_or_report_mutation");
    }
    reject(input({row(0, 10602, 99)}), dictionary, catalog, "unknown_condition_rejects_batch");
    reject(input({row(0, 10602, 0, true, 10, 600, 101)}), dictionary, catalog, "wear_over_limit_not_clamped");
    reject(input({row(0, 10602, 0, true, 10, 600, 5, 10000)}), dictionary, catalog, "quantity_over_limit_not_clamped");
    reject(input({row(0, 10602, 0, true, 10, 1000000000)}), dictionary, catalog, "price_over_limit_not_clamped");
    reject(input({row(0, 10602, 0, true, 700, 600)}), dictionary, catalog, "inverted_price_range_rejected");
    reject(bytes.left(bytes.size() - 1), dictionary, catalog, "truncated_source_atomic_rejection");
    reject(input({}), dictionary, catalog, "empty_collection_rejected");
    auto changedDictionary = dictionary;
    auto products = changedDictionary.value(QStringLiteral("products")).toObject();
    auto s11 = products.value("11102").toObject(); s11["season_label"] = QStringLiteral("疾光魅影");
    products["11102"] = s11; changedDictionary["products"] = products;
    reject(bytes, changedDictionary, catalog, "distinct_s11_spelling_rejected_not_fuzzy_matched");
    changedDictionary = dictionary;
    products = changedDictionary.value("products").toObject();
    auto aug = products.value("10602").toObject(); aug["name"] = "P90";
    products["10602"] = aug; changedDictionary["products"] = products;
    reject(bytes, changedDictionary, catalog, "dictionary_name_and_display_conflict_rejected");
    changedDictionary = dictionary; changedDictionary.remove("collection_limit_zero_semantics");
    reject(bytes, changedDictionary, catalog, "unknown_zero_limit_semantics_rejected");
    auto missingCatalog = catalog;
    for (qsizetype i = 0; i < missingCatalog.skins.size(); ++i)
        if (missingCatalog.skins[i].productId == "10603") { missingCatalog.skins.removeAt(i); break; }
    reject(bytes, dictionary, missingCatalog, "dictionary_only_id_without_catalog_rejected");
    const auto oldOrigin = state.tasks[1].importSource;
    state.tasks[1].importSource.dictionarySha256 = QString(64, QLatin1Char('a'));
    reject(bytes, dictionary, catalog, "conflicting_existing_provenance_rejected");
    state.tasks[1].importSource = oldOrigin;
    auto oldTask = state.tasks[1];
    state.tasks[1].importSource = {};
    reject(bytes, dictionary, catalog, "stable_id_collision_not_overwritten");
    state.tasks[1] = oldTask;
    auto duplicate = oldTask; duplicate.id = "duplicate-origin";
    state.tasks.append(duplicate);
    check(!state.saveTo(savePath, &error), "duplicate_import_origin_rejected_by_config_codec");
    state.tasks.removeLast();
    const auto invalidOrigins = QVector<TaskImportSource>{
        {"savedValue", "bad-hash", oldOrigin.dictionarySha256, oldOrigin.productId, oldOrigin.conditionId, oldOrigin.row, oldOrigin.fields},
        {"savedValue", oldOrigin.sourceSha256, oldOrigin.dictionarySha256, "10603", oldOrigin.conditionId, oldOrigin.row, oldOrigin.fields},
        {"savedValue", oldOrigin.sourceSha256, oldOrigin.dictionarySha256, oldOrigin.productId, oldOrigin.conditionId, 500, oldOrigin.fields},
        {"savedValue", oldOrigin.sourceSha256, oldOrigin.dictionarySha256, oldOrigin.productId, oldOrigin.conditionId, oldOrigin.row, {}}};
    for (const auto& invalid : invalidOrigins) {
        state.tasks[1].importSource = invalid;
        check(!state.saveTo(savePath, &error), "malformed_provenance_rejected_before_disk_write");
    }
    state.tasks[1] = oldTask;
    const auto disabled = input({row(0, 10100, 0, false)});
    check(previewCollectionTaskImport(disabled, dictionary, catalog, state, &preview, &error)
          && preview.enabledCount == 0 && preview.addedCount == 1 && preview.rows[0].variantLabel == QStringLiteral("极品"),
          "all_disabled_template_imported_without_activation");

    if (argc > 3) {
        const QString sourcePath = QString::fromLocal8Bit(argv[3]);
        const auto actualBytes = read(sourcePath);
        AppState actual;
        check(applyCatalogConfiguration(actual, catalog, nullptr, &error), "actual_catalog_ready");
        check(previewCollectionTaskImport(actualBytes, dictionary, catalog, actual, &preview, &error), "actual_file_preview");
        check(preview.rowCount == 50 && preview.enabledCount == 21 && preview.distinctProducts == 9
              && preview.totalDistinctProducts == 13 && preview.addedCount == 50, "actual_50_rows_21_enabled_9_active_13_total_products");
        const auto decoded = relink::config::savedValueCollectionSnapshot(actualBytes, dictionary);
        const auto originalRows = decoded.value("rows").toArray();
        bool exact = preview.rows.size() == originalRows.size();
        for (int i = 0; exact && i < originalRows.size(); ++i) {
            const auto source = originalRows[i].toObject(); const auto& task = preview.rows[i].task;
            exact &= task.importSource.row == source.value("row_index").toInt()
                && task.importSource.fields == source.value("source_fields").toObject()
                && task.enabled == source.value("enabled").toBool()
                && task.condition == source.value("condition_label").toString()
                && task.minPrice == source.value("price_min").toString().toDouble()
                && task.maxPrice == source.value("price_max").toString().toDouble()
                && task.maxWear == source.value("max_wear").toString().toDouble()
                && task.quantity == source.value("limit_raw").toInt();
        }
        check(exact, "actual_every_row_preserves_all_source_parameters");
        check(applyCollectionTaskImport(actualBytes, dictionary, catalog, actual, &preview, &error)
              && actual.tasks.size() == 50 && actual.saveTo(savePath, &error), "actual_all_rows_saved");
        AppState actualReopened;
        check(actualReopened.loadFrom(savePath, &error)
              && applyCollectionTaskImport(actualBytes, dictionary, catalog, actualReopened, &preview, &error)
              && actualReopened.tasks.size() == 50 && preview.duplicateCount == 50 && preview.addedCount == 0,
              "actual_reopened_import_idempotent");
        check(read(sourcePath) == actualBytes, "actual_source_file_unchanged");
    }
    std::cout << "COLLECTION_TASK_IMPORT_TESTS=" << (failures ? "FAIL" : "PASS")
              << "; assertions=" << assertions << "; failures=" << failures
              << "; game_input_sent=false\n";
    return failures ? 1 : 0;
}
