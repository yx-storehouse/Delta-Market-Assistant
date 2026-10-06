#pragma once

#include "runtime.h"

#include <QObject>

namespace relink::runtime {

// UI-facing command adapter for the M1 replay branch. It only emits synthetic
// runtime events; no capture, OCR, input, or external process is started.
class ReplayController final : public QObject {
    Q_OBJECT
public:
    explicit ReplayController(QObject* parent = nullptr);

    const RunSnapshot& snapshot() const { return m_coordinator.snapshot(); }
    const QVector<RuntimeEffect>& lastEffects() const { return m_lastEffects; }
    bool replayStarted() const;

public slots:
    void startReplay();
    void pauseReplay();
    void resumeReplay();
    void stopReplay();

signals:
    void changed();
    void commandRejected(const QString& reason);

private:
    void submit(const QString& type, const QVariantMap& payload = {});
    ReplayEvent makeEvent(const QString& type, const QVariantMap& payload);

    RunCoordinator m_coordinator;
    QVector<RuntimeEffect> m_lastEffects;
    qint64 m_nextEventNumber = 1;
    qint64 m_nextMonoMs = 0;
};

} // namespace relink::runtime
