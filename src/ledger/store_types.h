#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <variant>

namespace relink::ledger {

struct StoreError {
    QString code;
    QString message;
    QString entityId;
};

template <typename T>
using StoreResult = std::variant<T, StoreError>;
using StoreVoid = std::monostate;
using VoidResult = StoreResult<StoreVoid>;

struct EventDraft {
    QString eventId;
    QString runId;
    QString sessionId;
    QString clockDomainId;
    QString stepId;
    qint64 seq = 0;
    qint64 atMonoMs = 0;
    qint64 cancelEpoch = 0;
    qint64 viewportGeneration = 0;
    QString type;
    QJsonObject payload;
};

struct AppendReceipt {
    QString eventId;
    QString transactionId;
    bool duplicate = false;
};

struct AttemptDraft {
    QString attemptId;
    QString intentId;
    QString runId;
    QString ruleTaskId;
    int ruleRevision = 1;
    QString kind = QStringLiteral("purchase");
    int quantity = 1;
    QString reservationId;
    QString scopeId;
    // Explicit quota is required. Zero means scope is unresolved and is rejected.
    int quotaTarget = 0;
};

struct DispatchDraft {
    QString attemptId;
    QString eventId;
    QString state = QStringLiteral("Dispatching");
    QString dispatchProof = QStringLiteral("possibly_dispatched");
};

struct ReceiptDraft {
    QString attemptId;
    QString receiptIdentity;
    QString outcome;
    QString association = QStringLiteral("confirmed");
    QString eventId;
};

struct LedgerDraft {
    QString transactionId;
    QString attemptId;
    QString terminal;
    bool queueEmpty = false;
    QString receiptIdentity;
    QString eventId;
};

struct AttemptRecord {
    QString attemptId;
    QString intentId;
    QString runId;
    QString ruleTaskId;
    int ruleRevision = 1;
    QString kind = QStringLiteral("purchase");
    QString state = QStringLiteral("Prepared");
    int quantity = 1;
    bool reservationHeld = false;
    QString dispatchProof = QStringLiteral("not_dispatched");
    QString receiptIdentity;
    QString reservationId;
    QString scopeId;
    int quotaTarget = 0;
    QString transactionId;
    bool duplicate = false;
};

struct QuotaReservationRecord {
    QString reservationId;
    QString attemptId;
    QString scopeId;
    int quantity = 1;
    QString state = QStringLiteral("held"); // held|released|consumed|unresolved
};

struct LedgerTransactionRecord {
    QString transactionId;
    QString attemptId;
    QString terminal;
    bool queueEmpty = false;
    QString receiptIdentity;
};

struct LedgerSnapshot {
    QString runId;
    QVector<AttemptRecord> attempts;
    QVector<QuotaReservationRecord> reservations;
    QVector<LedgerTransactionRecord> transactions;
    int confirmedSuccess = 0;
    int unresolvedReservations = 0;
};

struct RecoverySet {
    QVector<AttemptRecord> attempts;
};

struct StoredRun {
    QString runId;
    QString sessionId;
};
struct RecoveryAuditRecord {
    QString attemptId, previousState, recoveredState, previousProof, reason;
};

class IEventStore {
public:
    virtual ~IEventStore() = default;
    virtual StoreResult<AppendReceipt> appendEvent(const EventDraft&) = 0;
    virtual StoreResult<AttemptRecord> reserveAttempt(const AttemptDraft&) = 0;
    virtual StoreResult<AttemptRecord> recordDispatch(const DispatchDraft&) = 0;
    virtual StoreResult<AttemptRecord> applyReceipt(const ReceiptDraft&) = 0;
    virtual StoreResult<LedgerTransactionRecord> commitLedger(const LedgerDraft&) = 0;
    virtual StoreResult<LedgerSnapshot> snapshot(const QString& runId) const = 0;
    virtual StoreResult<RecoverySet> recoverUnresolved(const QString& sessionId) const = 0;
    virtual VoidResult commit() = 0;
    virtual VoidResult rollback() = 0;
};

} // namespace relink::ledger
