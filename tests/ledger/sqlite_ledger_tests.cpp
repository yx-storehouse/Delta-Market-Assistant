#include "ledger/in_memory_event_store.h"
#include "ledger/sqlite_event_store.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include <cstdlib>
#include <iostream>
#include <variant>

using namespace relink::ledger;

namespace {
int assertions = 0;
int failures = 0;

void check(bool passed, const QString& name)
{
    ++assertions;
    if (!passed) ++failures;
    std::cout << (passed ? "PASS " : "FAIL ") << name.toStdString() << '\n';
}

template<class T> bool ok(const StoreResult<T>& result)
{
    return std::holds_alternative<T>(result);
}

template<class T> const T* value(const StoreResult<T>& result)
{
    return std::get_if<T>(&result);
}

template<class T> QString errorCode(const StoreResult<T>& result)
{
    const auto* error = std::get_if<StoreError>(&result);
    return error ? error->code : QString();
}

template<class T> bool expectOk(const StoreResult<T>& result, const QString& name)
{
    check(ok(result), name);
    if (const auto* error = std::get_if<StoreError>(&result)) {
        std::cout << "  error=" << error->code.toStdString()
                  << "; message=" << error->message.toStdString() << '\n';
    }
    return ok(result);
}

template<class T> void expectMutationError(SqliteEventStore& store,
    const StoreResult<T>& result, const QString& name, const QString& expectedCode = {})
{
    check(!errorCode(result).isEmpty()
              && (expectedCode.isEmpty() || errorCode(result) == expectedCode),
          name + QStringLiteral("_rejected") + (expectedCode.isEmpty() ? QString() : QStringLiteral("_") + expectedCode));
    if (ok(result) || (!expectedCode.isEmpty() && errorCode(result) != expectedCode))
        std::cout << "  observed_error=" << errorCode(result).toStdString() << '\n';
    const auto commit = store.commit();
    check(errorCode(commit) == QStringLiteral("TRANSACTION_ABORTED"), name + QStringLiteral("_commit_aborted"));
    expectOk(store.rollback(), name + QStringLiteral("_rollback_clears_abort"));
}

SqliteEventStore::OpenOptions noRecovery(bool readOnly = false)
{
    SqliteEventStore::OpenOptions options;
    options.readOnly = readOnly;
    options.busyTimeoutMs = 40;
    options.recoverOnOpen = false;
    return options;
}

EventDraft event(const QString& suffix = QStringLiteral("one"), qint64 seq = 0)
{
    EventDraft out;
    out.eventId = QStringLiteral("event-") + suffix;
    out.runId = QStringLiteral("run-1");
    out.sessionId = QStringLiteral("session-1");
    out.clockDomainId = QStringLiteral("clock-1");
    out.stepId = QStringLiteral("step-1");
    out.seq = seq;
    out.atMonoMs = 10;
    out.type = QStringLiteral("AttemptPrepared");
    out.payload = {{QStringLiteral("a"), 1},
                   {QStringLiteral("nested"), QJsonObject{{QStringLiteral("b"), 2}, {QStringLiteral("a"), 1}}},
                   {QStringLiteral("array"), QJsonArray{1, 2}}};
    return out;
}

AttemptDraft attempt(const QString& suffix = QStringLiteral("one"),
                     const QString& scope = QStringLiteral("scope-1"), int quota = 1)
{
    AttemptDraft out;
    out.attemptId = QStringLiteral("attempt-") + suffix;
    out.intentId = QStringLiteral("intent-") + suffix;
    out.runId = QStringLiteral("run-1");
    out.ruleTaskId = QStringLiteral("rule-1");
    out.reservationId = QStringLiteral("reservation-") + suffix;
    out.scopeId = scope;
    out.quotaTarget = quota;
    return out;
}

DispatchDraft dispatch(const QString& suffix = QStringLiteral("one"))
{
    return {QStringLiteral("attempt-") + suffix, QStringLiteral("dispatch-") + suffix,
            QStringLiteral("Sent"), QStringLiteral("acknowledged")};
}

ReceiptDraft receipt(const QString& suffix = QStringLiteral("one"),
                     const QString& outcome = QStringLiteral("success"),
                     const QString& association = QStringLiteral("confirmed"))
{
    return {QStringLiteral("attempt-") + suffix, QStringLiteral("receipt-") + suffix,
            outcome, association, QStringLiteral("receipt-event-") + suffix};
}

LedgerDraft transaction(const QString& suffix = QStringLiteral("one"),
                        const QString& terminal = QStringLiteral("Success"))
{
    return {QStringLiteral("transaction-") + suffix, QStringLiteral("attempt-") + suffix,
            terminal, false, QStringLiteral("receipt-") + suffix,
            QStringLiteral("ledger-event-") + suffix};
}

const AttemptRecord* findAttempt(const LedgerSnapshot& snapshot, const QString& id)
{
    for (const auto& item : snapshot.attempts)
        if (item.attemptId == id) return &item;
    return nullptr;
}

const QuotaReservationRecord* findReservation(const LedgerSnapshot& snapshot, const QString& id)
{
    for (const auto& item : snapshot.reservations)
        if (item.reservationId == id) return &item;
    return nullptr;
}

void expectSnapshot(SqliteEventStore& store, int successes, int unresolved,
                    int count, const QString& name)
{
    const auto result = store.snapshot(QStringLiteral("run-1"));
    const auto* snapshot = value(result);
    check(snapshot && snapshot->confirmedSuccess == successes
              && snapshot->unresolvedReservations == unresolved
              && snapshot->attempts.size() == count, name);
}

QByteArray digest(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

// Every raw SQL connection belongs solely to this temporary test database.
// Destruction drops all QSqlQuery handles before removing the connection.
class RawConnection {
public:
    explicit RawConnection(const QString& path)
        : name(QStringLiteral("sqlite-test-") + QUuid::createUuid().toString(QUuid::WithoutBraces)),
          database(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name))
    {
        database.setDatabaseName(path);
        database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=40"));
        opened = database.open();
    }

    ~RawConnection()
    {
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }

    bool exec(const QString& sql)
    {
        QSqlQuery query(database);
        const bool result = query.exec(sql);
        if (!result) std::cout << "  fixture_sql_error=" << query.lastError().text().toStdString() << '\n';
        return result;
    }

    qint64 scalar(const QString& sql)
    {
        QSqlQuery query(database);
        if (!query.exec(sql) || !query.next()) return -1;
        return query.value(0).toLongLong();
    }

    QString name;
    QSqlDatabase database;
    bool opened = false;
};

bool seed(SqliteEventStore& store, const QString& suffix = QStringLiteral("one"),
          const QString& scope = QStringLiteral("scope-1"), qint64 seq = 0)
{
    return expectOk(store.appendEvent(event(suffix, seq)), QStringLiteral("seed_event_") + suffix)
        && expectOk(store.reserveAttempt(attempt(suffix, scope)), QStringLiteral("seed_attempt_") + suffix)
        && expectOk(store.commit(), QStringLiteral("seed_commit_") + suffix);
}

void testIdentity(const QString& path)
{
    SqliteEventStore store;
    if (!expectOk(store.open(path, noRecovery()), QStringLiteral("identity_open"))) return;
    check(store.isOpen() && store.schemaVersion() == 2, QStringLiteral("identity_schema_v2"));
    const EventDraft original = event();
    const auto first = store.appendEvent(original);
    check(value(first) && !value(first)->duplicate, QStringLiteral("event_first_is_new"));
    expectOk(store.commit(), QStringLiteral("event_first_commit"));
    EventDraft reordered = original;
    reordered.payload = {{QStringLiteral("array"), QJsonArray{1, 2}},
                         {QStringLiteral("nested"), QJsonObject{{QStringLiteral("a"), 1}, {QStringLiteral("b"), 2}}},
                         {QStringLiteral("a"), 1}};
    const auto duplicate = store.appendEvent(reordered);
    check(value(duplicate) && value(duplicate)->duplicate, QStringLiteral("event_canonical_payload_idempotent"));
    expectOk(store.commit(), QStringLiteral("event_duplicate_commit"));
    auto changed = original;
    changed.payload.insert(QStringLiteral("a"), 9);
    expectMutationError(store, store.appendEvent(changed), QStringLiteral("event_payload_conflict"), QStringLiteral("EVENT_ID_CONFLICT"));
    changed = original;
    changed.atMonoMs += 1;
    expectMutationError(store, store.appendEvent(changed), QStringLiteral("event_metadata_conflict"), QStringLiteral("EVENT_ID_CONFLICT"));
    changed = original;
    changed.eventId = QStringLiteral("other-event-at-zero");
    expectMutationError(store, store.appendEvent(changed), QStringLiteral("event_sequence_zero_conflict"), QStringLiteral("EVENT_SEQ_CONFLICT"));
    const auto count = store.eventCount();
    check(value(count) && *value(count) == 1, QStringLiteral("event_count_after_conflicts_is_one"));

    const auto reserved = store.reserveAttempt(attempt());
    check(value(reserved) && value(reserved)->state == QStringLiteral("Prepared") && value(reserved)->reservationHeld,
          QStringLiteral("attempt_reserve_prepared"));
    expectOk(store.commit(), QStringLiteral("attempt_reserve_commit"));
    const auto again = store.reserveAttempt(attempt());
    check(value(again) && value(again)->duplicate, QStringLiteral("attempt_idempotent"));
    expectOk(store.commit(), QStringLiteral("attempt_duplicate_commit"));
    AttemptDraft altered = attempt();
    altered.quantity = 2;
    expectMutationError(store, store.reserveAttempt(altered), QStringLiteral("attempt_payload_conflict"), QStringLiteral("ATTEMPT_ID_CONFLICT"));
    altered = attempt(QStringLiteral("other"), QStringLiteral("other-scope"));
    altered.intentId = QStringLiteral("intent-one");
    expectMutationError(store, store.reserveAttempt(altered), QStringLiteral("intent_conflict"), QStringLiteral("INTENT_ID_CONFLICT"));
    altered.intentId = QStringLiteral("intent-other");
    altered.reservationId = QStringLiteral("reservation-one");
    expectMutationError(store, store.reserveAttempt(altered), QStringLiteral("reservation_conflict"), QStringLiteral("RESERVATION_CONFLICT"));
    expectMutationError(store, store.reserveAttempt(attempt(QStringLiteral("quota"))), QStringLiteral("quota_full"), QStringLiteral("QUOTA_FULL"));
    altered = attempt(QStringLiteral("no-quota"), QStringLiteral("other-scope"), 0);
    expectMutationError(store, store.reserveAttempt(altered), QStringLiteral("quota_required"), QStringLiteral("QUOTA_SCOPE_UNRESOLVED"));
    expectOk(store.appendEvent(event(QStringLiteral("must-rollback"), 1)), QStringLiteral("conflict_stage_prior_event"));
    expectOk(store.reserveAttempt(attempt(QStringLiteral("must-rollback"), QStringLiteral("rollback-scope"))), QStringLiteral("conflict_stage_prior_attempt"));
    changed = original;
    changed.payload.insert(QStringLiteral("changed"), true);
    expectMutationError(store, store.appendEvent(changed), QStringLiteral("conflict_aborts_entire_unit_of_work"), QStringLiteral("EVENT_ID_CONFLICT"));
    const auto atomicCount = store.eventCount();
    check(value(atomicCount) && *value(atomicCount) == 1, QStringLiteral("conflict_rolls_back_prior_event_in_same_transaction"));
    expectSnapshot(store, 0, 1, 1, QStringLiteral("identity_errors_preserve_committed_state"));
    expectOk(store.close(), QStringLiteral("identity_close"));
}

void testSuccessFailureAndVisibility(const QString& path)
{
    SqliteEventStore store;
    if (!expectOk(store.open(path, noRecovery()), QStringLiteral("success_open")) || !seed(store)) return;
    expectOk(store.recordDispatch(dispatch()), QStringLiteral("success_dispatch"));
    expectOk(store.commit(), QStringLiteral("success_dispatch_commit"));
    auto repeatedDispatch = dispatch();
    repeatedDispatch.eventId = QStringLiteral("dispatch-observed-again");
    const auto dispatchAgain = store.recordDispatch(repeatedDispatch);
    check(value(dispatchAgain) && value(dispatchAgain)->duplicate, QStringLiteral("same_dispatch_new_event_id_is_idempotent"));
    expectOk(store.commit(), QStringLiteral("dispatch_new_event_id_commit"));
    expectOk(store.applyReceipt(receipt()), QStringLiteral("success_receipt"));
    expectOk(store.commitLedger(transaction()), QStringLiteral("success_ledger_staged"));
    expectSnapshot(store, 0, 1, 1, QStringLiteral("ui_snapshot_before_commit_has_no_success"));
    {
        SqliteEventStore observer;
        if (expectOk(observer.open(path, noRecovery(true)), QStringLiteral("observer_readonly_open")))
            expectSnapshot(observer, 0, 1, 1, QStringLiteral("other_connection_before_commit_has_no_success"));
    }
    expectOk(store.commit(), QStringLiteral("success_atomic_commit"));
    expectSnapshot(store, 1, 0, 1, QStringLiteral("success_visible_only_after_commit"));
    auto repeatedReceipt = receipt();
    repeatedReceipt.eventId = QStringLiteral("same-receipt-observed-again");
    auto duplicateReceipt = store.applyReceipt(repeatedReceipt);
    check(value(duplicateReceipt) && value(duplicateReceipt)->duplicate, QStringLiteral("successful_receipt_new_event_id_is_idempotent"));
    auto repeatedTransaction = transaction();
    repeatedTransaction.eventId = QStringLiteral("same-ledger-replayed-again");
    expectOk(store.commitLedger(repeatedTransaction), QStringLiteral("successful_ledger_new_event_id_is_idempotent"));
    expectOk(store.commit(), QStringLiteral("success_replay_commit"));
    expectSnapshot(store, 1, 0, 1, QStringLiteral("success_replay_not_double_counted"));
    auto conflict = transaction();
    conflict.terminal = QStringLiteral("Failed");
    expectMutationError(store, store.commitLedger(conflict), QStringLiteral("transaction_payload_conflict"), QStringLiteral("TRANSACTION_ID_CONFLICT"));
    auto newReceipt = receipt();
    newReceipt.receiptIdentity = QStringLiteral("late-new-receipt");
    expectMutationError(store, store.applyReceipt(newReceipt), QStringLiteral("terminal_receipt_rejected"), QStringLiteral("ATTEMPT_TERMINAL"));

    if (!seed(store, QStringLiteral("failed"), QStringLiteral("scope-failed"), 1)) return;
    expectOk(store.recordDispatch(dispatch(QStringLiteral("failed"))), QStringLiteral("failed_dispatch"));
    expectOk(store.applyReceipt(receipt(QStringLiteral("failed"), QStringLiteral("failed"))), QStringLiteral("failed_receipt"));
    expectOk(store.commitLedger(transaction(QStringLiteral("failed"), QStringLiteral("Failed"))), QStringLiteral("failed_ledger"));
    expectSnapshot(store, 1, 1, 2, QStringLiteral("failed_before_commit_still_reserved"));
    expectOk(store.commit(), QStringLiteral("failed_commit"));
    expectSnapshot(store, 1, 0, 2, QStringLiteral("explicit_failed_releases_reservation"));
    const auto snapshot = store.snapshot(QStringLiteral("run-1"));
    const auto* reservation = value(snapshot) ? findReservation(*value(snapshot), QStringLiteral("reservation-failed")) : nullptr;
    check(reservation && reservation->state == QStringLiteral("released"), QStringLiteral("failed_reservation_marked_released"));
    expectOk(store.reserveAttempt(attempt(QStringLiteral("retry"), QStringLiteral("scope-failed"))), QStringLiteral("failed_frees_quota_for_new_intent"));
    expectOk(store.rollback(), QStringLiteral("retry_rollback"));
    expectSnapshot(store, 1, 0, 2, QStringLiteral("rollback_discards_new_attempt"));
    expectOk(store.close(), QStringLiteral("success_close"));
    expectOk(store.open(path, noRecovery()), QStringLiteral("success_reopen"));
    expectSnapshot(store, 1, 0, 2, QStringLiteral("success_and_failure_persist_across_reopen"));
    const auto counts = store.eventCount();
    check(value(counts) && *value(counts) == 2, QStringLiteral("events_persist_across_reopen"));

    // Reuse the pure in-memory implementation as an oracle only for reviewed,
    // confirmed success. Its historical unknown->success behavior is not an oracle.
    InMemoryEventStore oracle;
    oracle.appendEvent(event());
    oracle.reserveAttempt(attempt());
    oracle.recordDispatch(dispatch());
    oracle.applyReceipt(receipt());
    oracle.commitLedger(transaction());
    oracle.commit();
    const auto expected = oracle.snapshot(QStringLiteral("run-1"));
    const auto actual = store.snapshot(QStringLiteral("run-1"));
    check(value(expected) && value(actual) && value(expected)->confirmedSuccess == value(actual)->confirmedSuccess,
          QStringLiteral("confirmed_success_matches_memory_oracle"));
}

void testUnknownAndReceiptAssociation(const QString& path)
{
    SqliteEventStore store;
    if (!expectOk(store.open(path, noRecovery()), QStringLiteral("unknown_open")) || !seed(store)) return;
    expectOk(store.recordDispatch(dispatch()), QStringLiteral("unknown_dispatch"));
    const auto unknown = store.applyReceipt(receipt(QStringLiteral("one"), QStringLiteral("unknown")));
    check(value(unknown) && value(unknown)->state == QStringLiteral("Unknown") && value(unknown)->reservationHeld,
          QStringLiteral("unknown_receipt_retains_reservation"));
    expectOk(store.commit(), QStringLiteral("unknown_commit"));
    expectMutationError(store, store.commitLedger(transaction()), QStringLiteral("unknown_receipt_cannot_be_success"), QStringLiteral("RECEIPT_UNCONFIRMED"));
    expectMutationError(store, store.commitLedger(transaction(QStringLiteral("one"), QStringLiteral("Failed"))), QStringLiteral("unknown_receipt_cannot_release_as_failure"), QStringLiteral("RECEIPT_UNCONFIRMED"));
    expectMutationError(store, store.recordDispatch(dispatch()), QStringLiteral("unknown_never_redispatched"), QStringLiteral("ATTEMPT_UNKNOWN"));
    expectMutationError(store, store.reserveAttempt(attempt(QStringLiteral("blocked"))), QStringLiteral("unknown_occupies_quota"), QStringLiteral("QUOTA_FULL"));
    const auto duplicate = store.applyReceipt(receipt(QStringLiteral("one"), QStringLiteral("unknown")));
    check(value(duplicate) && value(duplicate)->duplicate, QStringLiteral("unknown_receipt_duplicate"));
    expectOk(store.commit(), QStringLiteral("unknown_duplicate_commit"));
    expectMutationError(store, store.applyReceipt(receipt()), QStringLiteral("receipt_outcome_conflict"), QStringLiteral("RECEIPT_CONFLICT"));
    expectSnapshot(store, 0, 1, 1, QStringLiteral("unknown_never_increments_success"));

    if (!seed(store, QStringLiteral("ambiguous"), QStringLiteral("scope-ambiguous"), 1)) return;
    expectOk(store.recordDispatch(dispatch(QStringLiteral("ambiguous"))), QStringLiteral("ambiguous_dispatch"));
    const auto ambiguous = store.applyReceipt(receipt(QStringLiteral("ambiguous"), QStringLiteral("success"), QStringLiteral("ambiguous")));
    check(value(ambiguous) && value(ambiguous)->state == QStringLiteral("Unknown"), QStringLiteral("ambiguous_success_becomes_unknown"));
    expectOk(store.commit(), QStringLiteral("ambiguous_commit"));
    expectMutationError(store, store.commitLedger(transaction(QStringLiteral("ambiguous"))), QStringLiteral("ambiguous_cannot_commit_success"), QStringLiteral("RECEIPT_UNCONFIRMED"));
    expectSnapshot(store, 0, 2, 2, QStringLiteral("ambiguous_retains_both_reservations"));

    auto confirmed = receipt(QStringLiteral("ambiguous"));
    confirmed.receiptIdentity = QStringLiteral("receipt-ambiguous-confirmed-later");
    auto confirmedTx = transaction(QStringLiteral("ambiguous"));
    confirmedTx.receiptIdentity = confirmed.receiptIdentity;
    expectOk(store.applyReceipt(confirmed), QStringLiteral("late_distinct_confirmed_receipt_accepted"));
    expectOk(store.commitLedger(confirmedTx), QStringLiteral("late_confirmation_staged"));
    expectSnapshot(store, 0, 2, 2, QStringLiteral("late_confirmation_hidden_until_commit"));
    expectOk(store.commit(), QStringLiteral("late_confirmation_commit"));
    expectSnapshot(store, 1, 1, 2, QStringLiteral("only_explicit_late_confirmation_counts_success"));

    if (!seed(store, QStringLiteral("mismatch"), QStringLiteral("scope-mismatch"), 2)) return;
    expectOk(store.recordDispatch(dispatch(QStringLiteral("mismatch"))), QStringLiteral("mismatch_dispatch"));
    expectOk(store.applyReceipt(receipt(QStringLiteral("mismatch"), QStringLiteral("failed"))), QStringLiteral("mismatch_failed_receipt"));
    expectOk(store.commit(), QStringLiteral("mismatch_receipt_commit"));
    expectMutationError(store, store.commitLedger(transaction(QStringLiteral("mismatch"))), QStringLiteral("failed_receipt_cannot_be_success"), QStringLiteral("RECEIPT_OUTCOME_MISMATCH"));
    expectSnapshot(store, 1, 2, 3, QStringLiteral("mismatched_outcome_does_not_corrupt_ledger"));
}

void testRollbackAndRecovery(const QString& path)
{
    SqliteEventStore store;
    if (!expectOk(store.open(path, noRecovery()), QStringLiteral("recovery_open"))) return;
    expectOk(store.appendEvent(event(QStringLiteral("rolled-back"))), QStringLiteral("rollback_event_stage"));
    expectOk(store.reserveAttempt(attempt(QStringLiteral("rolled-back"))), QStringLiteral("rollback_attempt_stage"));
    expectSnapshot(store, 0, 0, 0, QStringLiteral("uncommitted_attempt_not_projected"));
    expectOk(store.rollback(), QStringLiteral("explicit_rollback"));
    const auto emptyCount = store.eventCount();
    check(value(emptyCount) && *value(emptyCount) == 0, QStringLiteral("rollback_removes_event_and_attempt_atomically"));
    if (!seed(store, QStringLiteral("prepared"), QStringLiteral("scope-prepared"), 0)
        || !seed(store, QStringLiteral("sent"), QStringLiteral("scope-sent"), 1)
        || !seed(store, QStringLiteral("dispatching"), QStringLiteral("scope-dispatching"), 2)
        || !seed(store, QStringLiteral("awaiting"), QStringLiteral("scope-awaiting"), 3)) return;
    expectOk(store.recordDispatch(dispatch(QStringLiteral("sent"))), QStringLiteral("recovery_sent_stage"));
    auto dispatching = dispatch(QStringLiteral("dispatching"));
    dispatching.state = QStringLiteral("Dispatching");
    dispatching.dispatchProof = QStringLiteral("possibly_dispatched");
    expectOk(store.recordDispatch(dispatching), QStringLiteral("recovery_dispatching_stage"));
    auto awaiting = dispatch(QStringLiteral("awaiting"));
    awaiting.state = QStringLiteral("AwaitingReceipt");
    expectOk(store.recordDispatch(awaiting), QStringLiteral("recovery_awaiting_stage"));
    expectOk(store.commit(), QStringLiteral("recovery_stages_commit"));
    expectOk(store.reserveAttempt(attempt(QStringLiteral("close-rollback"), QStringLiteral("scope-close"))), QStringLiteral("close_uncommitted_attempt"));
    expectOk(store.close(), QStringLiteral("close_rolls_back_pending_transaction"));
    expectOk(store.open(path), QStringLiteral("recovery_default_open"));
    expectSnapshot(store, 0, 3, 4, QStringLiteral("restart_preserves_only_possibly_sent_reservations"));
    const auto snap = store.snapshot(QStringLiteral("run-1"));
    const auto* prepared = value(snap) ? findAttempt(*value(snap), QStringLiteral("attempt-prepared")) : nullptr;
    const auto* preparedQuota = value(snap) ? findReservation(*value(snap), QStringLiteral("reservation-prepared")) : nullptr;
    check(prepared && prepared->state == QStringLiteral("Cancelled") && !prepared->reservationHeld
              && preparedQuota && preparedQuota->state == QStringLiteral("released"),
          QStringLiteral("restart_prepared_not_dispatched_is_cancelled"));
    for (const QString& suffix : {QStringLiteral("sent"), QStringLiteral("dispatching"), QStringLiteral("awaiting")}) {
        const auto* record = value(snap) ? findAttempt(*value(snap), QStringLiteral("attempt-") + suffix) : nullptr;
        const auto* quota = value(snap) ? findReservation(*value(snap), QStringLiteral("reservation-") + suffix) : nullptr;
        check(record && record->state == QStringLiteral("Unknown") && record->reservationHeld
                  && quota && quota->state == QStringLiteral("unresolved"), QStringLiteral("restart_unknown_") + suffix);
        expectMutationError(store, store.recordDispatch(dispatch(suffix)), QStringLiteral("restart_blocks_redispatch_") + suffix,
                            QStringLiteral("ATTEMPT_UNKNOWN"));
    }
    const auto recovered = store.recoverUnresolved(QStringLiteral("session-1"));
    check(value(recovered) && value(recovered)->attempts.size() == 3, QStringLiteral("recovery_set_contains_only_unresolved_session_attempts"));
    const auto otherSession = store.recoverUnresolved(QStringLiteral("other-session"));
    check(value(otherSession) && value(otherSession)->attempts.isEmpty(), QStringLiteral("recovery_set_is_session_scoped"));
    expectOk(store.close(), QStringLiteral("recovery_second_close"));
    expectOk(store.open(path), QStringLiteral("recovery_second_open"));
    expectSnapshot(store, 0, 3, 4, QStringLiteral("recovery_is_idempotent_across_restarts"));
}

void testReadOnlyAndBusy(const QString& path)
{
    {
        SqliteEventStore seedStore;
        if (!expectOk(seedStore.open(path, noRecovery()), QStringLiteral("readonly_seed_open")) || !seed(seedStore)) return;
    }
    const QByteArray before = digest(path);
    {
        SqliteEventStore reader;
        if (!expectOk(reader.open(path, noRecovery(true)), QStringLiteral("readonly_open"))) return;
        expectSnapshot(reader, 0, 1, 1, QStringLiteral("readonly_does_not_apply_recovery"));
        expectMutationError(reader, reader.appendEvent(event(QStringLiteral("readonly"), 1)), QStringLiteral("readonly_write"), QStringLiteral("DB_READ_ONLY"));
        expectSnapshot(reader, 0, 1, 1, QStringLiteral("readonly_failure_keeps_old_snapshot"));
    }
    check(!before.isEmpty() && before == digest(path), QStringLiteral("readonly_preserves_source_bytes"));

    SqliteEventStore writer;
    if (!expectOk(writer.open(path, noRecovery()), QStringLiteral("busy_writer_open"))) return;
    {
        SqliteEventStore competingWriter;
        const auto result = competingWriter.open(path);
        check(errorCode(result) == QStringLiteral("STORE_IN_USE") && !competingWriter.isOpen(),
              QStringLiteral("second_writable_instance_cannot_recover_live_attempts"));
        expectSnapshot(writer, 0, 1, 1, QStringLiteral("second_writer_rejection_preserves_first_writer_state"));
        SqliteEventStore observer;
        if (expectOk(observer.open(path, noRecovery(true)), QStringLiteral("readonly_observer_can_coexist_with_writer"))) {
            expectSnapshot(observer, 0, 1, 1, QStringLiteral("coexisting_readonly_observer_sees_committed_snapshot"));
            const auto snapshot = observer.snapshot(QStringLiteral("run-1"));
            const auto* record = value(snapshot) ? findAttempt(*value(snapshot), QStringLiteral("attempt-one")) : nullptr;
            check(record && record->state == QStringLiteral("Prepared"), QStringLiteral("coexisting_readonly_open_does_not_cancel_live_prepared"));
        }
    }
    {
        RawConnection lock(path);
        if (!lock.opened || !lock.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
            check(false, QStringLiteral("busy_fixture_lock"));
            return;
        }
        check(true, QStringLiteral("busy_fixture_real_write_lock"));
        expectMutationError(writer, writer.appendEvent(event(QStringLiteral("busy"), 1)), QStringLiteral("busy_real_lock"), QStringLiteral("DB_BUSY"));
        expectSnapshot(writer, 0, 1, 1, QStringLiteral("busy_error_keeps_previous_snapshot"));
        check(lock.exec(QStringLiteral("ROLLBACK")), QStringLiteral("busy_fixture_unlock"));
    }
    expectOk(writer.appendEvent(event(QStringLiteral("busy-retry"), 1)), QStringLiteral("busy_retry_after_unlock"));
    expectOk(writer.commit(), QStringLiteral("busy_retry_commit"));

    // SQLite's DELETE journal permits BEGIN IMMEDIATE while another reader has
    // a SHARED lock, then makes COMMIT fail with SQLITE_BUSY. No fake failure hook.
    {
        RawConnection reader(path);
        if (!reader.opened || !reader.exec(QStringLiteral("BEGIN"))
            || reader.scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_master")) < 1) {
            check(false, QStringLiteral("commit_failure_fixture_read_lock"));
            return;
        }
        check(true, QStringLiteral("commit_failure_fixture_real_read_lock"));
        expectOk(writer.recordDispatch(dispatch()), QStringLiteral("commit_failure_dispatch_staged"));
        expectOk(writer.applyReceipt(receipt()), QStringLiteral("commit_failure_receipt_staged"));
        expectOk(writer.commitLedger(transaction()), QStringLiteral("commit_failure_success_staged"));
        const auto commitResult = writer.commit();
        check(errorCode(commitResult) == QStringLiteral("DB_BUSY"), QStringLiteral("real_commit_failure_returned_DB_BUSY"));
        expectOk(writer.rollback(), QStringLiteral("commit_failure_explicit_rollback"));
        expectSnapshot(writer, 0, 1, 1, QStringLiteral("failed_commit_never_publishes_success"));
        check(reader.exec(QStringLiteral("ROLLBACK")), QStringLiteral("commit_failure_reader_unlock"));
    }
    expectOk(writer.close(), QStringLiteral("commit_failure_close"));
    expectOk(writer.open(path, noRecovery()), QStringLiteral("commit_failure_reopen"));
    expectSnapshot(writer, 0, 1, 1, QStringLiteral("failed_commit_has_no_partial_success_after_reopen"));
}

void testFullDatabase(const QString& path)
{
    SqliteEventStore store;
    if (!expectOk(store.open(path, noRecovery()), QStringLiteral("full_open"))) return;
    // max_page_count is connection-local. Locate the store-owned writable Qt
    // connection to this unique temporary database and cap pages at its current
    // size. This produces a real SQLITE_FULL result without filling any disk.
    QString writerName;
    for (const QString& name : QSqlDatabase::connectionNames()) {
        const auto database = QSqlDatabase::database(name, false);
        if (database.isOpen() && QFileInfo(database.databaseName()).absoluteFilePath() == QFileInfo(path).absoluteFilePath()
            && !database.connectOptions().contains(QStringLiteral("QSQLITE_OPEN_READONLY"))) {
            writerName = name;
            break;
        }
    }
    if (writerName.isEmpty()) {
        check(false, QStringLiteral("full_find_temporary_writer_connection"));
        return;
    }
    bool capped = false;
    {
        QSqlQuery query(QSqlDatabase::database(writerName, false));
        if (query.exec(QStringLiteral("PRAGMA page_count")) && query.next()) {
            const qint64 pages = query.value(0).toLongLong();
            capped = pages > 0 && query.exec(QStringLiteral("PRAGMA max_page_count=%1").arg(pages))
                && query.next() && query.value(0).toLongLong() == pages;
        }
    }
    check(capped, QStringLiteral("full_set_real_sqlite_page_limit"));
    if (!capped) return;
    auto huge = event(QStringLiteral("full"));
    huge.payload.insert(QStringLiteral("synthetic_blob"), QString(2 * 1024 * 1024, QLatin1Char('x')));
    const auto result = store.appendEvent(huge);
    expectMutationError(store, result, QStringLiteral("full_real_sqlite_limit"), QStringLiteral("DB_FULL"));
    const auto count = store.eventCount();
    check(value(count) && *value(count) == 0, QStringLiteral("full_failure_leaves_no_partial_event"));
    expectSnapshot(store, 0, 0, 0, QStringLiteral("full_failure_keeps_empty_snapshot"));
    expectOk(store.close(), QStringLiteral("full_close"));
    expectOk(store.open(path, noRecovery()), QStringLiteral("full_reopen"));
    expectOk(store.appendEvent(event(QStringLiteral("after-full"))), QStringLiteral("full_recovery_accepts_small_event"));
    expectOk(store.commit(), QStringLiteral("full_recovery_commit"));
}

void testSchemaAndCorruption(const QString& directory)
{
    const QString path = directory + QStringLiteral("/migration.sqlite");
    {
        SqliteEventStore store;
        if (!expectOk(store.open(path, noRecovery()), QStringLiteral("migration_seed_open")) || !seed(store)) return;
    }
    {
        RawConnection raw(path);
        check(raw.opened && raw.exec(QStringLiteral("DROP TABLE recovery_audit"))
                  && raw.exec(QStringLiteral("PRAGMA user_version=1")), QStringLiteral("migration_create_v1_fixture"));
    }
    const QByteArray v1Hash = digest(path);
    {
        SqliteEventStore readOnly;
        const auto result = readOnly.open(path, noRecovery(true));
        check(errorCode(result) == QStringLiteral("SCHEMA_MIGRATION_REQUIRED") && !readOnly.isOpen(),
              QStringLiteral("readonly_v1_does_not_implicitly_migrate"));
    }
    check(!v1Hash.isEmpty() && digest(path) == v1Hash, QStringLiteral("readonly_v1_preserves_source_bytes"));
    {
        SqliteEventStore store;
        expectOk(store.open(path, noRecovery()), QStringLiteral("migration_v1_to_v2_open"));
        check(store.schemaVersion() == 2, QStringLiteral("migration_version_is_two"));
        expectSnapshot(store, 0, 1, 1, QStringLiteral("migration_preserves_business_records"));
        const auto count = store.eventCount();
        check(value(count) && *value(count) == 1, QStringLiteral("migration_preserves_events"));
    }
    {
        RawConnection raw(path);
        check(raw.scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='recovery_audit'")) == 1,
              QStringLiteral("migration_creates_recovery_audit"));
        check(raw.exec(QStringLiteral("PRAGMA user_version=999")), QStringLiteral("future_schema_fixture"));
    }
    const QByteArray futureHash = digest(path);
    {
        SqliteEventStore store;
        const auto result = store.open(path);
        check(errorCode(result) == QStringLiteral("SCHEMA_TOO_NEW") && !store.isOpen(), QStringLiteral("future_schema_open_rejected"));
    }
    check(!futureHash.isEmpty() && digest(path) == futureHash, QStringLiteral("future_schema_source_bytes_preserved"));

    const QString corruptPath = directory + QStringLiteral("/corrupt.sqlite");
    {
        QFile corrupt(corruptPath);
        const QByteArray bytes = QByteArrayLiteral("This is deliberately not a SQLite database.\0retain-original");
        check(corrupt.open(QIODevice::WriteOnly) && corrupt.write(bytes) == bytes.size(), QStringLiteral("corrupt_fixture_written"));
    }
    const QByteArray corruptHash = digest(corruptPath);
    {
        SqliteEventStore store;
        const auto result = store.open(corruptPath);
        check(errorCode(result) == QStringLiteral("DB_CORRUPT") && !store.isOpen(), QStringLiteral("corrupt_open_rejected_without_rebuild"));
    }
    check(!corruptHash.isEmpty() && digest(corruptPath) == corruptHash, QStringLiteral("corrupt_source_bytes_preserved"));

    const QString missingIndexPath = directory + QStringLiteral("/missing-index.sqlite");
    {
        SqliteEventStore store;
        if (!expectOk(store.open(missingIndexPath, noRecovery()), QStringLiteral("missing_index_seed_open")) || !seed(store)) return;
    }
    {
        RawConnection raw(missingIndexPath);
        check(raw.opened && raw.exec(QStringLiteral("DROP INDEX events_run_seq")), QStringLiteral("missing_index_fixture_created"));
    }
    const QByteArray missingIndexHash = digest(missingIndexPath);
    {
        SqliteEventStore store;
        const auto result = store.open(missingIndexPath);
        check(errorCode(result) == QStringLiteral("SCHEMA_INVALID") && !store.isOpen(),
              QStringLiteral("missing_required_index_rejected_not_silently_recreated"));
    }
    check(!missingIndexHash.isEmpty() && digest(missingIndexPath) == missingIndexHash,
          QStringLiteral("missing_required_index_source_bytes_preserved"));

    const QString inconsistentPath = directory + QStringLiteral("/inconsistent-ledger.sqlite");
    {
        SqliteEventStore store;
        if (!expectOk(store.open(inconsistentPath, noRecovery()), QStringLiteral("inconsistent_ledger_seed_open")) || !seed(store)) return;
    }
    {
        RawConnection raw(inconsistentPath);
        check(raw.opened && raw.exec(QStringLiteral(
                  "UPDATE attempts SET state='Success',reservation_held=0,transaction_id='missing-ledger-transaction' "
                  "WHERE attempt_id='attempt-one'")), QStringLiteral("inconsistent_ledger_fixture_created"));
    }
    const QByteArray inconsistentHash = digest(inconsistentPath);
    {
        SqliteEventStore store;
        const auto result = store.open(inconsistentPath);
        check(errorCode(result) == QStringLiteral("DB_CORRUPT") && !store.isOpen(),
              QStringLiteral("success_without_ledger_fact_is_rejected"));
    }
    check(!inconsistentHash.isEmpty() && digest(inconsistentPath) == inconsistentHash,
          QStringLiteral("inconsistent_ledger_source_bytes_preserved"));
}

void testConsistentBackup(const QString& directory)
{
    const QString source = directory + QStringLiteral("/backup-source.sqlite");
    const QString backup = directory + QStringLiteral("/backup.sqlite");
    SqliteEventStore store;
    if (!expectOk(store.open(source, noRecovery()), QStringLiteral("backup_source_open")) || !seed(store)) return;
    expectOk(store.recordDispatch(dispatch()), QStringLiteral("backup_unknown_dispatch"));
    expectOk(store.applyReceipt(receipt(QStringLiteral("one"), QStringLiteral("unknown"))), QStringLiteral("backup_unknown_receipt"));
    expectOk(store.commit(), QStringLiteral("backup_unknown_commit"));
    if (!seed(store, QStringLiteral("success"), QStringLiteral("scope-success"), 1)) return;
    expectOk(store.recordDispatch(dispatch(QStringLiteral("success"))), QStringLiteral("backup_success_dispatch"));
    expectOk(store.applyReceipt(receipt(QStringLiteral("success"))), QStringLiteral("backup_success_receipt"));
    expectOk(store.commitLedger(transaction(QStringLiteral("success"))), QStringLiteral("backup_success_ledger"));
    expectOk(store.commit(), QStringLiteral("backup_success_commit"));
    expectOk(store.backupTo(backup), QStringLiteral("backup_consistent_copy_created"));
    check(QFileInfo::exists(backup) && QFileInfo(backup).size() > 0, QStringLiteral("backup_real_file_exists"));
    const QByteArray backupHash = digest(backup);
    check(!errorCode(store.backupTo(backup)).isEmpty(), QStringLiteral("backup_refuses_overwrite"));
    check(digest(backup) == backupHash, QStringLiteral("backup_existing_copy_preserved"));
    expectOk(store.reserveAttempt(attempt(QStringLiteral("pending-backup"), QStringLiteral("pending-backup-scope"))), QStringLiteral("backup_pending_attempt"));
    const QString refused = directory + QStringLiteral("/backup-pending.sqlite");
    check(!errorCode(store.backupTo(refused)).isEmpty(), QStringLiteral("backup_pending_transaction_rejected"));
    check(!QFileInfo::exists(refused), QStringLiteral("backup_pending_creates_no_partial_file"));
    expectOk(store.rollback(), QStringLiteral("backup_pending_rollback"));
    SqliteEventStore restored;
    if (expectOk(restored.open(backup), QStringLiteral("backup_restore_open"))) {
        expectSnapshot(restored, 1, 1, 2, QStringLiteral("backup_restores_success_and_unresolved_reservation"));
        const auto recovery = restored.recoverUnresolved(QStringLiteral("session-1"));
        check(value(recovery) && value(recovery)->attempts.size() == 1
                  && value(recovery)->attempts.first().state == QStringLiteral("Unknown"),
              QStringLiteral("backup_restore_keeps_unknown_for_reconciliation"));
        const auto count = restored.eventCount();
        check(value(count) && *value(count) == 2, QStringLiteral("backup_restores_event_history"));
    }
}

int crashChild(const QString& path, const QString& phase)
{
    SqliteEventStore store;
    if (!ok(store.open(path, noRecovery()))) return 90;
    if (phase == QStringLiteral("before_transaction")) std::_Exit(71);
    if (!ok(store.appendEvent(event())) || !ok(store.reserveAttempt(attempt()))) return 91;
    if (phase == QStringLiteral("before_commit")) {
        // Exceed SQLite's page cache so pending writes reach the main DB and
        // leave a genuinely hot rollback journal, rather than a small buffered
        // transaction whose zero-header journal needs no native recovery.
        for (const QString& name : QSqlDatabase::connectionNames()) {
            const auto database = QSqlDatabase::database(name, false);
            if (database.isOpen() && QFileInfo(database.databaseName()).absoluteFilePath() == QFileInfo(path).absoluteFilePath()
                && !database.connectOptions().contains(QStringLiteral("QSQLITE_OPEN_READONLY"))) {
                QSqlQuery query(database);
                if (!query.exec(QStringLiteral("PRAGMA cache_size=4"))
                    || !query.exec(QStringLiteral("PRAGMA cache_spill=ON"))) return 96;
                break;
            }
        }
        auto spill = event(QStringLiteral("force-cache-spill"), 1);
        spill.payload.insert(QStringLiteral("synthetic_blob"), QString(4 * 1024 * 1024, QLatin1Char('j')));
        if (!ok(store.appendEvent(spill))) return 97;
        std::_Exit(72);
    }
    if (!ok(store.recordDispatch(dispatch()))) return 92;
    if (phase == QStringLiteral("before_receipt")) {
        if (!ok(store.commit())) return 93;
        std::_Exit(74);
    }
    if (phase == QStringLiteral("after_commit")) {
        if (!ok(store.applyReceipt(receipt())) || !ok(store.commitLedger(transaction())) || !ok(store.commit())) return 94;
        std::_Exit(73);
    }
    return 95;
}

void testCrashWindows(const QString& directory)
{
    const QStringList phases{QStringLiteral("before_transaction"), QStringLiteral("before_commit"),
                             QStringLiteral("after_commit"), QStringLiteral("before_receipt")};
    const int expectedExit[] = {71, 72, 73, 74};
    for (int index = 0; index < phases.size(); ++index) {
        const QString& phase = phases.at(index);
        const QString path = directory + QStringLiteral("/crash-") + phase + QStringLiteral(".sqlite");
        QProcess child;
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({QStringLiteral("--crash-child"), path, phase});
        child.start();
        const bool started = child.waitForStarted(10000);
        bool exited = started && child.waitForFinished(15000);
        if (started && !exited) {
            child.kill();
            child.waitForFinished(5000);
        }
        check(started && exited && child.exitStatus() == QProcess::NormalExit && child.exitCode() == expectedExit[index],
              QStringLiteral("crash_child_exact_exit_") + phase);
        if (!started || !exited || child.exitCode() != expectedExit[index]) {
            std::cout << "  crash_child_stdout=" << child.readAllStandardOutput().toStdString()
                      << "; stderr=" << child.readAllStandardError().toStdString()
                      << "; exit=" << child.exitCode() << '\n';
        }
        if (phase == QStringLiteral("before_commit")) {
            QFile journal(path + QStringLiteral("-journal"));
            const bool opened = journal.open(QIODevice::ReadOnly);
            const QByteArray header = opened ? journal.read(8) : QByteArray();
            check(opened && journal.size() > 512 && header.size() == 8 && header != QByteArray(8, '\0'),
                  QStringLiteral("crash_before_commit_has_real_hot_journal"));
            std::cout << "CRASH_HOT_JOURNAL_BYTES=" << (opened ? journal.size() : 0)
                      << "; header=" << header.toHex().toStdString() << '\n';
        }
        SqliteEventStore reopened;
        if (!expectOk(reopened.open(path), QStringLiteral("crash_reopen_") + phase)) continue;
        const bool committed = index >= 2;
        const int successes = index == 2 ? 1 : 0;
        const int unknown = index == 3 ? 1 : 0;
        expectSnapshot(reopened, successes, unknown, committed ? 1 : 0,
                       QStringLiteral("crash_committed_snapshot_matches_oracle_") + phase);
        const auto count = reopened.eventCount();
        check(value(count) && *value(count) == (committed ? 1 : 0), QStringLiteral("crash_atomic_event_count_") + phase);
        const auto recover = reopened.recoverUnresolved(QStringLiteral("session-1"));
        check(value(recover) && value(recover)->attempts.size() == unknown,
              QStringLiteral("crash_recovery_set_matches_oracle_") + phase);
        if (index == 3) {
            check(value(recover) && !value(recover)->attempts.isEmpty()
                      && value(recover)->attempts.first().state == QStringLiteral("Unknown"),
                  QStringLiteral("crash_sent_before_receipt_is_unknown_not_retried"));
            expectMutationError(reopened, reopened.recordDispatch(dispatch()), QStringLiteral("crash_recovery_blocks_automatic_redispatch"), QStringLiteral("ATTEMPT_UNKNOWN"));
        }
        if (index == 2) {
            const auto duplicate = reopened.applyReceipt(receipt());
            check(value(duplicate) && value(duplicate)->duplicate, QStringLiteral("crash_post_commit_receipt_replay_idempotent"));
            expectOk(reopened.commitLedger(transaction()), QStringLiteral("crash_post_commit_ledger_replay"));
            expectOk(reopened.commit(), QStringLiteral("crash_post_commit_replay_commit"));
            expectSnapshot(reopened, 1, 0, 1, QStringLiteral("crash_post_commit_replay_does_not_double_count"));
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 4 && app.arguments().at(1) == QStringLiteral("--crash-child"))
        return crashChild(app.arguments().at(2), app.arguments().at(3));
    QTemporaryDir directory;
    check(directory.isValid(), QStringLiteral("temporary_disk_directory_created"));
    check(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")), QStringLiteral("qsqlite_driver_available"));
    if (!directory.isValid() || !QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) return 1;
    testIdentity(directory.filePath(QStringLiteral("identity.sqlite")));
    testSuccessFailureAndVisibility(directory.filePath(QStringLiteral("success.sqlite")));
    testUnknownAndReceiptAssociation(directory.filePath(QStringLiteral("unknown.sqlite")));
    testRollbackAndRecovery(directory.filePath(QStringLiteral("recovery.sqlite")));
    testReadOnlyAndBusy(directory.filePath(QStringLiteral("busy.sqlite")));
    testFullDatabase(directory.filePath(QStringLiteral("full.sqlite")));
    testSchemaAndCorruption(directory.path());
    testConsistentBackup(directory.path());
    testCrashWindows(directory.path());
    std::cout << "SQLITE_LEDGER_TESTS=" << (failures == 0 ? "PASS" : "FAIL")
              << "; assertions=" << assertions << "; failures=" << failures
              << "; real_database=true; crash_processes=4; external_actions=0\n";
    return failures == 0 ? 0 : 1;
}
