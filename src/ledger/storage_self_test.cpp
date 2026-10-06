#include "storage_self_test.h"
#include "sqlite_event_store.h"

#include <QFileInfo>
#include <QSqlDatabase>
#include <QTemporaryDir>
#include <cstdio>

namespace relink::ledger {
int runStorageSelfTest()
{
    int failures = 0, assertions = 0;
    const auto check = [&failures, &assertions](bool passed, const char* name) {
        ++assertions;
        std::printf("STORAGE_%s=%s\n", name, passed ? "PASS" : "FAIL");
        if (!passed) ++failures;
        return passed;
    };
    const auto good = [&check](const auto& result, const char* name) {
        if (const auto* error = std::get_if<StoreError>(&result))
            std::printf("STORAGE_ERROR=%s; message=%s\n", error->code.toUtf8().constData(), error->message.toUtf8().constData());
        return check(!std::holds_alternative<StoreError>(result), name);
    };
    QTemporaryDir temporary;
    if (!check(temporary.isValid(), "TEMP_DIRECTORY")
        || !check(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")), "QSQLITE_DRIVER")) return 1;
    const QString database = temporary.filePath(QStringLiteral("ledger.sqlite"));
    const QString backup = temporary.filePath(QStringLiteral("backup.sqlite"));
    SqliteEventStore store;
    if (!good(store.open(database), "OPEN")) return 1;
    EventDraft event;
    event.eventId = QStringLiteral("selftest-event");
    event.runId = QStringLiteral("selftest-run");
    event.sessionId = QStringLiteral("selftest-session");
    event.clockDomainId = QStringLiteral("selftest-clock");
    event.stepId = QStringLiteral("selftest-step");
    event.type = QStringLiteral("ReservationRequested");
    if (!good(store.appendEvent(event), "EVENT")) return 1;
    AttemptDraft attempt;
    attempt.attemptId = QStringLiteral("selftest-attempt");
    attempt.intentId = QStringLiteral("selftest-intent");
    attempt.runId = event.runId;
    attempt.ruleTaskId = QStringLiteral("selftest-rule");
    attempt.reservationId = QStringLiteral("selftest-reservation");
    attempt.scopeId = QStringLiteral("selftest-scope");
    attempt.quotaTarget = 1;
    if (!good(store.reserveAttempt(attempt), "RESERVATION")) return 1;
    if (!good(store.commit(), "COMMIT")) return 1;
    if (!good(store.recordDispatch({attempt.attemptId, QStringLiteral("selftest-dispatch"), QStringLiteral("Sent"), QStringLiteral("acknowledged")}), "DISPATCH_FACT")
        || !good(store.commit(), "DISPATCH_COMMIT")) return 1;
    if (!good(store.close(), "CLOSE") || !good(store.open(database), "REOPEN")) return 1;
    auto snapshot = store.snapshot(event.runId);
    const auto* view = std::get_if<LedgerSnapshot>(&snapshot);
    check(view && view->attempts.size() == 1 && view->attempts.first().state == QStringLiteral("Unknown")
          && view->confirmedSuccess == 0 && view->unresolvedReservations == 1, "RECOVERY_PRESERVES_UNKNOWN");
    if (!good(store.backupTo(backup), "BACKUP")) return 1;
    check(QFileInfo(backup).size() > 0, "BACKUP_EXISTS");
    if (!good(store.close(), "CLOSE_FOR_BACKUP") || !good(store.open(backup), "BACKUP_OPEN")) return 1;
    snapshot = store.snapshot(event.runId);
    view = std::get_if<LedgerSnapshot>(&snapshot);
    check(view && view->attempts.size() == 1 && view->unresolvedReservations == 1, "BACKUP_RECOVERY");
    if (!good(store.applyReceipt({attempt.attemptId, QStringLiteral("selftest-receipt"), QStringLiteral("success"), QStringLiteral("confirmed"), QStringLiteral("selftest-receipt-event")}), "LATE_CONFIRMED_RECEIPT")
        || !good(store.commitLedger({QStringLiteral("selftest-transaction"), attempt.attemptId, QStringLiteral("Success"), false, QStringLiteral("selftest-receipt"), QStringLiteral("selftest-ledger-event")}), "LEDGER_STAGE")) return 1;
    snapshot = store.snapshot(event.runId);
    view = std::get_if<LedgerSnapshot>(&snapshot);
    check(view && view->confirmedSuccess == 0 && view->unresolvedReservations == 1, "NO_SUCCESS_BEFORE_COMMIT");
    if (!good(store.commit(), "LEDGER_COMMIT")) return 1;
    snapshot = store.snapshot(event.runId);
    view = std::get_if<LedgerSnapshot>(&snapshot);
    check(view && view->confirmedSuccess == 1 && view->unresolvedReservations == 0, "COMMITTED_SUCCESS");
    good(store.close(), "FINAL_CLOSE");
    std::printf("STORAGE_SELF_TEST=%s; driver=QSQLITE; temporary_data=true; system_input_sent=false; assertions=%d\n", failures == 0 ? "PASS" : "FAIL", assertions);
    return failures == 0 ? 0 : 1;
}
}
