#include "startup_observer.h"
#include <QJsonArray>

namespace relink::runtime {
namespace {
constexpr qint64 maxInteger = 9007199254740991LL;
bool id(const QString& value) { return !value.isEmpty() && value.size() <= 200; }
bool integer(qint64 value) { return value >= 0 && value <= maxInteger; }
bool same(const RuntimeContext& a, const RuntimeContext& b) {
    return a.runId == b.runId && a.sessionId == b.sessionId && a.clockDomainId == b.clockDomainId
        && a.stepId == b.stepId && a.cancelEpoch == b.cancelEpoch && a.viewportGeneration == b.viewportGeneration;
}
}
QString toString(StartupPhase phase) {
    switch (phase) {
    case StartupPhase::Idle: return QStringLiteral("Idle");
    case StartupPhase::Locating: return QStringLiteral("Locating");
    case StartupPhase::AwaitWatchlist: return QStringLiteral("AwaitWatchlist");
    case StartupPhase::AwaitHomeAfterEmpty: return QStringLiteral("AwaitHomeAfterEmpty");
    case StartupPhase::AwaitCatalogFilter: return QStringLiteral("AwaitCatalogFilter");
    case StartupPhase::FilterObserved: return QStringLiteral("FilterObserved");
    case StartupPhase::ExistingListObserved: return QStringLiteral("ExistingListObserved");
    case StartupPhase::Paused: return QStringLiteral("Paused");
    case StartupPhase::Stopped: return QStringLiteral("Stopped");
    }
    return QStringLiteral("Unknown");
}
QJsonObject StartupSnapshot::toJson() const {
    return {{"phase", toString(phase)}, {"checkpoint", checkpoint}, {"reason", reason},
        {"page", vision::toString(page)}, {"overlay", vision::toString(overlay)},
        {"observations", observationCount}, {"accepted", acceptedCount}, {"rejected", rejectedCount},
        {"empty_confirmations", emptyConfirmations}, {"empty_watchlist_verified", emptyWatchlistVerified},
        {"home_after_empty_verified", homeAfterEmptyVerified}, {"source_steps", QJsonArray::fromStringList(sourceSteps)},
        {"actions_enabled", false}, {"purchase_authorized", false}, {"capture_requested", false},
        {"business_fields_validated", false}};
}
bool StartupObserver::active() const {
    return m_snapshot.phase == StartupPhase::Locating || m_snapshot.phase == StartupPhase::AwaitWatchlist
        || m_snapshot.phase == StartupPhase::AwaitHomeAfterEmpty || m_snapshot.phase == StartupPhase::AwaitCatalogFilter;
}
bool StartupObserver::start(const RuntimeContext& context, qint64 now, qint64 timeout, int budget, int confirmations) {
    if (active()) { m_snapshot.reason = QStringLiteral("E_STARTUP_BUSY"); return false; }
    if (!id(context.runId) || !id(context.sessionId) || !id(context.clockDomainId) || !id(context.stepId)
        || !integer(context.cancelEpoch) || !integer(context.viewportGeneration) || !integer(now)
        || timeout < 1 || timeout > 60000 || now > maxInteger - timeout || budget < 1 || budget > 128
        || confirmations < 1 || confirmations > 5 || confirmations > budget) {
        m_snapshot.reason = QStringLiteral("E_STARTUP_ARGUMENTS"); return false;
    }
    if (m_hasContext && same(context, m_context)) { m_snapshot.reason = QStringLiteral("E_STARTUP_CONTEXT_REUSE"); return false; }
    m_context = context; m_hasContext = true;
    m_snapshot = {}; m_snapshot.phase = StartupPhase::Locating;
    m_snapshot.checkpoint = QStringLiteral("identify_current_page"); m_snapshot.sourceSteps = {QStringLiteral("S04")};
    m_started = m_lastNow = now; m_deadline = now + timeout; m_lastCapture = -1;
    m_budget = budget; m_requiredEmpty = confirmations; m_frameIds.clear();
    return true;
}
void StartupObserver::discardPrecheck() {
    m_snapshot.emptyConfirmations = 0;
    m_snapshot.emptyWatchlistVerified = false;
    m_snapshot.homeAfterEmptyVerified = false;
}
bool StartupObserver::reject(const QString& reason) {
    ++m_snapshot.rejectedCount; m_snapshot.reason = reason;
    if (m_snapshot.observationCount >= m_budget) {
        m_snapshot.phase = StartupPhase::Paused;
        m_snapshot.checkpoint = QStringLiteral("observation_budget_exhausted");
        discardPrecheck();
    }
    return false;
}
bool StartupObserver::advanceClock(qint64 now) {
    if (!active()) return false;
    if (!integer(now) || now < m_lastNow) {
        m_snapshot.phase = StartupPhase::Paused; m_snapshot.reason = QStringLiteral("E_STARTUP_CLOCK");
        m_snapshot.checkpoint = QStringLiteral("restart_observation_context"); discardPrecheck(); return false;
    }
    m_lastNow = now;
    if (now >= m_deadline) {
        m_snapshot.phase = StartupPhase::Paused; m_snapshot.reason = QStringLiteral("E_STARTUP_DEADLINE");
        m_snapshot.checkpoint = QStringLiteral("observation_deadline_exhausted"); discardPrecheck(); return false;
    }
    return true;
}
bool StartupObserver::tick(qint64 now) { return advanceClock(now); }
bool StartupObserver::observe(const StartupPageObservation& observation, qint64 now) {
    if (!advanceClock(now)) return false;
    ++m_snapshot.observationCount;
    if (!same(observation.context, m_context)) return reject(QStringLiteral("E_STARTUP_CONTEXT"));
    if (!id(observation.frameId)) return reject(QStringLiteral("E_STARTUP_FRAME_ID"));
    if (m_frameIds.contains(observation.frameId)) return reject(QStringLiteral("E_STARTUP_DUPLICATE_FRAME"));
    // Even two different IDs cannot confirm a reused/cached presentation.
    if (!integer(observation.captureLowerBoundMonoMs) || !integer(observation.captureEndMonoMs)
        || observation.captureLowerBoundMonoMs < m_started || observation.captureLowerBoundMonoMs <= m_lastCapture
        || observation.captureEndMonoMs < observation.captureLowerBoundMonoMs || observation.captureEndMonoMs > now
        || now - observation.captureLowerBoundMonoMs > 1000) return reject(QStringLiteral("E_STARTUP_STALE_FRAME"));
    m_frameIds.insert(observation.frameId);
    m_lastCapture = observation.captureLowerBoundMonoMs;
    const auto page = vision::classifySkinPage(observation.ocr);
    m_snapshot.page = page.page; m_snapshot.overlay = page.overlay;
    if (!page.validInput) { m_snapshot.emptyConfirmations = 0; return reject(page.reason); }
    ++m_snapshot.acceptedCount;
    m_snapshot.reason = page.reason;
    if (page.page != vision::SkinPage::EmptyWatchlist) m_snapshot.emptyConfirmations = 0;
    if (page.overlay != vision::PageOverlay::None) {
        discardPrecheck();
        m_snapshot.checkpoint = QStringLiteral("recheck_after_overlay");
        m_snapshot.sourceSteps = {QStringLiteral("S04"), QStringLiteral("S32")};
    } else switch (page.page) {
    case vision::SkinPage::SkinHome:
        if (m_snapshot.emptyWatchlistVerified) {
            m_snapshot.homeAfterEmptyVerified = true;
            m_snapshot.phase = StartupPhase::AwaitCatalogFilter;
            m_snapshot.checkpoint = QStringLiteral("catalog_filter");
            m_snapshot.sourceSteps = {QStringLiteral("S09"), QStringLiteral("S10")};
        } else {
            m_snapshot.phase = StartupPhase::AwaitWatchlist;
            m_snapshot.checkpoint = QStringLiteral("watchlist_precheck");
            m_snapshot.sourceSteps = {QStringLiteral("S07"), QStringLiteral("S08")};
        }
        break;
    case vision::SkinPage::EmptyWatchlist:
        ++m_snapshot.emptyConfirmations;
        if (m_snapshot.emptyConfirmations >= m_requiredEmpty) {
            m_snapshot.emptyWatchlistVerified = true;
            m_snapshot.phase = StartupPhase::AwaitHomeAfterEmpty;
            m_snapshot.checkpoint = QStringLiteral("skin_home_after_empty");
            m_snapshot.sourceSteps = {QStringLiteral("S08"), QStringLiteral("S09")};
        } else {
            m_snapshot.phase = StartupPhase::AwaitWatchlist;
            m_snapshot.checkpoint = QStringLiteral("confirm_empty_watchlist");
            m_snapshot.sourceSteps = {QStringLiteral("S08")};
        }
        break;
    case vision::SkinPage::CatalogFilter:
        if (m_snapshot.emptyWatchlistVerified && m_snapshot.homeAfterEmptyVerified) {
            m_snapshot.phase = StartupPhase::FilterObserved;
            m_snapshot.checkpoint = QStringLiteral("read_season_ownership_quality");
            m_snapshot.sourceSteps = {QStringLiteral("S10"), QStringLiteral("S12"), QStringLiteral("S13")};
        } else {
            m_snapshot.checkpoint = QStringLiteral("watchlist_precheck_unproven");
            m_snapshot.reason = QStringLiteral("E_STARTUP_PRECHECK_REQUIRED");
            m_snapshot.sourceSteps = {QStringLiteral("S04"), QStringLiteral("S07"), QStringLiteral("S08")};
        }
        break;
    case vision::SkinPage::SkinListings:
    case vision::SkinPage::WatchlistListings:
        m_snapshot.phase = StartupPhase::ExistingListObserved;
        m_snapshot.checkpoint = QStringLiteral("read_existing_list_sort_and_identity");
        m_snapshot.sourceSteps = {QStringLiteral("S32"), QStringLiteral("S33")};
        // No new collection/reset instruction: continue the ORIGINAL branch.
        break;
    case vision::SkinPage::Mandel:
        m_snapshot.checkpoint = QStringLiteral("skin_home");
        m_snapshot.sourceSteps = {QStringLiteral("S06")}; break;
    case vision::SkinPage::Lobby:
    case vision::SkinPage::Warehouse:
    case vision::SkinPage::GameSettings:
    case vision::SkinPage::Base:
        m_snapshot.checkpoint = QStringLiteral("entry_recovery_requires_observation");
        m_snapshot.sourceSteps = {QStringLiteral("S04"), QStringLiteral("S05")}; break;
    case vision::SkinPage::Unknown:
        m_snapshot.checkpoint = QStringLiteral("reobserve_unknown_page");
        m_snapshot.sourceSteps = {QStringLiteral("S04")}; break;
    }
    if (active() && m_snapshot.observationCount >= m_budget) {
        m_snapshot.phase = StartupPhase::Paused; m_snapshot.reason = QStringLiteral("E_STARTUP_BUDGET");
        m_snapshot.checkpoint = QStringLiteral("observation_budget_exhausted"); discardPrecheck();
    }
    return true;
}
void StartupObserver::pause() {
    if (!m_hasContext) return;
    m_snapshot.phase = StartupPhase::Paused; m_snapshot.reason = QStringLiteral("USER_PAUSED");
    m_snapshot.checkpoint.clear(); discardPrecheck();
}
void StartupObserver::stop() {
    if (!m_hasContext) return;
    m_snapshot.phase = StartupPhase::Stopped; m_snapshot.reason = QStringLiteral("USER_STOPPED");
    m_snapshot.checkpoint.clear(); discardPrecheck();
}
} // namespace relink::runtime
