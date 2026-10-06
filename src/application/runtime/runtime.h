#pragma once

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

namespace relink::runtime {

struct RuntimeContext {
    QString runId;
    QString sessionId;
    QString clockDomainId;
    QString stepId;
    qint64 cancelEpoch = 0;
    qint64 viewportGeneration = 0;
};

struct ReplayEvent {
    QString eventId;
    qint64 atMonoMs = 0;
    RuntimeContext context;
    QString type;
    QVariantMap payload;
    qint64 seq = 0;

    static ReplayEvent fromJson(const QJsonObject& object, QString* error = nullptr);
};

struct AttemptSnapshot {
    QString attemptId;
    QString intentId;
    QString runId;
    QString ruleTaskId;
    int ruleRevision = 0;
    QString kind = QStringLiteral("purchase");
    QString state = QStringLiteral("Prepared");
    int quantity = 1;
    bool reservationHeld = false;
    QString dispatchProof = QStringLiteral("not_dispatched");
    QString receiptIdentity;
};

struct RunSnapshot {
    QString runId;
    QString sessionId;
    QString clockDomainId;
    QString stepId;
    qint64 cancelEpoch = 0;
    qint64 viewportGeneration = 0;
    QString runState = QStringLiteral("Ready");
    QString mode = QStringLiteral("Replay");
    qint64 nowMonoMs = 0;
    bool autoCollect = false;
    int targetQuantity = 1;
    int confirmedSuccess = 0;
    int unresolvedReservations = 0;
    QVector<AttemptSnapshot> attempts;

    // M1 replay observability counters. imageFileWriteCount is intentionally never
    // incremented by this reducer; replay has no file transport.
    int demandCount = 0;
    int captureRequestCount = 0;
    int imageFileWriteCount = 0;
    int ignoredEventCount = 0;
    QStringList emittedEvents;

    qint64 nextSeq = 1;
    QSet<QString> seenEventIds;
    QSet<QString> committedTransactions;
    QHash<QString, QString> pendingTransactions;
};

struct RuntimeEffect {
    QString type;
    QVariantMap payload;
};

struct ReduceResult {
    RunSnapshot snapshot;
    QVector<RuntimeEffect> effects;
    bool accepted = true;
    bool ignored = false;
    QString error;
};

class FakeClock {
public:
    explicit FakeClock(qint64 initialMs = 0) : m_nowMs(initialMs) {}
    qint64 nowMs() const { return m_nowMs; }
    void setNow(qint64 value) { m_nowMs = value; }
    void advanceBy(qint64 delta) { m_nowMs += delta; }
    void advanceTo(qint64 value) { if (value > m_nowMs) m_nowMs = value; }
private:
    qint64 m_nowMs = 0;
};

class ReplayReducer {
public:
    static ReduceResult reduce(const RunSnapshot& snapshot, const ReplayEvent& event);
    static RuntimeContext contextOf(const RunSnapshot& snapshot);
};

class RunCoordinator {
public:
    explicit RunCoordinator(RunSnapshot initial = {});

    ReduceResult submit(ReplayEvent event);
    ReduceResult submitJson(const QJsonObject& object, QString* error = nullptr);
    QVector<ReduceResult> replay(const QVector<ReplayEvent>& events);

    const RunSnapshot& snapshot() const { return m_snapshot; }
    RunSnapshot& snapshot() { return m_snapshot; }
    FakeClock& clock() { return m_clock; }
    const FakeClock& clock() const { return m_clock; }

private:
    RunSnapshot m_snapshot;
    FakeClock m_clock;
};

QString runStateName(const RunSnapshot& snapshot);

} // namespace relink::runtime
