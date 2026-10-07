#pragma once

#include "ledger/sqlite_event_store.h"
#include <QString>
#include <variant>

namespace relink::workspace {

struct HistoryRunBundle {
    ledger::RunSummary summary;
    QVector<ledger::EventDraft> events;
    ledger::LedgerSnapshot snapshot;
    QVector<ledger::RecoveryAuditRecord> audits;
};
struct HistoryQueryError { QString code; QString message; };
using HistorySummaryResult = std::variant<QVector<ledger::RunSummary>, HistoryQueryError>;
using HistoryDetailResult = std::variant<HistoryRunBundle, HistoryQueryError>;

// Read-only history boundary. Summary enumeration does not read event or
// ledger detail. Selected-run detail uses the same store connection owned by
// WorkspaceController's construction thread.
class HistoryQueryService final {
public:
    HistorySummaryResult loadSummaries(ledger::SqliteEventStore& store) const;
    HistoryDetailResult loadSelected(ledger::SqliteEventStore& store,
                                     const QString& runId) const;
};

} // namespace relink::workspace
