#pragma once

#include "store_types.h"

#include <memory>

namespace relink::ledger {

// One instance owns two named Qt SQL connections on its construction thread.
// All methods and destruction must occur on that same thread. A writable
// instance owns a process-aware sidecar lock until close(), so recovery cannot
// reinterpret another live store's committed, in-flight attempts.
class SqliteEventStore final : public IEventStore {
public:
    struct OpenOptions {
        bool readOnly = false;
        int busyTimeoutMs = 1000;
        bool recoverOnOpen = true;
    };

    static constexpr int CurrentSchemaVersion = 2;

    SqliteEventStore();
    ~SqliteEventStore() override;
    SqliteEventStore(const SqliteEventStore&) = delete;
    SqliteEventStore& operator=(const SqliteEventStore&) = delete;

    VoidResult open(const QString& path);
    VoidResult open(const QString& path, const OpenOptions& options);
    VoidResult close(); // Rolls back any uncommitted work; never commits it.
    bool isOpen() const;
    int schemaVersion() const;
    StoreResult<int> eventCount() const; // Committed events only.
    StoreResult<QVector<StoredRun>> committedRuns() const;
    StoreResult<QVector<EventDraft>> eventsForRun(const QString& runId) const;
    StoreResult<QVector<RecoveryAuditRecord>> recoveryAudit(const QString& runId) const;
    VoidResult backupTo(const QString& destinationPath) const;

    StoreResult<AppendReceipt> appendEvent(const EventDraft&) override;
    StoreResult<AttemptRecord> reserveAttempt(const AttemptDraft&) override;
    StoreResult<AttemptRecord> recordDispatch(const DispatchDraft&) override;
    StoreResult<AttemptRecord> applyReceipt(const ReceiptDraft&) override;
    StoreResult<LedgerTransactionRecord> commitLedger(const LedgerDraft&) override;
    StoreResult<LedgerSnapshot> snapshot(const QString& runId) const override;
    StoreResult<RecoverySet> recoverUnresolved(const QString& sessionId) const override;
    VoidResult commit() override;
    VoidResult rollback() override;

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace relink::ledger
