#include "ledger/in_memory_event_store.h"

#include <QCoreApplication>
#include <iostream>

using namespace relink::ledger;
namespace {
int failures = 0;
void check(bool ok, const char* name) { std::cout << (ok ? "PASS " : "FAIL ") << name << '\n'; if (!ok) ++failures; }
template <typename T> bool ok(const StoreResult<T>& r) { return std::holds_alternative<T>(r); }
template <typename T> const T* value(const StoreResult<T>& r) { return std::get_if<T>(&r); }
template <typename T> const StoreError* err(const StoreResult<T>& r) { return std::get_if<StoreError>(&r); }
AttemptDraft draft(const char* attempt, const char* intent, const char* reservation, int quota = 1) {
    AttemptDraft d; d.attemptId = attempt; d.intentId = intent; d.runId = QStringLiteral("run-1");
    d.ruleTaskId = QStringLiteral("task-1"); d.quantity = 1; d.reservationId = reservation;
    d.scopeId = QStringLiteral("scope-1"); d.quotaTarget = quota; return d;
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    InMemoryEventStore store;
    EventDraft event; event.eventId = QStringLiteral("event-1"); event.runId = QStringLiteral("run-1"); event.sessionId = QStringLiteral("session-1"); event.seq = 1; event.type = QStringLiteral("ReservationRequested");
    event.payload.insert(QStringLiteral("attempt_id"), QStringLiteral("attempt-1"));
    auto e1 = store.appendEvent(event); check(ok(e1) && !value(e1)->duplicate, "event_append");
    auto eDup = store.appendEvent(event); check(ok(eDup) && value(eDup)->duplicate, "event_duplicate_idempotent");
    EventDraft keyOrder = event; keyOrder.eventId = QStringLiteral("event-key-order"); keyOrder.seq = 0; keyOrder.payload = QJsonObject{{QStringLiteral("b"), 2}, {QStringLiteral("a"), 1}};
    check(ok(store.appendEvent(keyOrder)), "event_key_order_first");
    keyOrder.payload = QJsonObject{{QStringLiteral("a"), 1}, {QStringLiteral("b"), 2}};
    auto keyOrderDup = store.appendEvent(keyOrder); check(ok(keyOrderDup) && value(keyOrderDup)->duplicate, "event_key_order_idempotent");
    EventDraft seqZeroConflict = keyOrder; seqZeroConflict.eventId = QStringLiteral("event-seq-zero-conflict");
    auto seqZeroResult = store.appendEvent(seqZeroConflict);
    check(!ok(seqZeroResult) && err(seqZeroResult)->code == QStringLiteral("EVENT_SEQ_CONFLICT"), "event_seq_zero_conflict");
    EventDraft seqConflict = event; seqConflict.eventId = QStringLiteral("event-seq-conflict"); seqConflict.seq = 1;
    auto seqResult = store.appendEvent(seqConflict); check(!ok(seqResult) && err(seqResult)->code == QStringLiteral("EVENT_SEQ_CONFLICT"), "event_seq_conflict");
    event.payload.insert(QStringLiteral("changed"), true); auto eConflict = store.appendEvent(event);
    check(!ok(eConflict) && err(eConflict)->code == QStringLiteral("EVENT_ID_CONFLICT"), "event_payload_conflict");

    auto reserved = store.reserveAttempt(draft("attempt-1", "intent-1", "reservation-1"));
    check(ok(reserved) && value(reserved)->state == QStringLiteral("Prepared") && value(reserved)->reservationHeld, "reserve_prepared");
    auto duplicateAttempt = store.reserveAttempt(draft("attempt-1", "intent-1", "reservation-1"));
    check(ok(duplicateAttempt) && value(duplicateAttempt)->duplicate, "attempt_duplicate_idempotent");
    auto quotaFull = store.reserveAttempt(draft("attempt-2", "intent-2", "reservation-2"));
    check(!ok(quotaFull) && err(quotaFull)->code == QStringLiteral("QUOTA_FULL"), "quota_full");
    AttemptDraft noReservation = draft("attempt-no-reservation", "intent-no-reservation", "");
    auto invalidReservation = store.reserveAttempt(noReservation);
    check(!ok(invalidReservation) && err(invalidReservation)->code == QStringLiteral("INVALID_RESERVATION"), "reservation_identity_required");

    auto dispatched = store.recordDispatch({QStringLiteral("attempt-1"), QStringLiteral("dispatch-1"), QStringLiteral("Sent"), QStringLiteral("acknowledged")});
    check(ok(dispatched) && value(dispatched)->state == QStringLiteral("Sent"), "dispatch_recorded");
    auto regression = store.recordDispatch({QStringLiteral("attempt-1"), QStringLiteral("dispatch-regression"), QStringLiteral("Dispatching"), QStringLiteral("possibly_dispatched")});
    check(!ok(regression) && err(regression)->code == QStringLiteral("DISPATCH_REGRESSION"), "dispatch_regression_rejected");
    auto unknown = store.applyReceipt({QStringLiteral("attempt-1"), QStringLiteral("receipt-1"), QStringLiteral("unknown"), QStringLiteral("confirmed"), QStringLiteral("receipt-event-1")});
    check(ok(unknown) && value(unknown)->state == QStringLiteral("Unknown") && value(unknown)->reservationHeld, "unknown_keeps_reservation");
    auto unknownDispatch = store.recordDispatch({QStringLiteral("attempt-1"), QStringLiteral("dispatch-unknown"), QStringLiteral("Sent"), QStringLiteral("acknowledged")});
    check(!ok(unknownDispatch) && err(unknownDispatch)->code == QStringLiteral("ATTEMPT_UNKNOWN"), "unknown_blocks_redispatch");
    auto unknownDup = store.applyReceipt({QStringLiteral("attempt-1"), QStringLiteral("receipt-1"), QStringLiteral("unknown"), QStringLiteral("confirmed"), QStringLiteral("receipt-event-2")});
    check(ok(unknownDup) && value(unknownDup)->duplicate, "receipt_duplicate_idempotent");
    auto receiptConflict = store.applyReceipt({QStringLiteral("attempt-1"), QStringLiteral("receipt-1"), QStringLiteral("success"), QStringLiteral("confirmed"), QStringLiteral("receipt-event-3")});
    check(!ok(receiptConflict) && err(receiptConflict)->code == QStringLiteral("RECEIPT_CONFLICT"), "receipt_identity_conflict");

    auto tx = store.commitLedger({QStringLiteral("tx-1"), QStringLiteral("attempt-1"), QStringLiteral("Success"), true, QStringLiteral("receipt-1"), QStringLiteral("ledger-event-1")});
    check(ok(tx) && value(tx)->terminal == QStringLiteral("Success"), "ledger_commit_success");
    auto txDup = store.commitLedger({QStringLiteral("tx-1"), QStringLiteral("attempt-1"), QStringLiteral("Success"), true, QStringLiteral("receipt-1"), QStringLiteral("ledger-event-2")});
    check(ok(txDup), "ledger_duplicate_idempotent");
    auto txReceiptConflict = store.commitLedger({QStringLiteral("tx-1"), QStringLiteral("attempt-1"), QStringLiteral("Success"), true, QStringLiteral("receipt-other"), QStringLiteral("ledger-event-2b")});
    check(!ok(txReceiptConflict) && err(txReceiptConflict)->code == QStringLiteral("TRANSACTION_ID_CONFLICT"), "ledger_receipt_identity_conflict");
    auto txConflict = store.commitLedger({QStringLiteral("tx-1"), QStringLiteral("attempt-1"), QStringLiteral("Failed"), true, QStringLiteral("receipt-1"), QStringLiteral("ledger-event-3")});
    check(!ok(txConflict) && err(txConflict)->code == QStringLiteral("TRANSACTION_ID_CONFLICT"), "ledger_transaction_conflict");
    auto terminalReceipt = store.applyReceipt({QStringLiteral("attempt-1"), QStringLiteral("receipt-new"), QStringLiteral("success"), QStringLiteral("confirmed"), QStringLiteral("receipt-event-terminal")});
    check(!ok(terminalReceipt) && err(terminalReceipt)->code == QStringLiteral("ATTEMPT_TERMINAL"), "terminal_rejects_new_receipt");
    auto snapshot = store.snapshot(QStringLiteral("run-1"));
    check(ok(snapshot) && value(snapshot)->confirmedSuccess == 1 && value(snapshot)->unresolvedReservations == 0, "success_consumes_reservation");

    auto quotaStillFull = store.reserveAttempt(draft("attempt-2", "intent-2", "reservation-2"));
    check(!ok(quotaStillFull) && err(quotaStillFull)->code == QStringLiteral("QUOTA_FULL"), "success_counts_against_quota");
    AttemptDraft secondDraft = draft("attempt-2", "intent-2", "reservation-2");
    secondDraft.scopeId = QStringLiteral("scope-2");
    auto second = store.reserveAttempt(secondDraft);
    check(ok(second), "independent_scope_reservation");
    auto failed = store.applyReceipt({QStringLiteral("attempt-2"), QStringLiteral("receipt-2"), QStringLiteral("failed"), QStringLiteral("confirmed"), QStringLiteral("receipt-event-4")});
    check(ok(failed) && value(failed)->state == QStringLiteral("AwaitingReceipt"), "failed_receipt_pending_commit");
    auto failedTx = store.commitLedger({QStringLiteral("tx-2"), QStringLiteral("attempt-2"), QStringLiteral("Failed"), false, QStringLiteral("receipt-2"), QStringLiteral("ledger-event-3")});
    check(ok(failedTx), "ledger_commit_failed");
    snapshot = store.snapshot(QStringLiteral("run-1"));
    check(ok(snapshot) && value(snapshot)->unresolvedReservations == 0, "failed_releases_reservation");
    store.commit();

    AttemptDraft thirdDraft = draft("attempt-3", "intent-3", "reservation-3");
    thirdDraft.scopeId = QStringLiteral("scope-2");
    auto uncommitted = store.reserveAttempt(thirdDraft); check(ok(uncommitted), "reserve_before_rollback");
    store.rollback(); auto afterRollback = store.snapshot(QStringLiteral("run-1"));
    check(ok(afterRollback) && value(afterRollback)->attempts.size() == 2, "rollback_restores_committed_state");
    store.reserveAttempt(thirdDraft); store.commit();
    auto recovery = store.recoverUnresolved(QStringLiteral("session-1"));
    check(ok(recovery) && value(recovery)->attempts.size() == 1 && value(recovery)->attempts.first().attemptId == QStringLiteral("attempt-3"), "recover_unresolved_reservation");
    auto unresolvedScope = store.reserveAttempt(draft("attempt-4", "intent-4", "reservation-4", 0));
    check(!ok(unresolvedScope) && err(unresolvedScope)->code == QStringLiteral("QUOTA_SCOPE_UNRESOLVED"), "quota_scope_required");
    AttemptDraft pendingCommit = draft("attempt-5", "intent-5", "reservation-5");
    pendingCommit.scopeId = QStringLiteral("scope-5");
    check(ok(store.reserveAttempt(pendingCommit)), "pending_commit_reserve");
    auto invalidLedger = store.commitLedger({QStringLiteral("tx-5"), QStringLiteral("attempt-5"), QStringLiteral("Success"), false, QStringLiteral("receipt-5"), QStringLiteral("ledger-event-5")});
    check(!ok(invalidLedger) && err(invalidLedger)->code == QStringLiteral("ATTEMPT_STATE_INVALID"), "commit_requires_pending_receipt");
    return failures == 0 ? 0 : 1;
}
