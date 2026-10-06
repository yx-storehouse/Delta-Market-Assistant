#pragma once

#include "store_types.h"

#include <QHash>
#include <QVector>

namespace relink::ledger {

class InMemoryEventStore final : public IEventStore {
public:
    InMemoryEventStore();

    StoreResult<AppendReceipt> appendEvent(const EventDraft&) override;
    StoreResult<AttemptRecord> reserveAttempt(const AttemptDraft&) override;
    StoreResult<AttemptRecord> recordDispatch(const DispatchDraft&) override;
    StoreResult<AttemptRecord> applyReceipt(const ReceiptDraft&) override;
    StoreResult<LedgerTransactionRecord> commitLedger(const LedgerDraft&) override;
    StoreResult<LedgerSnapshot> snapshot(const QString& runId) const override;
    StoreResult<RecoverySet> recoverUnresolved(const QString& sessionId) const override;
    VoidResult commit() override;
    VoidResult rollback() override;

    int eventCount() const { return m_events.size(); }
    int attemptCount() const { return m_attempts.size(); }

private:
    struct EventRecord { EventDraft draft; QByteArray canonicalPayload; };
    struct ReceiptRecord { ReceiptDraft draft; QByteArray canonicalPayload; };

    static QByteArray canonical(const QJsonObject& payload);
    static StoreError error(const QString& code, const QString& message, const QString& id = {});
    AttemptRecord* findAttempt(const QString& attemptId);
    const AttemptRecord* findAttempt(const QString& attemptId) const;
    QuotaReservationRecord* findReservation(const QString& reservationId);
    const QuotaReservationRecord* findReservation(const QString& reservationId) const;
    bool sameAttempt(const AttemptRecord&, const AttemptDraft&) const;
    int scopeUsed(const QString& scopeId) const;
    bool scopeHasUnknown(const QString& scopeId) const;
    LedgerSnapshot snapshotImpl(const QString& runId) const;

    QHash<QString, EventRecord> m_events;
    QHash<QString, AttemptRecord> m_attempts;
    QHash<QString, QString> m_intentToAttempt;
    QHash<QString, ReceiptRecord> m_receipts;
    QHash<QString, LedgerTransactionRecord> m_transactions;
    QHash<QString, QuotaReservationRecord> m_reservations;
    QHash<QString, QString> m_runSessions;

    QHash<QString, EventRecord> m_committedEvents;
    QHash<QString, AttemptRecord> m_committedAttempts;
    QHash<QString, QString> m_committedIntentToAttempt;
    QHash<QString, ReceiptRecord> m_committedReceipts;
    QHash<QString, LedgerTransactionRecord> m_committedTransactions;
    QHash<QString, QuotaReservationRecord> m_committedReservations;
    QHash<QString, QString> m_committedRunSessions;
};

} // namespace relink::ledger
