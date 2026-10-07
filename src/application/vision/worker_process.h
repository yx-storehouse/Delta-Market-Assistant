#pragma once

#include "worker_protocol.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QStringList>

namespace relink::vision {

struct WorkerPoll {
    QVector<WorkerEnvelope> messages;
    ProtocolError error;
    bool timedOut = false;
    bool exited = false;
    int exitCode = -1;
    bool crashed = false;
};

struct WorkerExit {
    bool exited = false;
    int exitCode = -1;
    bool crashed = false;
    bool forced = false;
    ProtocolError error;
};

// Explicit-program, bounded NDJSON child transport. This class neither chooses
// an OCR provider nor launches capture/input tools. The caller retains lease
// ownership until release_frame or confirmed process exit + mapping closure.
class WorkerProcess final {
public:
    explicit WorkerProcess(QString sessionId, ProtocolLimits limits = {});
    ~WorkerProcess();
    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;

    bool start(const QString& absoluteProgram, const QStringList& arguments,
               const QString& absoluteWorkingDirectory, const WorkerEnvelope& hello,
               int handshakeTimeoutMs, ProtocolError* error = nullptr);
    bool send(const WorkerEnvelope& envelope, int writeTimeoutMs = 1000,
              ProtocolError* error = nullptr);
    WorkerPoll poll(int timeoutMs);
    WorkerExit shutdown(const WorkerEnvelope& envelope, int timeoutMs);
    WorkerExit waitForExit(int timeoutMs);
    WorkerExit stop(int terminateTimeoutMs = 250, int killTimeoutMs = 1000);

    bool running() const { return m_process.state() != QProcess::NotRunning; }
    bool exitConfirmed() const { return m_started && !running(); }
    bool quarantineNeeded() const { return m_protocol.inFlightCount() != 0 && !exitConfirmed(); }
    const WorkerProtocol& protocol() const { return m_protocol; }
    QByteArray stderrTail() const { return m_stderr; }
    qint64 stderrBytesRead() const { return m_stderrBytes; }
    int rejectedLateResults() const { return m_rejectedLateResults; }
    ProtocolError lastError() const { return m_error; }
    static constexpr int stderrCapacity = 65536;
    static constexpr int maximumPollMs = 60000;

private:
    void drainOutputs();
    void latch(const QString& code, const QString& message);
    bool validateTimeout(int timeoutMs, ProtocolError* error) const;
    void updateDeadline(const WorkerEnvelope& envelope, bool outgoing);
    void checkDeadline();
    WorkerExit exitResult(bool forced = false) const;

    QProcess m_process;
    WorkerProtocol m_protocol;
    NdjsonFramer m_framer;
    QElapsedTimer m_clock;
    QVector<WorkerEnvelope> m_pending;
    QByteArray m_stderr;
    ProtocolError m_error;
    bool m_startAttempted = false;
    bool m_started = false;
    bool m_draining = false;
    bool m_eofFinished = false;
    qint64 m_stderrBytes = 0;
    qint64 m_requestDeadline = -1;
    QString m_activeRequestId;
    QString m_activeDemandId;
    bool m_drainDeadlineSet = false;
    int m_rejectedLateResults = 0;
};

} // namespace relink::vision
