#include "domain.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <functional>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void check(bool result, const char* label) {
    std::cout << (result ? "PASS " : "FAIL ") << label << '\n';
    if (!result) ++failures;
}
QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool write(const QString& path, const QByteArray& data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    check(directory.isValid(), "temporary_directory");
    if (!directory.isValid()) return 1;
    const QString baseline = directory.filePath(QStringLiteral("baseline.json"));
    const QString candidate = directory.filePath(QStringLiteral("candidate.json"));
    const QString snapshot = directory.filePath(QStringLiteral("snapshot.json"));
    QString error;
    AppState state;
    check(state.skins.size() == 12 && state.tasks.size() == 5, "synthetic_fixture_counts");
    check(state.saveTo(baseline, &error), "atomic_save");
    const QByteArray original = read(baseline);
    const QJsonObject source = QJsonDocument::fromJson(original).object();
    check(source.value(QStringLiteral("schema_version")).toInt() == 1
          && source.value(QStringLiteral("demo")).toBool(), "schema_and_demo_marker");

    AppState imported;
    check(imported.loadFrom(baseline, &error) && imported.saveTo(snapshot, &error)
          && read(snapshot) == original, "save_load_roundtrip");

    auto rejection = [&](const char* label, const std::function<void(QJsonObject&)>& mutate) {
        QJsonObject root = source;
        mutate(root);
        const bool saved = write(candidate, QJsonDocument(root).toJson());
        const QString previousPath = imported.configPath;
        const int previousLogs = imported.logs.size();
        const bool rejected = !imported.loadFrom(candidate, &error) && !error.isEmpty();
        const bool unchanged = imported.saveTo(snapshot, nullptr) && read(snapshot) == original
            && imported.configPath == previousPath && imported.logs.size() == previousLogs;
        check(saved && rejected && unchanged, label);
    };
    auto changeTask = [](QJsonObject& root, const QString& field, const QJsonValue& value) {
        auto array = root.value(QStringLiteral("tasks")).toArray();
        auto task = array.at(0).toObject(); task.insert(field, value); array[0] = task;
        root.insert(QStringLiteral("tasks"), array);
    };
    auto changeSkin = [](QJsonObject& root, const QString& field, const QJsonValue& value) {
        auto array = root.value(QStringLiteral("skins")).toArray();
        auto skin = array.at(0).toObject(); skin.insert(field, value); array[0] = skin;
        root.insert(QStringLiteral("skins"), array);
    };
    rejection("reject_range_transactional", [&](QJsonObject& root) { changeTask(root, QStringLiteral("minPrice"), 999999); });
    rejection("reject_unknown_skin_transactional", [&](QJsonObject& root) { changeTask(root, QStringLiteral("skinId"), QStringLiteral("missing")); });
    rejection("reject_quantity_zero_transactional", [&](QJsonObject& root) { changeTask(root, QStringLiteral("quantity"), 0); });
    rejection("reject_quantity_fraction_transactional", [&](QJsonObject& root) { changeTask(root, QStringLiteral("quantity"), 1.5); });
    rejection("reject_quantity_overflow_transactional", [&](QJsonObject& root) { changeTask(root, QStringLiteral("quantity"), 9999999999.0); });
    rejection("reject_quantity_ui_limit_transactional", [&](QJsonObject& root) { changeTask(root, QStringLiteral("quantity"), 10000); });
    rejection("reject_task_name_ui_limit_transactional", [&](QJsonObject& root) { changeTask(root, QStringLiteral("name"), QString(61, QChar(0x4EFB))); });
    rejection("reject_task_name_utf16_units_transactional", [&](QJsonObject& root) {
        changeTask(root, QStringLiteral("name"), QString::fromUtf8("\xF0\x9F\x98\x80").repeated(31));
    });
    rejection("reject_task_price_ui_limit_transactional", [&](QJsonObject& root) { changeTask(root, QStringLiteral("maxPrice"), 1000000000.0); });
    rejection("reject_task_min_price_precision_transactional", [&](QJsonObject& root) { changeTask(root, QStringLiteral("minPrice"), 1.001); });
    rejection("reject_task_max_price_precision_transactional", [&](QJsonObject& root) { changeTask(root, QStringLiteral("maxPrice"), 650.001); });
    rejection("reject_task_wear_ui_limit_transactional", [&](QJsonObject& root) { changeTask(root, QStringLiteral("maxWear"), 101); });
    rejection("reject_task_wear_precision_transactional", [&](QJsonObject& root) { changeTask(root, QStringLiteral("maxWear"), 5.0000001); });
    rejection("reject_negative_price_transactional", [&](QJsonObject& root) { changeSkin(root, QStringLiteral("price"), -1); });
    rejection("reject_null_wear_transactional", [&](QJsonObject& root) { changeSkin(root, QStringLiteral("wear"), QJsonValue()); });
    rejection("reject_string_price_transactional", [&](QJsonObject& root) { changeSkin(root, QStringLiteral("price"), QStringLiteral("628")); });
    rejection("reject_non_demo_transactional", [&](QJsonObject& root) { root.insert(QStringLiteral("demo"), false); });
    rejection("reject_unknown_schema_transactional", [&](QJsonObject& root) { root.insert(QStringLiteral("schema_version"), 2); });
    rejection("reject_duplicate_task_id_transactional", [&](QJsonObject& root) {
        auto array = root.value(QStringLiteral("tasks")).toArray(); array.append(array.at(0)); root.insert(QStringLiteral("tasks"), array);
    });
    rejection("reject_duplicate_skin_id_transactional", [&](QJsonObject& root) {
        auto array = root.value(QStringLiteral("skins")).toArray(); array.append(array.at(0)); root.insert(QStringLiteral("skins"), array);
    });
    check(write(candidate, QByteArray("{broken")) && !imported.loadFrom(candidate, &error)
          && !error.isEmpty() && imported.saveTo(snapshot) && read(snapshot) == original,
          "reject_invalid_json_transactional");
    check(write(candidate, QByteArray("[]")) && !imported.loadFrom(candidate, &error), "reject_non_object_root");
    check(!imported.loadFrom(directory.filePath(QStringLiteral("missing.json")), &error)
          && !error.isEmpty(), "missing_file_error");
    imported.skins[0].price = std::numeric_limits<double>::infinity();
    check(!imported.saveTo(snapshot, &error), "reject_nonfinite_save");
    check(imported.loadFrom(baseline, &error), "restore_valid_fixture");

    QJsonObject boundary = source;
    changeTask(boundary, QStringLiteral("name"), QString(60, QChar(0x4EFB)));
    changeTask(boundary, QStringLiteral("minPrice"), 0.29);
    changeTask(boundary, QStringLiteral("maxPrice"), 999999999.0);
    changeTask(boundary, QStringLiteral("maxWear"), 100.0);
    changeTask(boundary, QStringLiteral("quantity"), 9999);
    AppState boundaryState;
    check(write(candidate, QJsonDocument(boundary).toJson())
          && boundaryState.loadFrom(candidate, &error)
          && boundaryState.tasks.first().name.size() == 60
          && boundaryState.tasks.first().quantity == 9999, "accept_task_exact_ui_boundaries");
    changeTask(boundary, QStringLiteral("name"), QString::fromUtf8("\xF0\x9F\x98\x80").repeated(30));
    changeTask(boundary, QStringLiteral("minPrice"), 0.1 + 0.2);
    changeTask(boundary, QStringLiteral("maxPrice"), 999999998.99);
    changeTask(boundary, QStringLiteral("maxWear"), 99.123456);
    changeSkin(boundary, QStringLiteral("price"), 123.123456789);
    changeSkin(boundary, QStringLiteral("wear"), 101.123456789);
    check(write(candidate, QJsonDocument(boundary).toJson())
          && boundaryState.loadFrom(candidate, &error)
          && boundaryState.tasks.first().name.size() == 60
          && boundaryState.skins.first().price == 123.123456789
          && boundaryState.skins.first().wear == 101.123456789,
          "accept_task_float_noise_utf16_boundary_and_unrestricted_skin_precision");

    const QString csvPath = directory.filePath(QStringLiteral("prices.csv"));
    check(imported.exportPricesCsv(csvPath, &error), "csv_export");
    const QByteArray csv = read(csvPath);
    check(csv.startsWith(QByteArray("\xEF\xBB\xBF", 3)) && csv.count("DEMO_SYNTHETIC") == 12
          && csv.contains("price") && csv.contains("628.00"), "csv_utf8_demo_prices");
    imported.skins[0].name = QStringLiteral("=HYPERLINK(\"demo\")");
    check(imported.exportPricesCsv(csvPath, &error) && read(csvPath).contains("'=HYPERLINK(\"\"demo\"\")"),
          "csv_formula_escaping");
    imported.loadDemo();
    int changedCount = 0;
    QObject::connect(&imported, &AppState::changed, [&]() { ++changedCount; });
    imported.simulateTick();
    check(imported.simulatedScans == 0 && changedCount == 0, "paused_tick_noop");
    imported.startSimulation();
    const int firstStartLogs = imported.logs.size();
    const int firstStartSignals = changedCount;
    imported.startSimulation();
    check(imported.simulationRunning && imported.logs.size() == firstStartLogs
          && changedCount == firstStartSignals, "start_idempotent");
    imported.simulateTick();
    check(imported.simulatedScans == 4 && imported.simulatedMatches == 3
          && imported.simulatedSuccess == 3, "simulation_local_matching");
    imported.pauseSimulation();
    const int firstPauseLogs = imported.logs.size();
    const int firstPauseSignals = changedCount;
    imported.pauseSimulation();
    imported.simulateTick();
    check(!imported.simulationRunning && imported.logs.size() == firstPauseLogs
          && changedCount == firstPauseSignals && imported.simulatedSuccess == 3, "pause_idempotent_and_freezes_counts");
    imported.startSimulation();
    for (int i = 0; i < 10; ++i) imported.simulateTick();
    check(imported.simulatedSuccess == 9, "simulation_respects_task_quantities");
    imported.pauseSimulation();

    imported.loadDemo();
    for (auto& task : imported.tasks) task.enabled = false;
    imported.startSimulation();
    check(!imported.simulationRunning && imported.simulatedScans == 0, "no_enabled_tasks_no_start");
    imported.tasks[0].enabled = true;
    imported.tasks[0].quantity = 1;
    imported.startSimulation();
    imported.simulateTick();
    imported.simulateTick();
    check(!imported.simulationRunning && imported.simulatedSuccess == 1, "all_completed_autopause");
    const int scansBeforeReset = imported.simulatedScans;
    const int matchesBeforeReset = imported.simulatedMatches;
    const int successBeforeReset = imported.simulatedSuccess;
    const int signalsBeforeReset = changedCount;
    imported.tasks[0].skinId = QStringLiteral("demo-02");
    imported.resetTaskSimulation(imported.tasks[0].id);
    check(imported.tasks[0].status == QStringLiteral("待启动")
          && imported.simulatedScans == scansBeforeReset
          && imported.simulatedMatches == matchesBeforeReset
          && imported.simulatedSuccess == successBeforeReset
          && changedCount == signalsBeforeReset + 1
          && imported.logs.first().message.contains(QStringLiteral("统计保留")),
          "reset_task_preserves_aggregate_statistics_and_notifies");
    imported.startSimulation();
    imported.simulateTick();
    imported.simulateTick();
    check(!imported.simulationRunning && imported.simulatedSuccess == successBeforeReset + 1
          && imported.tasks[0].status == QStringLiteral("演示已完成")
          && imported.logs[1].message.contains(QStringLiteral("468.00")),
          "reset_completed_task_allows_new_skin_simulation");
    imported.tasks[0].enabled = false;
    imported.resetTaskSimulation(imported.tasks[0].id);
    check(imported.tasks[0].status == QStringLiteral("未启用"), "reset_disabled_task_keeps_disabled_status");
    const int signalsBeforeUnknownReset = changedCount;
    const int logsBeforeUnknownReset = imported.logs.size();
    imported.resetTaskSimulation(QStringLiteral("unknown-task-id"));
    check(changedCount == signalsBeforeUnknownReset && imported.logs.size() == logsBeforeUnknownReset,
          "reset_unknown_task_noop");
    imported.tasks[0].enabled = true;
    imported.startSimulation();
    check(imported.loadFrom(baseline, &error) && !imported.simulationRunning
          && imported.simulatedSuccess == 0 && imported.simulatedScans == 0, "successful_import_resets_runtime");
    for (int i = 0; i < 600; ++i) imported.addLog(QStringLiteral("DEMO"), QStringLiteral("synthetic log"));
    check(imported.logs.size() == 500, "bounded_log_buffer");
    // Run settings and task condition mirrored from the original assistant.
    AppState runState;
    const QJsonObject runDefaults = source.value(QStringLiteral("run_settings")).toObject();
    check(runDefaults.value(QStringLiteral("purchaseDelayMs")).toInt() == 830
          && runDefaults.value(QStringLiteral("hotkey")).toString() == QStringLiteral("F2")
          && runDefaults.value(QStringLiteral("burstClick")).toBool()
          && runDefaults.value(QStringLiteral("clickIntervalMs")).toInt() == 10
          && runDefaults.value(QStringLiteral("limitOrange")).toInt() == 10
          && runDefaults.value(QStringLiteral("skipLotteryPage")).toBool()
          && !runDefaults.value(QStringLiteral("refreshPage")).toBool()
          && source.value(QStringLiteral("tasks")).toArray().at(0).toObject()
                 .value(QStringLiteral("condition")).toString() == QStringLiteral("成色S"),
          "run_settings_original_defaults_saved");
    QJsonObject legacy = source;
    legacy.remove(QStringLiteral("run_settings"));
    {
        auto array = legacy.value(QStringLiteral("tasks")).toArray();
        auto task = array.at(0).toObject(); task.remove(QStringLiteral("condition")); array[0] = task;
        legacy.insert(QStringLiteral("tasks"), array);
    }
    runState.run.purchaseDelayMs = 1;
    check(write(candidate, QJsonDocument(legacy).toJson()) && runState.loadFrom(candidate, &error)
          && runState.run.purchaseDelayMs == 830 && runState.tasks.first().condition == QStringLiteral("不限"),
          "legacy_config_without_run_settings_loads_defaults");
    runState.run.profile = QStringLiteral("测试方案");
    runState.run.hotkey = QStringLiteral("F9");
    runState.run.purchaseDelayMs = 1200;
    runState.run.publicityStepMs = 2.5;
    runState.run.limitBlue = 0;
    runState.run.scheduleEnabled = true;
    runState.run.scheduleStart = QStringLiteral("08:30");
    runState.tasks.first().condition = QStringLiteral("成色A");
    AppState runReloaded;
    check(runState.saveTo(snapshot, &error) && runReloaded.loadFrom(snapshot, &error)
          && runReloaded.run.profile == QStringLiteral("测试方案") && runReloaded.run.hotkey == QStringLiteral("F9")
          && runReloaded.run.purchaseDelayMs == 1200 && runReloaded.run.publicityStepMs == 2.5
          && runReloaded.run.limitBlue == 0 && runReloaded.run.scheduleEnabled
          && runReloaded.run.scheduleStart == QStringLiteral("08:30")
          && runReloaded.tasks.first().condition == QStringLiteral("成色A"), "run_settings_custom_roundtrip");
    auto changeRun = [](QJsonObject& root, const QString& field, const QJsonValue& value) {
        auto run = root.value(QStringLiteral("run_settings")).toObject(); run.insert(field, value);
        root.insert(QStringLiteral("run_settings"), run);
    };
    check(imported.loadFrom(baseline, &error), "restore_fixture_before_run_rejections");
    rejection("reject_run_delay_range_transactional", [&](QJsonObject& root) { changeRun(root, QStringLiteral("purchaseDelayMs"), -1); });
    rejection("reject_run_hotkey_transactional", [&](QJsonObject& root) { changeRun(root, QStringLiteral("hotkey"), QStringLiteral("F13")); });
    rejection("reject_run_step_precision_transactional", [&](QJsonObject& root) { changeRun(root, QStringLiteral("queueFullStepMs"), 1.25); });
    rejection("reject_run_schedule_time_transactional", [&](QJsonObject& root) { changeRun(root, QStringLiteral("scheduleStart"), QStringLiteral("25:00")); });
    rejection("reject_run_flag_type_transactional", [&](QJsonObject& root) { changeRun(root, QStringLiteral("skipSuccessPage"), 1); });
    rejection("reject_run_settings_not_object_transactional", [&](QJsonObject& root) { root.insert(QStringLiteral("run_settings"), 5); });
    rejection("reject_task_condition_type_transactional", [&](QJsonObject& root) { changeTask(root, QStringLiteral("condition"), 3); });
    AppState conditionState;
    conditionState.tasks[0].condition = QStringLiteral("成色A");
    conditionState.startSimulation();
    conditionState.simulateTick();
    check(conditionState.tasks[0].status == QStringLiteral("演示等待条件")
          && conditionState.simulatedMatches == 2, "condition_filter_blocks_mismatched_simulation");
    std::cout << "RESULT " << (failures == 0 ? "PASS" : "FAIL") << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
