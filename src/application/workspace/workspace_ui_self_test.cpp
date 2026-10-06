#include "workspace_ui_self_test.h"
#include "workspace_controller.h"
#include "mainwindow.h"
#include "domain.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <cstdio>

namespace relink::workspace {
bool runWorkspaceUiSelfTest(MainWindow& window, WorkspaceController& workspace, const QString& outputDirectory)
{
    QDir().mkpath(outputDirectory);
    int failures = 0;
    QJsonArray checks;
    const auto check = [&](bool result, const char* name) {
        checks.append(QJsonObject{{QStringLiteral("check"), QString::fromLatin1(name)}, {QStringLiteral("passed"), result}});
        std::printf("UI_WORKSPACE_%s=%s\n", name, result ? "PASS" : "FAIL");
        if (!result) ++failures;
    };
    const auto flush = [] {
        QApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
    };
    const auto click = [&](const char* name) {
        auto* button = window.findChild<QPushButton*>(name);
        const bool ready = button && button->isEnabled();
        if (ready) button->click();
        flush();
        return ready;
    };
    const auto answerPrompt = [&](const char* objectName, QMessageBox::ButtonRole role, const auto& trigger) {
        bool answered = false;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &window, [&] {
            auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (prompt && prompt->objectName() == QLatin1String(objectName)) {
                for (auto* button : prompt->buttons()) {
                    if (prompt->buttonRole(button) == role) {
                        answered = true;
                        button->click();
                        return;
                    }
                }
            }
            if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) modal->reject();
        });
        timer.start(20);
        trigger();
        timer.stop();
        return answered;
    };
    auto* profiles = window.findChild<QComboBox*>("workspaceProfileCombo");
    auto* listings = window.findChild<QTableWidget*>("workspaceListingsTable");
    auto* records = window.findChild<QTableWidget*>("workspaceRecordsTable");
    auto* error = window.findChild<QLabel*>("workspaceErrorLabel");
    check(workspace.projection().opened && profiles && listings && records && error, "WIDGETS_AND_STORE");
    if (!profiles || !listings || !records) return false;

    if (workspace.projection().active) workspace.stop();
    workspace.discardReviewDraft();
    check(click("saveRunSettingsButton"), "SAVE_LEGACY_TEST_DRAFT");
    check(workspace.setMode(QStringLiteral("Replay")), "REPLAY_MODE");
    auto* mode = window.findChild<QComboBox*>("workspaceModeCombo");
    auto* delay = window.findChild<QSpinBox*>("runPurchaseDelay");
    if (mode && delay) {
        const int previousDelay = delay->value();
        delay->setValue(previousDelay + 1);
        check(answerPrompt("workspaceDirtyDialog", QMessageBox::RejectRole, [&] {
            mode->setCurrentIndex(mode->findData(QStringLiteral("Demo")));
        }) && workspace.projection().mode == QStringLiteral("Replay"), "DIRTY_MODE_RETURN_PRESERVES_MODE");
        check(answerPrompt("workspaceDirtyDialog", QMessageBox::AcceptRole, [&] {
            mode->setCurrentIndex(mode->findData(QStringLiteral("Demo")));
        }) && workspace.projection().mode == QStringLiteral("Demo") && delay->value() == previousDelay + 1,
            "DIRTY_MODE_CONTINUE_PRESERVES_DRAFT");
        delay->setValue(previousDelay);
        click("saveRunSettingsButton");
        mode->setCurrentIndex(mode->findData(QStringLiteral("Replay")));
        flush();
    } else check(false, "DIRTY_MODE_CONTROLS_EXIST");
    profiles->setCurrentIndex(profiles->findData(WorkspaceController::builtinProfileId()));
    flush();
    check(workspace.projection().selectedProfileId == WorkspaceController::builtinProfileId(), "EXPLICIT_FIXTURE_SELECTION");
    check(click("replayStartButton") && workspace.projection().active, "START_PERSISTENT_RUN");
    const QString completedRun = workspace.projection().selectedRunId;
    const auto before = workspace.projection();
    const QString connection = QStringLiteral("ui-pr11-lock-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        auto blocker = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        blocker.setDatabaseName(before.databasePath);
        bool locked = blocker.open();
        {
            QSqlQuery lock(blocker);
            locked = locked && lock.exec(QStringLiteral("BEGIN IMMEDIATE"));
            check(locked, "REAL_WRITE_LOCK_ACQUIRED");
            if (locked) {
                click("replayStepButton");
                check(workspace.projection().fixtureStep == before.fixtureStep
                          && workspace.projection().records.size() == before.records.size()
                          && workspace.projection().confirmedSuccess == before.confirmedSuccess,
                      "WRITE_FAILURE_PRESERVES_COMMITTED_VIEW");
                check(error && error->text().contains(QStringLiteral("DB_BUSY")), "WRITE_FAILURE_VISIBLE");
                check(lock.exec(QStringLiteral("ROLLBACK")), "RELEASE_WRITE_LOCK");
            }
        }
        blocker.close();
    }
    QSqlDatabase::removeDatabase(connection);
    check(click("replayStepButton") && workspace.projection().fixtureStep == 1, "STEP_AFTER_LOCK_RELEASE");
    check(click("replayPauseButton") && workspace.projection().runState == QStringLiteral("Paused"), "PERSISTENT_PAUSE");
    check(click("replayResumeButton") && workspace.projection().runState != QStringLiteral("Paused"), "PERSISTENT_RESUME");
    check(click("replayAdvanceButton"), "ADVANCE_CONTROL");
    const auto finished = workspace.projection();
    check(!finished.active && finished.confirmedSuccess == 1 && finished.unknownCount == 1
              && finished.reservationCount == 1, "COMMITTED_LEDGER_COUNTS");
    check(listings->rowCount() == finished.listings.size() && finished.listings.size() >= 2, "LISTING_ROWS_FROM_PROJECTION");
    bool distinctListings = false, staleVisible = false, missingVisible = false, distinctPrices = false;
    for (const auto& row : finished.listings) {
        staleVisible |= row.stale;
        missingVisible |= !row.missingFields.isEmpty();
        distinctPrices |= !row.observedPrice.isEmpty() && !row.confirmedPrice.isEmpty();
        for (const auto& other : finished.listings)
            distinctListings |= row.productId == other.productId && row.listingId != other.listingId;
    }
    check(distinctListings, "SAME_PRODUCT_DISTINCT_LISTINGS");
    check(staleVisible && missingVisible, "STALE_AND_MISSING_FIELDS");
    check(distinctPrices, "OBSERVED_AND_CONFIRMED_PRICE_FIELDS");
    check(records->rowCount() == finished.records.size() && records->rowCount() > 0, "RECORDS_FROM_COMMITTED_EVENTS");
    const auto* completedState = window.findChild<QLabel*>("replayStateLabel");
    check(completedState && completedState->text().contains(QStringLiteral("已完成"))
              && completedState->property("status").toString() == QStringLiteral("Completed"), "CHINESE_STATE_WITH_RAW_STATUS");
    check(records->rowCount() > 0 && records->item(0, 1)
              && records->item(0, 1)->text() == QStringLiteral("开始回放")
              && records->item(0, 1)->data(Qt::UserRole).toString() == QStringLiteral("Start"), "CHINESE_EVENT_WITH_RAW_IDENTITY");
    check(records->rowCount() > 0 && records->item(0, 1)
              && records->columnWidth(1) >= records->fontMetrics().horizontalAdvance(records->item(0, 1)->text()) + 32,
          "EVENT_COLUMN_SHOWS_COMPLETE_NAME");
    check(finished.imageFileWriteCount == 0, "NO_CAPTURE_IMAGE_WRITES");
    window.setPage(7); flush();
    check(window.grab().save(outputDirectory + QStringLiteral("/run.png")), "RUN_SNAPSHOT");
    window.setPage(5); flush();
    check(window.grab().save(outputDirectory + QStringLiteral("/records.png")), "RECORDS_SNAPSHOT");
    const QString exportPath = outputDirectory + QStringLiteral("/replay-records.csv");
    check(window.exportWorkspaceRecordsTo(exportPath), "CSV_EXPORT_UI_PATH");
    QFile csv(exportPath);
    check(csv.open(QIODevice::ReadOnly) && csv.readAll().contains("Replay"), "CSV_MARKED_REPLAY");

    window.previewImportDemo(); flush();
    auto* preview = window.findChild<QDialog*>("importPreviewDialog");
    auto* apply = preview ? preview->findChild<QPushButton*>("importPreviewApplyButton") : nullptr;
    auto* review = preview ? preview->findChild<QPushButton*>("importPreviewReviewButton") : nullptr;
    check(preview && apply && !apply->isEnabled() && review && review->isEnabled(), "PREVIEW_STAYS_READ_ONLY");
    if (review) review->click();
    flush();
    auto* dialog = window.findChild<QDialog*>("profileReviewDialog");
    check(dialog != nullptr, "REVIEW_DIALOG");
    QString savedProfile;
    if (dialog) {
        auto* name = dialog->findChild<QLineEdit*>("profileReviewName");
        auto* quantum = dialog->findChild<QLineEdit*>("profileReviewQuantum");
        auto* unit = dialog->findChild<QLineEdit*>("profileReviewUnit");
        auto* save = dialog->findChild<QPushButton*>("profileReviewSaveButton");
        check(name && quantum && unit && save, "REVIEW_FIELDS");
        if (name) name->setText(QStringLiteral("离屏审定方案"));
        if (quantum) quantum->setText(QStringLiteral("0.01"));
        if (unit) unit->setText(QStringLiteral("演示币"));
        flush();
        check(workspace.projection().dirty, "EDIT_MARKS_REVIEW_DIRTY");
        check(!workspace.setMode(QStringLiteral("Demo")) && workspace.projection().mode == QStringLiteral("Replay"), "DIRTY_MODE_SWITCH_REJECTED");
        const int oldProfileCount = workspace.projection().profiles.size();
        check(save && !save->isEnabled(), "INCOMPLETE_REVIEW_SAVE_DISABLED");
        for (const char* field : {"profileReviewProducts", "profileReviewPriceRange", "profileReviewTaxonomy", "profileReviewUnknownColumns", "profileReviewQuantityUnbound"}) {
            auto* confirmation = dialog->findChild<QCheckBox*>(field);
            check(confirmation != nullptr, field);
            if (confirmation) confirmation->setChecked(true);
        }
        flush();
        check(save && save->isEnabled(), "COMPLETE_REVIEW_SAVE_ENABLED");
        if (save && save->isEnabled()) save->click();
        flush();
        check(!workspace.projection().dirty && workspace.projection().profiles.size() == oldProfileCount + 1, "REVIEW_SAVED_NOT_ACTIVATED");
        for (const auto& profile : workspace.projection().profiles)
            if (profile.name == QStringLiteral("离屏审定方案")) savedProfile = profile.id;
        check(!savedProfile.isEmpty(), "SAVED_PROFILE_LISTED");
        if (auto* remaining = window.findChild<QDialog*>("profileReviewDialog")) remaining->accept();
    }
    if (auto* remaining = window.findChild<QDialog*>("importPreviewDialog")) remaining->accept();
    flush();
    if (!savedProfile.isEmpty()) {
        profiles->setCurrentIndex(profiles->findData(savedProfile)); flush();
        check(workspace.projection().selectedProfileId == savedProfile, "SELECT_SAVED_PROFILE");
        const auto document = workspace.selectedProfileDocument();
        bool inactive = true;
        for (const auto& value : document.value(QStringLiteral("rules")).toArray()) {
            const auto rule = value.toObject();
            inactive &= !rule.value(QStringLiteral("enabled")).toBool(true)
                && rule.value(QStringLiteral("activation_required")).toBool();
        }
        check(inactive && !document.value(QStringLiteral("rules")).toArray().isEmpty(), "SAVED_RULES_REMAIN_DISABLED");
    }

    workspace.discardReviewDraft();
    check(workspace.selectBuiltInFixture() && workspace.start() && workspace.step() && workspace.step(), "RECOVERY_SETUP_COMMITTED_SENT");
    const QString recoveryRun = workspace.projection().selectedRunId;
    const QString rootPath = workspace.projection().rootPath;
    check(workspace.close() && workspace.open(rootPath), "CLOSE_REOPEN_REAL_WORKSPACE");
    check(workspace.selectRun(recoveryRun), "SELECT_RECOVERED_RUN");
    flush();
    const auto recovered = workspace.projection();
    check(!recovered.active && recovered.historyReadOnly && recovered.unknownCount == 1
              && recovered.reservationCount == 1 && recovered.confirmedSuccess == 0, "RESTART_PRESERVES_UNKNOWN_NO_RESUME");
    auto* recovery = window.findChild<QLabel*>("workspaceRecoveryLabel");
    check(recovery && !recovery->text().isEmpty(), "RECOVERY_NOTICE_VISIBLE");
    bool profileSurvives = false;
    for (const auto& profile : recovered.profiles) profileSurvives |= profile.id == savedProfile;
    check(profileSurvives, "REVIEWED_PROFILE_SURVIVES_RESTART");
    auto* runCombo = window.findChild<QComboBox*>("workspaceRunCombo");
    check(runCombo && runCombo->count() >= 2, "HISTORY_SELECTOR_POPULATED");
    window.setPage(7); flush();
    check(window.grab().save(outputDirectory + QStringLiteral("/recovery.png")), "RECOVERY_SNAPSHOT");
    if (runCombo) { runCombo->setCurrentIndex(runCombo->findData(completedRun)); flush(); }
    check(workspace.projection().selectedRunId == completedRun && workspace.projection().confirmedSuccess == 1, "HISTORY_SELECTION_RESTORES_COMMITTED_VIEW");
    // Verify both close outcomes using a hidden second window and a separate
    // configuration. The main test window and its workspace remain open.
    {
        AppState closeState;
        closeState.configPath = outputDirectory + QStringLiteral("/close-config.json");
        QString saveError;
        check(closeState.saveTo(closeState.configPath, &saveError), "CLOSE_TEST_BASELINE_SAVED");
        QFile beforeFile(closeState.configPath);
        beforeFile.open(QIODevice::ReadOnly);
        const QByteArray originalBytes = beforeFile.readAll();
        beforeFile.close();
        MainWindow closeWindow(&closeState, nullptr, &workspace);
        closeState.run.purchaseDelayMs += 1;
        QCloseEvent cancelled;
        check(answerPrompt("workspaceCloseDirtyDialog", QMessageBox::RejectRole, [&] {
            QApplication::sendEvent(&closeWindow, &cancelled);
        }) && !cancelled.isAccepted() && !closeWindow.property("skipAutomaticConfigSave").toBool(),
            "CLOSE_CANCEL_KEEPS_WINDOW_AND_SAVE_POLICY");
        QCloseEvent discarded;
        check(answerPrompt("workspaceCloseDirtyDialog", QMessageBox::DestructiveRole, [&] {
            QApplication::sendEvent(&closeWindow, &discarded);
        }) && discarded.isAccepted() && closeWindow.property("skipAutomaticConfigSave").toBool(),
            "CLOSE_DISCARD_SKIPS_EXIT_AUTOSAVE");
        QFile afterFile(closeState.configPath);
        check(afterFile.open(QIODevice::ReadOnly) && afterFile.readAll() == originalBytes,
            "CLOSE_DISCARD_PRESERVES_ORIGINAL_CONFIG");
    }
    QFile result(outputDirectory + QStringLiteral("/ui_results.json"));
    if (result.open(QIODevice::WriteOnly)) result.write(QJsonDocument(QJsonObject{
        {QStringLiteral("passed"), failures == 0}, {QStringLiteral("checks"), checks},
        {QStringLiteral("game_connected"), false}, {QStringLiteral("system_input_sent"), false},
        {QStringLiteral("image_file_write_count"), workspace.projection().imageFileWriteCount}}).toJson());
    std::printf("WORKSPACE_UI_SELF_TEST=%s; assertions=%lld; failures=%d; persistent_store=true\n", failures == 0 ? "PASS" : "FAIL", static_cast<long long>(checks.size()), failures);
    return failures == 0;
}
}
