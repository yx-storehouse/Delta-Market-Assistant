#include "history_query_service.h"

namespace relink::workspace {
namespace {
template <typename T>
bool fail(const ledger::StoreResult<T>& result, HistoryQueryError* error, const QString& operation)
{
    if (const auto* failure = std::get_if<ledger::StoreError>(&result)) {
        if (error) *error = {failure->code, operation + QStringLiteral(": ") + failure->message};
        return true;
    }
    return false;
}
}

HistorySummaryResult HistoryQueryService::loadSummaries(ledger::SqliteEventStore& store) const
{
    const auto runs = store.runSummaries();
    if (const auto* failure = std::get_if<ledger::StoreError>(&runs))
        return HistoryQueryError{failure->code, QStringLiteral("runSummaries: ") + failure->message};
    return std::get<QVector<ledger::RunSummary>>(runs);
}

HistoryDetailResult HistoryQueryService::loadSelected(ledger::SqliteEventStore& store,
                                                      const QString& runId) const
{
    const auto summaries = store.runSummaries();
    if (const auto* failure = std::get_if<ledger::StoreError>(&summaries))
        return HistoryQueryError{failure->code, QStringLiteral("runSummaries: ") + failure->message};
    ledger::RunSummary summary;
    bool found = false;
    for (const auto& row : std::get<QVector<ledger::RunSummary>>(summaries))
        if (row.runId == runId) { summary = row; found = true; break; }
    if (!found) return HistoryQueryError{QStringLiteral("RUN_NOT_FOUND"), QStringLiteral("selected run is not committed")};
    const auto events = store.eventsForRun(runId);
    HistoryQueryError error;
    if (fail(events, &error, QStringLiteral("eventsForRun"))) return error;
    const auto snapshot = store.snapshot(runId);
    if (fail(snapshot, &error, QStringLiteral("snapshot"))) return error;
    const auto audits = store.recoveryAudit(runId);
    if (fail(audits, &error, QStringLiteral("recoveryAudit"))) return error;
    return HistoryRunBundle{summary, std::get<QVector<ledger::EventDraft>>(events),
                            std::get<ledger::LedgerSnapshot>(snapshot),
                            std::get<QVector<ledger::RecoveryAuditRecord>>(audits)};
}

} // namespace relink::workspace
