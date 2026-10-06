#include "in_memory_event_store.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

#include <algorithm>

namespace relink::ledger {
namespace {
QString receiptKey(const QString& attemptId, const QString& identity)
{
    return attemptId + QChar(0x1f) + identity;
}
}

InMemoryEventStore::InMemoryEventStore()
    : m_committedEvents(m_events), m_committedAttempts(m_attempts),
      m_committedIntentToAttempt(m_intentToAttempt), m_committedReceipts(m_receipts),
      m_committedTransactions(m_transactions), m_committedReservations(m_reservations),
      m_committedRunSessions(m_runSessions)
{
}

namespace {
QJsonValue canonicalValue(const QJsonValue& value)
{
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        QStringList keys = object.keys();
        std::sort(keys.begin(), keys.end());
        QJsonObject sorted;
        for (const QString& key : keys) sorted.insert(key, canonicalValue(object.value(key)));
        return sorted;
    }
    if (value.isArray()) {
        QJsonArray array;
        for (const QJsonValue& item : value.toArray()) array.append(canonicalValue(item));
        return array;
    }
    return value;
}
}

QByteArray InMemoryEventStore::canonical(const QJsonObject& payload)
{
    return QJsonDocument(canonicalValue(payload).toObject()).toJson(QJsonDocument::Compact);
}

StoreError InMemoryEventStore::error(const QString& code, const QString& message, const QString& id)
{
    return {code, message, id};
}

AttemptRecord* InMemoryEventStore::findAttempt(const QString& id) { return m_attempts.contains(id) ? &m_attempts[id] : nullptr; }
const AttemptRecord* InMemoryEventStore::findAttempt(const QString& id) const { auto it = m_attempts.constFind(id); return it == m_attempts.constEnd() ? nullptr : &it.value(); }
QuotaReservationRecord* InMemoryEventStore::findReservation(const QString& id) { return m_reservations.contains(id) ? &m_reservations[id] : nullptr; }
const QuotaReservationRecord* InMemoryEventStore::findReservation(const QString& id) const { auto it = m_reservations.constFind(id); return it == m_reservations.constEnd() ? nullptr : &it.value(); }

bool InMemoryEventStore::sameAttempt(const AttemptRecord& a, const AttemptDraft& d) const
{
    return a.attemptId == d.attemptId && a.intentId == d.intentId && a.runId == d.runId
        && a.ruleTaskId == d.ruleTaskId && a.ruleRevision == d.ruleRevision && a.kind == d.kind
        && a.quantity == d.quantity && a.reservationId == d.reservationId
        && a.scopeId == d.scopeId && a.quotaTarget == d.quotaTarget;
}

int InMemoryEventStore::scopeUsed(const QString& scopeId) const
{
    int used = 0;
    for (const AttemptRecord& a : m_attempts) {
        if (a.scopeId != scopeId) continue;
        if (a.state == QStringLiteral("Success") || a.reservationHeld)
            used += a.quantity;
    }
    return used;
}

bool InMemoryEventStore::scopeHasUnknown(const QString& scopeId) const
{
    for (const AttemptRecord& a : m_attempts)
        if (a.scopeId == scopeId && a.reservationHeld && a.state == QStringLiteral("Unknown")) return true;
    return false;
}

StoreResult<AppendReceipt> InMemoryEventStore::appendEvent(const EventDraft& draft)
{
    if (draft.eventId.isEmpty()) return error(QStringLiteral("INVALID_EVENT"), QStringLiteral("event_id is required"));
    const QByteArray payload = canonical(draft.payload);
    if (m_events.contains(draft.eventId)) {
        const EventRecord& old = m_events.value(draft.eventId);
        const EventDraft& prior = old.draft;
        const bool same = old.canonicalPayload == payload && prior.runId == draft.runId
            && prior.sessionId == draft.sessionId && prior.clockDomainId == draft.clockDomainId
            && prior.stepId == draft.stepId && prior.seq == draft.seq && prior.atMonoMs == draft.atMonoMs
            && prior.cancelEpoch == draft.cancelEpoch && prior.viewportGeneration == draft.viewportGeneration
            && prior.type == draft.type;
        if (!same)
            return error(QStringLiteral("EVENT_ID_CONFLICT"), QStringLiteral("event identity or payload differs"), draft.eventId);
        return AppendReceipt{draft.eventId, QStringLiteral("event:") + draft.eventId, true};
    }
    // `seq` is assigned by the coordinator and is a non-negative value in the
    // storage contract.  Zero is therefore a real sequence key, not an
    // "unset" sentinel; enforce the same (run_id, seq) uniqueness for it.
    if (!draft.runId.isEmpty()) {
        for (const EventRecord& existing : m_events) {
            if (existing.draft.runId == draft.runId && existing.draft.seq == draft.seq)
                return error(QStringLiteral("EVENT_SEQ_CONFLICT"), QStringLiteral("run sequence already exists"), draft.eventId);
        }
    }
    m_events.insert(draft.eventId, EventRecord{draft, payload});
    if (!draft.runId.isEmpty() && !draft.sessionId.isEmpty()) m_runSessions.insert(draft.runId, draft.sessionId);
    return AppendReceipt{draft.eventId, QStringLiteral("event:") + draft.eventId, false};
}

StoreResult<AttemptRecord> InMemoryEventStore::reserveAttempt(const AttemptDraft& draft)
{
    if (draft.attemptId.isEmpty() || draft.intentId.isEmpty() || draft.runId.isEmpty())
        return error(QStringLiteral("INVALID_ATTEMPT"), QStringLiteral("attempt, intent and run IDs are required"), draft.attemptId);
    if (draft.quantity <= 0) return error(QStringLiteral("INVALID_QUANTITY"), QStringLiteral("quantity must be positive"), draft.attemptId);
    if (draft.reservationId.isEmpty()) return error(QStringLiteral("INVALID_RESERVATION"), QStringLiteral("reservation identity is required"), draft.attemptId);
    if (draft.ruleRevision <= 0) return error(QStringLiteral("INVALID_RULE_REVISION"), QStringLiteral("revision must be positive"), draft.attemptId);
    if (draft.scopeId.isEmpty() || draft.quotaTarget <= 0)
        return error(QStringLiteral("QUOTA_SCOPE_UNRESOLVED"), QStringLiteral("explicit quota scope and target are required"), draft.attemptId);
    if (m_attempts.contains(draft.attemptId)) {
        AttemptRecord& existing = m_attempts[draft.attemptId];
        if (!sameAttempt(existing, draft)) return error(QStringLiteral("ATTEMPT_ID_CONFLICT"), QStringLiteral("attempt identity differs"), draft.attemptId);
        existing.duplicate = true;
        return existing;
    }
    if (m_intentToAttempt.contains(draft.intentId)) {
        return error(QStringLiteral("INTENT_ID_CONFLICT"), QStringLiteral("intent already reserved"), draft.intentId);
    }
    if (m_reservations.contains(draft.reservationId))
        return error(QStringLiteral("RESERVATION_CONFLICT"), QStringLiteral("reservation identity already exists"), draft.reservationId);
    if (scopeUsed(draft.scopeId) + draft.quantity > draft.quotaTarget)
        return error(QStringLiteral("QUOTA_FULL"), scopeHasUnknown(draft.scopeId) ? QStringLiteral("unknown reservation occupies quota") : QStringLiteral("quota is full"), draft.scopeId);

    AttemptRecord record;
    record.attemptId = draft.attemptId; record.intentId = draft.intentId; record.runId = draft.runId;
    record.ruleTaskId = draft.ruleTaskId; record.ruleRevision = draft.ruleRevision; record.kind = draft.kind;
    record.quantity = draft.quantity; record.state = QStringLiteral("Prepared"); record.reservationHeld = true;
    record.dispatchProof = QStringLiteral("not_dispatched"); record.reservationId = draft.reservationId;
    record.scopeId = draft.scopeId; record.quotaTarget = draft.quotaTarget;
    m_attempts.insert(record.attemptId, record); m_intentToAttempt.insert(record.intentId, record.attemptId);
    m_reservations.insert(record.reservationId, {record.reservationId, record.attemptId, record.scopeId, record.quantity, QStringLiteral("held")});
    return record;
}

StoreResult<AttemptRecord> InMemoryEventStore::recordDispatch(const DispatchDraft& draft)
{
    AttemptRecord* attempt = findAttempt(draft.attemptId);
    if (!attempt) return error(QStringLiteral("ATTEMPT_NOT_FOUND"), QStringLiteral("attempt does not exist"), draft.attemptId);
    if (attempt->state == QStringLiteral("Unknown"))
        return error(QStringLiteral("ATTEMPT_UNKNOWN"), QStringLiteral("unknown attempt requires reconciliation before dispatch"), draft.attemptId);
    if (draft.state != QStringLiteral("Dispatching") && draft.state != QStringLiteral("Sent") && draft.state != QStringLiteral("AwaitingReceipt"))
        return error(QStringLiteral("INVALID_DISPATCH_STATE"), QStringLiteral("unsupported dispatch state"), draft.attemptId);
    if (draft.dispatchProof != QStringLiteral("not_dispatched") && draft.dispatchProof != QStringLiteral("possibly_dispatched") && draft.dispatchProof != QStringLiteral("acknowledged"))
        return error(QStringLiteral("INVALID_DISPATCH_PROOF"), QStringLiteral("unsupported dispatch proof"), draft.attemptId);
    auto stateRank = [](const QString& state) { if (state == QStringLiteral("Dispatching")) return 1; if (state == QStringLiteral("Sent")) return 2; if (state == QStringLiteral("AwaitingReceipt")) return 3; return 0; };
    auto proofRank = [](const QString& proof) { if (proof == QStringLiteral("possibly_dispatched")) return 1; if (proof == QStringLiteral("acknowledged")) return 2; return 0; };
    if (stateRank(draft.state) < stateRank(attempt->state) || proofRank(draft.dispatchProof) < proofRank(attempt->dispatchProof))
        return error(QStringLiteral("DISPATCH_REGRESSION"), QStringLiteral("dispatch state cannot move backwards"), draft.attemptId);
    if (attempt->state == draft.state && attempt->dispatchProof == draft.dispatchProof) { attempt->duplicate = true; return *attempt; }
    if (attempt->state == QStringLiteral("Success") || attempt->state == QStringLiteral("Failed") || attempt->state == QStringLiteral("Cancelled"))
        return error(QStringLiteral("ATTEMPT_TERMINAL"), QStringLiteral("terminal attempt cannot dispatch"), draft.attemptId);
    attempt->state = draft.state; attempt->dispatchProof = draft.dispatchProof; attempt->duplicate = false;
    return *attempt;
}

StoreResult<AttemptRecord> InMemoryEventStore::applyReceipt(const ReceiptDraft& draft)
{
    AttemptRecord* attempt = findAttempt(draft.attemptId);
    if (!attempt) return error(QStringLiteral("ATTEMPT_NOT_FOUND"), QStringLiteral("attempt does not exist"), draft.attemptId);
    if (draft.receiptIdentity.isEmpty()) return error(QStringLiteral("INVALID_RECEIPT"), QStringLiteral("receipt identity is required"), draft.attemptId);
    if (draft.outcome != QStringLiteral("success") && draft.outcome != QStringLiteral("failed") && draft.outcome != QStringLiteral("unknown"))
        return error(QStringLiteral("INVALID_RECEIPT_OUTCOME"), QStringLiteral("unsupported receipt outcome"), draft.attemptId);
    if (draft.association != QStringLiteral("confirmed") && draft.association != QStringLiteral("ambiguous"))
        return error(QStringLiteral("INVALID_ASSOCIATION"), QStringLiteral("unsupported receipt association"), draft.attemptId);
    const QString key = receiptKey(draft.attemptId, draft.receiptIdentity);
    const QJsonObject payload{{QStringLiteral("outcome"), draft.outcome}, {QStringLiteral("association"), draft.association}};
    const QByteArray data = canonical(payload);
    if (m_receipts.contains(key)) {
        const ReceiptRecord& old = m_receipts.value(key);
        if (old.canonicalPayload != data) return error(QStringLiteral("RECEIPT_CONFLICT"), QStringLiteral("receipt identity differs"), draft.receiptIdentity);
        AttemptRecord duplicate = *attempt; duplicate.duplicate = true; return duplicate;
    }
    if (attempt->state == QStringLiteral("Success") || attempt->state == QStringLiteral("Failed")
        || attempt->state == QStringLiteral("Cancelled"))
        return error(QStringLiteral("ATTEMPT_TERMINAL"), QStringLiteral("terminal attempt cannot accept a new receipt"), draft.attemptId);
    m_receipts.insert(key, ReceiptRecord{draft, data});
    attempt->receiptIdentity = draft.receiptIdentity; attempt->duplicate = false;
    if (draft.outcome == QStringLiteral("unknown") || draft.association != QStringLiteral("confirmed")) {
        attempt->state = QStringLiteral("Unknown"); attempt->dispatchProof = QStringLiteral("possibly_dispatched");
        attempt->reservationHeld = true;
        if (QuotaReservationRecord* r = findReservation(attempt->reservationId)) r->state = QStringLiteral("unresolved");
    } else {
        attempt->state = QStringLiteral("AwaitingReceipt");
    }
    return *attempt;
}

StoreResult<LedgerTransactionRecord> InMemoryEventStore::commitLedger(const LedgerDraft& draft)
{
    AttemptRecord* attempt = findAttempt(draft.attemptId);
    if (!attempt) return error(QStringLiteral("ATTEMPT_NOT_FOUND"), QStringLiteral("attempt does not exist"), draft.attemptId);
    if (draft.transactionId.isEmpty()) return error(QStringLiteral("INVALID_TRANSACTION"), QStringLiteral("transaction identity is required"), draft.transactionId);
    if (draft.terminal != QStringLiteral("Success") && draft.terminal != QStringLiteral("Failed"))
        return error(QStringLiteral("INVALID_TERMINAL"), QStringLiteral("unsupported terminal"), draft.transactionId);
    if (attempt->transactionId.isEmpty() == false && attempt->transactionId != draft.transactionId)
        return error(QStringLiteral("ATTEMPT_TRANSACTION_CONFLICT"), QStringLiteral("attempt already has a ledger transaction"), draft.attemptId);
    if (m_transactions.contains(draft.transactionId)) {
        const LedgerTransactionRecord& old = m_transactions.value(draft.transactionId);
        if (old.attemptId != draft.attemptId || old.terminal != draft.terminal || old.queueEmpty != draft.queueEmpty
            || old.receiptIdentity != draft.receiptIdentity)
            return error(QStringLiteral("TRANSACTION_ID_CONFLICT"), QStringLiteral("transaction identity differs"), draft.transactionId);
        AttemptRecord duplicate = *attempt; Q_UNUSED(duplicate);
        return old;
    }
    if (attempt->state != QStringLiteral("AwaitingReceipt") && attempt->state != QStringLiteral("Unknown"))
        return error(QStringLiteral("ATTEMPT_STATE_INVALID"), QStringLiteral("ledger commit requires a pending receipt"), draft.attemptId);
    if (attempt->receiptIdentity.isEmpty())
        return error(QStringLiteral("RECEIPT_REQUIRED"), QStringLiteral("ledger commit requires a receipt identity"), draft.attemptId);
    if (draft.receiptIdentity != attempt->receiptIdentity)
        return error(QStringLiteral("RECEIPT_MISMATCH"), QStringLiteral("ledger receipt differs from attempt receipt"), draft.attemptId);
    const auto receipt = m_receipts.constFind(receiptKey(draft.attemptId, draft.receiptIdentity));
    if (receipt == m_receipts.cend() || receipt->draft.association != QStringLiteral("confirmed")
        || receipt->draft.outcome == QStringLiteral("unknown"))
        return error(QStringLiteral("RECEIPT_UNCONFIRMED"), QStringLiteral("a confirmed terminal receipt is required"), draft.attemptId);
    const QString expected = draft.terminal == QStringLiteral("Success") ? QStringLiteral("success") : QStringLiteral("failed");
    if (receipt->draft.outcome != expected)
        return error(QStringLiteral("RECEIPT_OUTCOME_MISMATCH"), QStringLiteral("ledger terminal differs from confirmed receipt"), draft.attemptId);
    LedgerTransactionRecord tx{draft.transactionId, draft.attemptId, draft.terminal, draft.queueEmpty, draft.receiptIdentity};
    m_transactions.insert(tx.transactionId, tx); attempt->transactionId = tx.transactionId;
    QuotaReservationRecord* reservation = findReservation(attempt->reservationId);
    if (draft.terminal == QStringLiteral("Success")) {
        attempt->state = QStringLiteral("Success"); attempt->reservationHeld = false;
        if (reservation) reservation->state = QStringLiteral("consumed");
    } else {
        attempt->state = QStringLiteral("Failed"); attempt->reservationHeld = false;
        if (reservation) reservation->state = QStringLiteral("released");
    }
    return tx;
}

LedgerSnapshot InMemoryEventStore::snapshotImpl(const QString& runId) const
{
    LedgerSnapshot out; out.runId = runId;
    for (const AttemptRecord& a : m_attempts) {
        if (a.runId != runId) continue;
        out.attempts.push_back(a);
        if (a.state == QStringLiteral("Success")) out.confirmedSuccess += a.quantity;
        if (a.reservationHeld) ++out.unresolvedReservations;
    }
    for (const auto& r : m_reservations) {
        if (const AttemptRecord* a = findAttempt(r.attemptId); a && a->runId == runId) out.reservations.push_back(r);
    }
    for (const auto& tx : m_transactions) {
        if (const AttemptRecord* a = findAttempt(tx.attemptId); a && a->runId == runId) out.transactions.push_back(tx);
    }
    return out;
}

StoreResult<LedgerSnapshot> InMemoryEventStore::snapshot(const QString& runId) const { return snapshotImpl(runId); }

StoreResult<RecoverySet> InMemoryEventStore::recoverUnresolved(const QString& sessionId) const
{
    RecoverySet out;
    for (const AttemptRecord& a : m_attempts) {
        if (!a.reservationHeld) continue;
        if (!sessionId.isEmpty() && m_runSessions.value(a.runId) == sessionId) out.attempts.push_back(a);
    }
    return out;
}

VoidResult InMemoryEventStore::commit()
{
    m_committedEvents = m_events; m_committedAttempts = m_attempts; m_committedIntentToAttempt = m_intentToAttempt;
    m_committedReceipts = m_receipts; m_committedTransactions = m_transactions; m_committedReservations = m_reservations;
    m_committedRunSessions = m_runSessions;
    return std::monostate{};
}

VoidResult InMemoryEventStore::rollback()
{
    m_events = m_committedEvents; m_attempts = m_committedAttempts; m_intentToAttempt = m_committedIntentToAttempt;
    m_receipts = m_committedReceipts; m_transactions = m_committedTransactions; m_reservations = m_committedReservations;
    m_runSessions = m_committedRunSessions;
    return std::monostate{};
}

} // namespace relink::ledger
