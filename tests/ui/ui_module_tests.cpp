// Independent page tests: no MainWindow, WorkspaceController, SQLite or live I/O.
#include "domain.h"
#include "application/startup_config.h"
#include "application/workspace/workspace_projection.h"
#include "ui/pages/task_page.h"
#include "ui/pages/run_settings_page.h"
#include "ui/pages/workspace_records_panel.h"
#include "ui/dialogs/task_editor_dialog.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimeEdit>
#include <cstdio>

namespace {
int assertions = 0, failures = 0;
void check(bool passed, const char* name) {
    ++assertions;
    if (!passed) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}
template<class T> T* child(QObject& parent, const char* name) {
    auto* result = parent.findChild<T*>(QString::fromLatin1(name));
    check(result != nullptr, name);
    if (!result) { std::fflush(stderr); std::exit(2); }
    return result;
}
QByteArray config(const AppState& state) {
    return QJsonDocument(encodeV1Config(state.skins, state.tasks, state.run)).toJson(QJsonDocument::Compact);
}
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setOrganizationName("RelinkStudioModuleTest");
    app.setApplicationName("RelinkStudioModuleTest");
    using namespace relink::ui;
    AppState state;
    const QByteArray initial = config(state);
    int modelSignals = 0;
    QObject::connect(&state, &AppState::changed, &app, [&] { ++modelSignals; });

    // Render is not a mutation. Controls and callbacks belong to the page.
    RunSettingsPage run(&state);
    run.refresh(); run.refresh();
    check(config(state) == initial && modelSignals == 0, "run refresh is read-only");
    const auto originalRun = state.run;
    const char* requiredRun[] = {"runProfileEdit", "runHotkeyCombo", "runScheduleStart", "runScheduleStop",
        "runScheduleToggle", "runPurchaseDelay", "runDynamicDelay", "runQueueTrigger", "runQueueStep",
        "runPublicityTrigger", "runPublicityStep", "runBurstClick", "runClickInterval", "runLimitOrange",
        "runLimitPurple", "runLimitBlue", "runRefreshPage", "runSkipLottery", "runSkipSuccess",
        "runAutoCollect", "runCollectOsd", "runManageTasksLink", "runRecordsLink", "runHelpLink"};
    for (const char* name : requiredRun) child<QWidget>(run, name);
    auto* delay = child<QSpinBox>(run, "runPurchaseDelay");
    check(delay->value() == originalRun.purchaseDelayMs, "initial delay rendered");
    delay->setValue(951);
    check(state.run.purchaseDelayMs == 951, "user delay edit updates only legacy model");
    auto* dynamic = child<QCheckBox>(run, "runDynamicDelay");
    dynamic->click();
    check(state.run.dynamicDelay != originalRun.dynamicDelay, "user toggle updates model");
    auto* schedule = child<QCheckBox>(run, "runScheduleToggle");
    auto* start = child<QTimeEdit>(run, "runScheduleStart");
    check(start->isEnabled() == originalRun.scheduleEnabled, "schedule initial enabled state");
    schedule->setChecked(true);
    start->setTime(QTime(10, 25));
    check(state.run.scheduleEnabled && start->isEnabled() && state.run.scheduleStart == "10:25", "schedule controls remain bound");
    int saves = 0, imports = 0, help = 0, navigations = 0, destination = -1;
    QObject::connect(&run, &RunSettingsPage::saveRequested, &app, [&] { ++saves; });
    QObject::connect(&run, &RunSettingsPage::importPreviewRequested, &app, [&] { ++imports; });
    QObject::connect(&run, &RunSettingsPage::helpRequested, &app, [&] { ++help; });
    QObject::connect(&run, &RunSettingsPage::navigateRequested, &app, [&](int page) { ++navigations; destination = page; });
    child<QPushButton>(run, "saveRunSettingsButton")->click();
    child<QPushButton>(run, "runImportPreviewButton")->click();
    child<QPushButton>(run, "runHelpLink")->click();
    child<QPushButton>(run, "runManageTasksLink")->click();
    check(saves == 1 && imports == 1 && help == 1 && navigations == 1 && destination == 2, "one click produces one intent");
    run.refresh();
    check(saves == 1 && imports == 1 && help == 1 && navigations == 1, "refresh does not emit intents");
    AppState independentState;
    RunSettingsPage independent(&independentState);
    independent.refresh();
    check(independentState.run.purchaseDelayMs == originalRun.purchaseDelayMs, "run pages do not share state");
    run.showSaveResult(QStringLiteral("保存完成"));
    check(child<QLabel>(run, "settingsMessage")->text() == QStringLiteral("保存完成"), "save result owned by run page");
    {
        QPointer<QWidget> workspacePanel = new QWidget;
        auto* composed = new RunSettingsPage(&state, workspacePanel);
        check(composed->isAncestorOf(workspacePanel), "injected panel becomes owned child");
        delete composed;
        check(workspacePanel.isNull(), "injected panel destroyed with owner");
    }

    TaskPage tasks(&state);
    const auto beforeTaskRender = config(state);
    const int signalsBefore = modelSignals;
    tasks.refresh(); tasks.refresh();
    check(config(state) == beforeTaskRender && signalsBefore == modelSignals, "task refresh is read-only");
    auto* table = child<QTableWidget>(tasks, "taskTable");
    check(table->rowCount() == state.tasks.size() && table->columnCount() == 8, "task rows and columns preserved");
    check(table->item(0, 0)->data(Qt::UserRole).toString() == state.tasks[0].id, "task stable ID retained");
    const bool enabled = state.tasks[0].enabled;
    auto* toggle = table->cellWidget(0, 6)->findChild<QCheckBox*>();
    check(toggle != nullptr, "task owns toggle");
    if (toggle) toggle->click();
    check(state.tasks[0].enabled != enabled && modelSignals == signalsBefore + 1, "task toggle mutates once");
    tasks.setSimulationAvailable(false);
    check(!child<QPushButton>(tasks, "tasksSimulationButton")->isEnabled(), "simulation availability controlled by shell");
    tasks.setCompact(true);
    check(table->verticalHeader()->defaultSectionSize() == 46, "compact density retained");
    tasks.setCompact(false);
    check(table->verticalHeader()->defaultSectionSize() == 56, "standard density retained");
    table->selectRow(0);
    tasks.refresh();
    check(table->selectionModel()->selectedRows().size() == 1, "selection survives refresh");

    // A dialog edits a local draft. It never commits directly to AppState.
    const QByteArray beforeDialog = config(state);
    TaskEditorDialog cancelled(&state, &state.tasks[0]);
    child<QLineEdit>(cancelled, "taskName")->setText(QStringLiteral("取消的草稿"));
    cancelled.reject();
    check(config(state) == beforeDialog, "cancelled draft never writes model");
    TaskEditorDialog accepted(&state, &state.tasks[0]);
    child<QLineEdit>(accepted, "taskName")->setText(QStringLiteral("仅编辑草稿"));
    child<QSpinBox>(accepted, "taskQuantity")->setValue(7);
    child<QPushButton>(accepted, "taskSaveButton")->click();
    check(accepted.result() == QDialog::Accepted && accepted.task().quantity == 7, "accepted draft exposes edited values");
    check(accepted.task().id == state.tasks[0].id && config(state) == beforeDialog, "dialog retains ID without model write");
    TaskEditorDialog invalid(&state);
    child<QLineEdit>(invalid, "taskName")->setText(QStringLiteral("价格测试"));
    child<QDoubleSpinBox>(invalid, "taskMinPrice")->setValue(1000);
    child<QDoubleSpinBox>(invalid, "taskMaxPrice")->setValue(1);
    child<QPushButton>(invalid, "taskSaveButton")->click();
    check(invalid.result() != QDialog::Accepted && !child<QLabel>(invalid, "taskValidationLabel")->text().isEmpty(), "invalid draft not accepted");

    // The records module consumes plain DTOs and is linked without SQLite.
    relink::workspace::WorkspaceProjection projection;
    projection.opened = true;
    projection.selectedRunId = "run-a";
    relink::workspace::RunRow first;
    first.id = "run-a"; first.profileName = QStringLiteral("方案甲"); first.profileRevision = 2; first.state = "Completed";
    auto second = first; second.id = "run-b";
    projection.runs = {first, second};
    relink::workspace::RecordRow event;
    event.type = "ReceiptConfirmed"; event.eventId = "event-a"; event.runId = "run-a";
    event.observedPrice = "650.00"; event.confirmedPrice = "649.00";
    event.seq = 1; event.atMonoMs = 123; event.source = "synthetic_replay";
    event.clockDomainId = "clock-a";
    projection.records = {event};
    WorkspaceRecordsPanel records;
    int selections = 0, exports = 0;
    QString selected;
    QObject::connect(&records, &WorkspaceRecordsPanel::runSelected, &app, [&](const QString& id) { ++selections; selected = id; });
    QObject::connect(&records, &WorkspaceRecordsPanel::exportRequested, &app, [&] { ++exports; });
    records.refresh(projection); records.refresh(projection);
    check(selections == 0 && exports == 0, "records refresh never selects or exports");
    auto* runs = child<QComboBox>(records, "workspaceRunCombo");
    auto* rows = child<QTableWidget>(records, "workspaceRecordsTable");
    check(runs->currentData().toString() == "run-a" && runs->count() == 3, "run IDs and current workspace entry retained");
    check(rows->rowCount() == 1 && rows->item(0, 1)->data(Qt::UserRole).toString() == "ReceiptConfirmed", "raw event type retained");
    check(rows->item(0, 3)->text() == "650.00 / 649.00", "observation and confirmed prices remain distinct");
    check(rows->item(0, 0)->toolTip().contains("event-a") && rows->columnWidth(1) == 124, "event tooltip and column width retained");
    runs->setCurrentIndex(2);
    check(selections == 1 && selected == "run-b", "user selection emits stable ID once");
    auto* exportButton = child<QPushButton>(records, "workspaceExportCsvButton");
    exportButton->click();
    check(exports == 1, "export is a command intent only");
    projection.active = true;
    records.refresh(projection);
    check(!runs->isEnabled() && selections == 1, "active projection locks selection without commands");
    projection.active = false; projection.dirty = true;
    records.refresh(projection);
    check(!runs->isEnabled(), "dirty projection locks selection");
    projection.dirty = false; projection.records.clear();
    records.refresh(projection);
    check(rows->rowCount() == 0 && !exportButton->isEnabled(), "empty projection clears rows and disables export");
    records.refresh(projection, QStringLiteral("诊断 <tag>"));
    auto* recordStatus = child<QLabel>(records, "workspaceRecordsState");
    check(recordStatus->textFormat() == Qt::PlainText && recordStatus->text().contains("<tag>"), "diagnostics stay plain text");

    QPointer<RunSettingsPage> transient = new RunSettingsPage(&state);
    QObject::connect(&state, &AppState::changed, transient, &RunSettingsPage::refresh);
    QPointer<QSpinBox> transientControl = transient->findChild<QSpinBox*>("runPurchaseDelay");
    delete transient;
    state.notifyChanged();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(transient.isNull() && transientControl.isNull(), "destroyed page releases controls and refresh context");
    check(!state.simulationRunning && state.simulatedScans == 0, "module tests did not start simulation");

    QTemporaryDir directory;
    check(directory.isValid(), "isolated startup fixture directory");
    const QString broken = directory.filePath("broken.json");
    { QFile file(broken); check(file.open(QIODevice::WriteOnly) && file.write("{broken") == 7, "write corrupt fixture"); }
    AppState recovered;
    const QString target = relink::application::prepareStartupConfig(recovered, broken, true);
    QFile original(broken); original.open(QIODevice::ReadOnly);
    check(target != broken && original.readAll() == "{broken", "shared startup preserves invalid original");
    QString error;
    check(recovered.saveTo(target, &error), "shared startup saves recovered copy");
    std::printf("UI_MODULE_TESTS=%s; assertions=%d; failures=%d; mainwindow=false; sqlite=false; offscreen=true\n",
                failures == 0 ? "PASS" : "FAIL", assertions, failures);
    return failures == 0 ? 0 : 1;
}
