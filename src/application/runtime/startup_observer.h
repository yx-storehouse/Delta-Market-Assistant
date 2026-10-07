#pragma once
#include "runtime.h"
#include "application/vision/skin_page_classifier.h"
#include <QJsonObject>
#include <QSet>

namespace relink::runtime {
enum class StartupPhase { Idle, Locating, AwaitWatchlist, AwaitHomeAfterEmpty, AwaitCatalogFilter,
                          FilterObserved, ExistingListObserved, Paused, Stopped };
QString toString(StartupPhase phase);

struct StartupPageObservation {
    QString frameId;
    RuntimeContext context;
    qint64 captureLowerBoundMonoMs = -1;
    qint64 captureEndMonoMs = -1;
    QJsonObject ocr;
};
struct StartupSnapshot {
    StartupPhase phase = StartupPhase::Idle;
    QString checkpoint;
    QString reason;
    vision::SkinPage page = vision::SkinPage::Unknown;
    vision::PageOverlay overlay = vision::PageOverlay::None;
    int observationCount = 0;
    int acceptedCount = 0;
    int rejectedCount = 0;
    int emptyConfirmations = 0;
    bool emptyWatchlistVerified = false;
    bool homeAfterEmptyVerified = false;
    QStringList sourceSteps;
    QJsonObject toJson() const;
};

// Read-only reconstruction of the ORIGINAL startup dispatch/precheck order.
// Does not click, acquire frames, set timers or mutate a trading ledger.
// Two distinct empty frames are a configurable observation confirmation policy,
// not a claim that the protected original used a universal two-frame constant.
class StartupObserver final {
public:
    bool start(const RuntimeContext& context, qint64 nowMonoMs, qint64 timeoutMs = 10000,
               int frameBudget = 32, int emptyConfirmations = 2);
    bool observe(const StartupPageObservation& observation, qint64 nowMonoMs);
    bool tick(qint64 nowMonoMs);
    void pause();
    void stop();
    bool active() const;
    const StartupSnapshot& snapshot() const { return m_snapshot; }
    int retainedFrameIds() const { return m_frameIds.size(); }
private:
    bool advanceClock(qint64 nowMonoMs);
    bool reject(const QString& reason);
    void discardPrecheck();
    RuntimeContext m_context;
    StartupSnapshot m_snapshot;
    QSet<QString> m_frameIds;
    qint64 m_started = -1;
    qint64 m_deadline = -1;
    qint64 m_lastNow = -1;
    qint64 m_lastCapture = -1;
    int m_budget = 0;
    int m_requiredEmpty = 2;
    bool m_hasContext = false;
};
} // namespace relink::runtime
