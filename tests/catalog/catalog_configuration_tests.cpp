#include "application/catalog_configuration.h"
#include "domain.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <iostream>

using relink::application::applyCatalogConfiguration;
using relink::application::catalogSkinId;
using relink::application::CatalogProjectionReport;

namespace {
QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QByteArray snapshot(const AppState& state) {
    return QJsonDocument(encodeV1Config(state.skins, state.tasks, state.run)).toJson();
}
Skin* skin(AppState& state, const QString& id) {
    for (auto& item : state.skins) if (item.id == id) return &item;
    return nullptr;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int count = 0, failures = 0;
    const auto check = [&](bool ok, const char* name) {
        ++count; failures += !ok;
        std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    };
    relink::catalog::Catalog catalog;
    QString error;
    check(relink::catalog::parseCatalog(read(argc > 1 ? QString::fromLocal8Bit(argv[1])
        : QStringLiteral("src/assets/catalog/skins.json")), &catalog, &error), "real_catalog_loads");
    AppState state;
    CatalogProjectionReport report;
    check(applyCatalogConfiguration(state, catalog, &report, &error), "empty_state_projects_real_catalog");
    check(state.skins.size() == 149 && state.tasks.isEmpty() && report.addedSkins == 149,
          "real_149_without_synthetic_tasks");
    bool unknown = true, noFollows = true, correctIds = true;
    for (const auto& item : state.skins) {
        unknown &= !item.priceKnown && !item.wearKnown && !item.changeKnown
            && item.price == 0 && item.wear == 0 && item.change == 0
            && item.condition == QStringLiteral("未采集") && item.rarity == QStringLiteral("待核对");
        noFollows &= !item.followed;
        correctIds &= item.id == catalogSkinId(item.catalogProductId);
    }
    check(unknown, "no_market_condition_or_quality_fabrication");
    check(noFollows, "no_fake_follows");
    check(correctIds, "stable_source_product_ids");
    auto* aug = skin(state, catalogSkinId("10602"));
    check(aug && aug->menuColor == "purple" && aug->skinSeries == QStringLiteral("天命")
          && aug->catalogProductId == "10602", "metadata_projected_without_color_quality_guess");
    check(skin(state, catalogSkinId("10100"))->variant == QStringLiteral("极品")
          && skin(state, catalogSkinId("10101"))->variant == QStringLiteral("优品"), "distinct_variants_preserved");
    aug->followed = true;
    aug->priceKnown = aug->wearKnown = aug->changeKnown = true;
    aug->price = 231.5; aug->wear = .25; aug->change = -1.2;
    aug->condition = QStringLiteral("成色S");
    aug->dataSource = "confirmed_user_observation";
    state.tasks.append({"user-task", QStringLiteral("我的收藏条件"), aug->id,
        10, 600, 5, 3, false, QStringLiteral("用户状态"), QStringLiteral("成色A")});
    state.run.purchaseDelayMs = 1473;
    const auto unchanged = snapshot(state);
    check(applyCatalogConfiguration(state, catalog, &report, &error) && snapshot(state) == unchanged,
          "repeat_projection_preserves_follows_tasks_parameters_and_observations");
    check(state.tasks[0].status == QStringLiteral("用户状态") && state.run.purchaseDelayMs == 1473,
          "task_runtime_status_and_run_settings_untouched");

    AppState numeric;
    Skin numericSkin;
    numericSkin.id = "10602"; numericSkin.name = "old label"; numericSkin.series = "old season";
    numericSkin.condition = QStringLiteral("成色S"); numericSkin.rarity = "old quality";
    numericSkin.followed = true;
    numeric.skins.append(numericSkin);
    numeric.tasks.append({"linked", "linked user task", "10602", 19, 501, 3, 8, false, "paused", QStringLiteral("成色B")});
    check(applyCatalogConfiguration(numeric, catalog, &report, &error)
          && numeric.tasks[0].skinId == catalogSkinId("10602")
          && numeric.tasks[0].minPrice == 19 && numeric.tasks[0].maxPrice == 501
          && numeric.tasks[0].quantity == 8 && !numeric.tasks[0].enabled,
          "legacy_numeric_id_remaps_only_task_reference");
    check(skin(numeric, catalogSkinId("10602"))->followed, "numeric_id_follow_preserved");

    AppState demo;
    demo.loadTestFixture();
    demo.run.purchaseDelayMs = 2468;
    check(applyCatalogConfiguration(demo, catalog, &report, &error), "default_fixture_migration_succeeds");
    check(demo.skins.size() == 149 && demo.tasks.isEmpty()
          && report.removedDefaultSkins == 12 && report.removedDefaultTasks == 5,
          "strict_12_skins_5_tasks_removed");
    check(demo.run.purchaseDelayMs == 2468, "migration_preserves_user_run_parameters");
    demo.startSimulation(); demo.simulateTick();
    check(!demo.property("testFixture").toBool() && !demo.simulationRunning
          && demo.simulatedScans == 0, "migrated_state_cannot_restart_synthetic_runtime");
    const auto migratedOnce = snapshot(demo);
    check(applyCatalogConfiguration(demo, catalog, &report, &error) && snapshot(demo) == migratedOnce
          && report.removedDefaultSkins == 0 && report.removedDefaultTasks == 0,
          "migration_is_idempotent");

    AppState disabledDefaults;
    disabledDefaults.loadTestFixture();
    for (auto& item : disabledDefaults.tasks) item.enabled = false;
    for (auto& item : disabledDefaults.skins) item.followed = !item.followed;
    check(applyCatalogConfiguration(disabledDefaults, catalog, &report, &error)
          && disabledDefaults.skins.size() == 149 && disabledDefaults.tasks.isEmpty()
          && report.removedDefaultSkins == 12 && report.removedDefaultTasks == 5,
          "historical_all_disabled_defaults_and_follow_toggles_removed");

    AppState historicalSource;
    historicalSource.loadTestFixture();
    auto historicalJson = encodeV1Config(historicalSource.skins, historicalSource.tasks, historicalSource.run);
    auto historicalTasks = historicalJson["tasks"].toArray();
    for (int index = 0; index < historicalTasks.size(); ++index) {
        auto item = historicalTasks[index].toObject();
        item.remove("condition");
        item["enabled"] = false;
        historicalTasks[index] = item;
    }
    historicalJson["tasks"] = historicalTasks;
    AppState historical;
    check(decodeV1Config(QJsonDocument(historicalJson).toJson(), historical.skins, historical.tasks,
          historical.run, &error) && !historical.tasks.first().conditionExplicit
          && historical.tasks.first().condition == QStringLiteral("不限"),
          "missing_legacy_condition_records_absence_not_user_choice");
    check(applyCatalogConfiguration(historical, catalog, &report, &error)
          && historical.skins.size() == 149 && historical.tasks.isEmpty()
          && report.removedDefaultSkins == 12 && report.removedDefaultTasks == 5,
          "actual_legacy_missing_condition_disabled_defaults_migrate_to_149_zero_tasks");

    auto changedHistoricalJson = historicalJson;
    auto changedHistoricalTasks = historicalTasks;
    auto changedHistoricalTask = changedHistoricalTasks[0].toObject();
    changedHistoricalTask["maxPrice"] = 777;
    changedHistoricalTasks[0] = changedHistoricalTask;
    changedHistoricalJson["tasks"] = changedHistoricalTasks;
    AppState changedHistorical;
    check(decodeV1Config(QJsonDocument(changedHistoricalJson).toJson(), changedHistorical.skins,
          changedHistorical.tasks, changedHistorical.run, &error)
          && applyCatalogConfiguration(changedHistorical, catalog, &report, &error)
          && changedHistorical.tasks.size() == 1 && changedHistorical.tasks[0].maxPrice == 777,
          "missing_condition_does_not_relax_other_default_task_fingerprints");

    auto explicitJson = historicalJson;
    auto explicitTasks = historicalTasks;
    for (int index = 0; index < explicitTasks.size(); ++index) {
        auto item = explicitTasks[index].toObject();
        item["condition"] = QStringLiteral("不限");
        explicitTasks[index] = item;
    }
    explicitJson["tasks"] = explicitTasks;
    AppState explicitNoCondition;
    check(decodeV1Config(QJsonDocument(explicitJson).toJson(), explicitNoCondition.skins,
          explicitNoCondition.tasks, explicitNoCondition.run, &error)
          && explicitNoCondition.tasks.first().conditionExplicit
          && applyCatalogConfiguration(explicitNoCondition, catalog, &report, &error)
          && explicitNoCondition.tasks.size() == 5 && explicitNoCondition.skins.size() == 154,
          "explicit_no_condition_user_rules_all_remain");
    check(Task{}.conditionExplicit, "new_tasks_have_explicit_condition");

    AppState customized;
    customized.loadTestFixture();
    customized.tasks[0].maxPrice = 777;
    customized.tasks[0].condition = QStringLiteral("成色B");
    check(applyCatalogConfiguration(customized, catalog, &report, &error)
          && customized.tasks.size() == 1 && customized.tasks[0].maxPrice == 777
          && customized.tasks[0].condition == QStringLiteral("成色B")
          && customized.tasks[0].skinId == "demo-01", "user_modified_default_task_never_deleted");
    const auto* retained = skin(customized, "demo-01");
    check(retained && !retained->priceKnown && !retained->wearKnown && !retained->changeKnown
          && retained->price == 0 && retained->wear == 0 && retained->change == 0
          && retained->condition == QStringLiteral("未采集")
          && retained->rarity == QStringLiteral("待核对")
          && report.clearedSyntheticObservations == 1,
          "referenced_default_skin_preserved_but_fake_market_fields_cleared");
    check(customized.skins.size() == 150 && report.removedDefaultSkins == 11
          && report.removedDefaultTasks == 4, "only_proven_unreferenced_defaults_removed");
    const auto retainedSnapshot = snapshot(customized);
    check(applyCatalogConfiguration(customized, catalog, &report, &error)
          && snapshot(customized) == retainedSnapshot, "retained_legacy_migration_idempotent");

    AppState editedCondition;
    editedCondition.loadTestFixture();
    editedCondition.tasks[0].condition = QStringLiteral("不限");
    check(applyCatalogConfiguration(editedCondition, catalog, &report, &error)
          && editedCondition.tasks.size() == 1
          && editedCondition.tasks[0].condition == QStringLiteral("不限"),
          "condition_only_user_edit_is_not_mistaken_for_default");

    AppState legacy;
    Skin custom;
    custom.id = "demo-custom-user"; custom.name = QStringLiteral("用户自己的皮肤");
    custom.series = QStringLiteral("自定义赛季"); custom.condition = QStringLiteral("成色A");
    custom.rarity = QStringLiteral("用户品质"); custom.price = 127.5; custom.wear = 7.75;
    custom.priceKnown = custom.wearKnown = true; custom.followed = true; custom.dataSource = "user";
    legacy.skins.append(custom);
    legacy.tasks.append({"private", "private task", custom.id, 80, 150, 9, 2, true, "keep", QStringLiteral("成色A")});
    const auto customBefore = encodeV1Config(legacy.skins, legacy.tasks, legacy.run);
    check(applyCatalogConfiguration(legacy, catalog, &report, &error)
          && legacy.skins.size() == 150 && legacy.tasks.size() == 1,
          "unknown_user_skin_and_task_preserved");
    const auto customAfter = encodeV1Config({*skin(legacy, custom.id)}, legacy.tasks, legacy.run);
    check(customBefore["skins"] == customAfter["skins"] && customBefore["tasks"] == customAfter["tasks"],
          "unknown_user_values_unchanged_even_with_demo_prefix");

    AppState renamedFixture;
    renamedFixture.skins.append(custom);
    renamedFixture.skins.last().dataSource = "test_fixture";
    renamedFixture.tasks = legacy.tasks;
    check(applyCatalogConfiguration(renamedFixture, catalog, &report, &error)
          && renamedFixture.tasks.size() == 1 && renamedFixture.tasks.first().maxPrice == 150
          && skin(renamedFixture, custom.id) && skin(renamedFixture, custom.id)->name == custom.name
          && !skin(renamedFixture, custom.id)->priceKnown && !skin(renamedFixture, custom.id)->wearKnown,
          "renamed_synthetic_observations_not_relabelled_as_real");

    AppState ambiguous;
    numericSkin.catalogProductId = "10602"; numericSkin.id = "first";
    ambiguous.skins.append(numericSkin); numericSkin.id = "second"; ambiguous.skins.append(numericSkin);
    const auto ambiguousBefore = snapshot(ambiguous);
    check(!applyCatalogConfiguration(ambiguous, catalog, &report, &error)
          && snapshot(ambiguous) == ambiguousBefore, "ambiguous_old_identity_rejected_without_loss");
    AppState missing;
    missing.tasks.append({"orphan", "orphan", "absent"});
    const auto missingBefore = snapshot(missing);
    check(!applyCatalogConfiguration(missing, catalog, &report, &error)
          && snapshot(missing) == missingBefore, "orphan_task_not_silently_discarded");
    const auto stateBeforeInvalid = snapshot(state);
    auto invalid = catalog; invalid.skins.append(invalid.skins[0]);
    check(!applyCatalogConfiguration(state, invalid, &report, &error)
          && snapshot(state) == stateBeforeInvalid, "invalid_catalog_leaves_configuration_unchanged");
    QTemporaryDir temp;
    const auto config = temp.filePath("catalog-config.json");
    check(state.saveTo(config, &error), "catalog_state_valid_in_existing_codec");
    AppState reopened;
    check(reopened.loadFrom(config, &error) && snapshot(reopened) == snapshot(state), "catalog_metadata_and_unknown_flags_persist");
    std::cout << "CATALOG_CONFIGURATION_TESTS=" << (failures ? "FAIL" : "PASS")
              << "; assertions=" << count << "; failures=" << failures << '\n';
    return failures ? 1 : 0;
}
