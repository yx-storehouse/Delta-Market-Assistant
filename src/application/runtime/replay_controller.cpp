#include "replay_controller.h"

namespace relink::runtime {
namespace {
RunSnapshot initialReplaySnapshot()
{
    RunSnapshot snapshot;
    snapshot.runId = QStringLiteral("replay-run-1");
    snapshot.sessionId = QStringLiteral("replay-session-1");
    snapshot.clockDomainId = QStringLiteral("replay-clock-1");
    snapshot.stepId = QStringLiteral("s0");
    snapshot.cancelEpoch = 0;
    snapshot.viewportGeneration = 1;
    snapshot.runState = QStringLiteral("Ready");
    snapshot.mode = QStringLiteral("Replay");
    snapshot.nowMonoMs = 0;
    snapshot.autoCollect = false;
    snapshot.targetQuantity = 1;
    return snapshot;
}
}

ReplayController::ReplayController(QObject* parent)
    : QObject(parent), m_coordinator(initialReplaySnapshot())
{
}

bool ReplayController::replayStarted() const
{
    const QString& state = m_coordinator.snapshot().runState;
    return state != QStringLiteral("Ready") && state != QStringLiteral("Stopped")
        && state != QStringLiteral("Completed");
}

ReplayEvent ReplayController::makeEvent(const QString& type, const QVariantMap& payload)
{
    ReplayEvent event;
    event.eventId = QStringLiteral("ui-replay-event-%1").arg(m_nextEventNumber++);
    m_nextMonoMs += 100;
    event.atMonoMs = m_nextMonoMs;
    event.context = ReplayReducer::contextOf(m_coordinator.snapshot());
    event.type = type;
    event.payload = payload;
    return event;
}

void ReplayController::submit(const QString& type, const QVariantMap& payload)
{
    const ReduceResult result = m_coordinator.submit(makeEvent(type, payload));
    m_lastEffects = result.effects;
    if (!result.accepted) {
        emit commandRejected(result.error.isEmpty() ? QStringLiteral("replay_command_rejected") : result.error);
        return;
    }
    emit changed();
}

void ReplayController::startReplay()
{
    if (m_coordinator.snapshot().runState != QStringLiteral("Ready")) {
        emit commandRejected(QStringLiteral("replay_not_ready"));
        return;
    }
    submit(QStringLiteral("Start"), {{QStringLiteral("configuration_reviewed"), true},
                                      {QStringLiteral("schedule_due"), true}});
}

void ReplayController::pauseReplay()
{
    if (!replayStarted() || m_coordinator.snapshot().runState == QStringLiteral("Paused")) {
        emit commandRejected(QStringLiteral("replay_not_running"));
        return;
    }
    submit(QStringLiteral("Pause"));
}

void ReplayController::resumeReplay()
{
    if (m_coordinator.snapshot().runState != QStringLiteral("Paused")) {
        emit commandRejected(QStringLiteral("replay_not_paused"));
        return;
    }
    submit(QStringLiteral("Resume"));
}

void ReplayController::stopReplay()
{
    const QString& state = m_coordinator.snapshot().runState;
    if (state == QStringLiteral("Ready") || state == QStringLiteral("Stopped")) {
        emit commandRejected(QStringLiteral("replay_not_started"));
        return;
    }
    submit(QStringLiteral("Stop"));
}

} // namespace relink::runtime
