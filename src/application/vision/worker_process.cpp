#include "worker_process.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace relink::vision {
namespace {
constexpr int kReadChunk = 16384;
constexpr int kPendingMessages = 16;
constexpr int kDrainTimeout = 1000;
bool failure(ProtocolError* error, const QString& code, const QString& message)
{
    if (error) *error = {code, message};
    return false;
}
}

WorkerProcess::WorkerProcess(QString sessionId, ProtocolLimits limits)
    : m_protocol(std::move(sessionId), WorkerRole::Coordinator, limits), m_framer(limits)
{
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(&m_process, &QProcess::started, [&] { m_started = true; });
    QObject::connect(&m_process, &QProcess::readyReadStandardOutput, [&] { drainOutputs(); });
    QObject::connect(&m_process, &QProcess::readyReadStandardError, [&] { drainOutputs(); });
#ifdef Q_OS_WIN
    m_process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
        arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        arguments->startupInfo->wShowWindow = SW_HIDE;
    });
#endif
}

WorkerProcess::~WorkerProcess()
{
    const auto result = stop();
    if (!result.exited && m_started)
        qWarning("WorkerProcess destructor: child exit unconfirmed; caller-owned leases require quarantine");
}

bool WorkerProcess::validateTimeout(int timeoutMs, ProtocolError* error) const
{
    return timeoutMs >= 0 && timeoutMs <= maximumPollMs
        ? true : failure(error, QStringLiteral("E_DEADLINE"), QStringLiteral("timeout outside 0..60000 ms"));
}

void WorkerProcess::latch(const QString& code, const QString& message)
{
    if (!m_error.isError()) m_error = {code, message.left(1024)};
}

void WorkerProcess::checkDeadline()
{
    if (m_requestDeadline >= 0 && m_clock.elapsed() >= m_requestDeadline)
        latch(QStringLiteral("E_DEADLINE"), QStringLiteral("worker request or release deadline elapsed"));
}

void WorkerProcess::updateDeadline(const WorkerEnvelope& envelope, bool outgoing)
{
    if (outgoing && envelope.kind == QStringLiteral("recognize")) {
        m_activeRequestId = envelope.body.value("request_id").toString();
        m_activeDemandId = envelope.body.value("demand_id").toString();
        m_drainDeadlineSet = false;
        m_requestDeadline = m_clock.elapsed() + envelope.body.value("relative_budget_ms").toInteger();
        return;
    }
    if (m_activeRequestId.isEmpty()) return;
    const bool activeResponse = !outgoing && envelope.body.value("request_id").toString() == m_activeRequestId;
    const bool activeCancel = outgoing && (envelope.kind == QStringLiteral("cancel") || envelope.kind == QStringLiteral("shutdown"))
        && ((envelope.body.value("target_kind").toString() == QStringLiteral("request")
                && envelope.body.value("target_id").toString() == m_activeRequestId)
            || (envelope.body.value("target_kind").toString() == QStringLiteral("demand")
                && envelope.body.value("target_id").toString() == m_activeDemandId)
            || envelope.kind == QStringLiteral("shutdown"));
    if (activeResponse && envelope.kind == QStringLiteral("release_frame")) {
        m_activeRequestId.clear();
        m_activeDemandId.clear();
        m_requestDeadline = -1;
        m_drainDeadlineSet = false;
    } else if (!m_drainDeadlineSet && (activeCancel || (activeResponse
                && (envelope.kind == QStringLiteral("result") || envelope.kind == QStringLiteral("error"))))) {
        m_drainDeadlineSet = true;
        m_requestDeadline = m_clock.elapsed() + kDrainTimeout;
    }
}

void WorkerProcess::drainOutputs()
{
    if (m_draining) return;
    m_draining = true;
    m_process.setReadChannel(QProcess::StandardError);
    for (int chunk = 0; chunk < 16 && m_process.bytesAvailable() > 0; ++chunk) {
        const QByteArray bytes = m_process.read(kReadChunk);
        m_stderrBytes = std::min(std::numeric_limits<qint64>::max() - bytes.size(), m_stderrBytes) + bytes.size();
        m_stderr += bytes;
        if (m_stderr.size() > stderrCapacity) m_stderr.remove(0, m_stderr.size() - stderrCapacity);
    }
    m_process.setReadChannel(QProcess::StandardOutput);
    for (int chunk = 0; chunk < 128 && m_process.bytesAvailable() > 0; ++chunk) {
        const QByteArray bytes = m_process.read(kReadChunk);
        if (m_error.isError()) continue; // Bounded discard after fault; never reinterpret a partial session.
        checkDeadline();
        if (m_error.isError()) continue;
        const auto batch = m_framer.feed(bytes);
        if (!batch.ok()) { latch(batch.error.code, batch.error.message); continue; }
        for (const auto& envelope : batch.messages) {
            ProtocolError error;
            if (!m_protocol.receive(envelope, &error)) {
                if (error.code == QStringLiteral("E_CANCELLED") && envelope.kind == QStringLiteral("result")) {
                    ++m_rejectedLateResults;
                    continue;
                }
                latch(error.code, error.message);
                break;
            }
            updateDeadline(envelope, false);
            if (m_pending.size() >= kPendingMessages) {
                latch(QStringLiteral("E_MESSAGE_TOO_LARGE"), QStringLiteral("worker output queue limit exceeded"));
                break;
            }
            m_pending.push_back(envelope);
        }
    }
    if (!running() && m_started && !m_eofFinished && m_process.bytesAvailable() == 0) {
        m_eofFinished = true;
        const auto end = m_framer.finish();
        if (!end.ok()) latch(end.error.code, end.error.message);
        else if (m_protocol.state() != SessionState::Closed && !m_error.isError())
            latch(QStringLiteral("E_WORKER_CRASH"), QStringLiteral("worker EOF before drained bye"));
    }
    m_draining = false;
}

bool WorkerProcess::start(const QString& absoluteProgram, const QStringList& arguments,
                          const QString& absoluteWorkingDirectory, const WorkerEnvelope& hello,
                          int handshakeTimeoutMs, ProtocolError* error)
{
    if (error) *error = {};
    if (m_startAttempted) return failure(error, QStringLiteral("E_MESSAGE_INVALID"), QStringLiteral("process session is single-use"));
    if (!validateTimeout(handshakeTimeoutMs, error) || handshakeTimeoutMs == 0)
        return failure(error, QStringLiteral("E_DEADLINE"), QStringLiteral("positive handshake timeout required"));
    const QFileInfo program(absoluteProgram), cwd(absoluteWorkingDirectory);
    if (!program.isAbsolute() || !program.isFile() || !program.isExecutable() || !cwd.isAbsolute() || !cwd.isDir())
        return failure(error, QStringLiteral("E_WORKER_START"), QStringLiteral("absolute existing executable and working directory required"));
    if (hello.kind != QStringLiteral("hello"))
        return failure(error, QStringLiteral("E_HANDSHAKE_REQUIRED"), QStringLiteral("start requires hello"));
    WorkerProtocol candidate = m_protocol;
    if (!candidate.send(hello, nullptr, error)) return false;
    m_startAttempted = true;
    m_clock.start();
    m_process.setProgram(program.absoluteFilePath());
    m_process.setArguments(arguments);
    m_process.setWorkingDirectory(cwd.absoluteFilePath());
    m_process.start(QIODevice::ReadWrite);
    if (!m_process.waitForStarted(handshakeTimeoutMs)) {
        latch(QStringLiteral("E_WORKER_START"), m_process.errorString());
        if (error) *error = m_error;
        stop();
        return false;
    }
    const int remaining = handshakeTimeoutMs - static_cast<int>(m_clock.elapsed());
    if (remaining <= 0 || !send(hello, remaining, error)) {
        if (!m_error.isError()) latch(QStringLiteral("E_DEADLINE"), QStringLiteral("handshake deadline elapsed"));
        if (error) *error = m_error;
        stop();
        return false;
    }
    while (m_protocol.state() != SessionState::Ready && !m_error.isError() && running()) {
        const int left = handshakeTimeoutMs - static_cast<int>(m_clock.elapsed());
        if (left <= 0) break;
        poll(left);
    }
    if (m_protocol.state() == SessionState::Ready && !m_error.isError()) {
        m_pending.clear(); // ready was consumed by this blocking handshake.
        return true;
    }
    if (!m_error.isError()) latch(QStringLiteral("E_DEADLINE"), QStringLiteral("worker handshake timed out"));
    if (error) *error = m_error;
    stop();
    return false;
}

bool WorkerProcess::send(const WorkerEnvelope& envelope, int writeTimeoutMs, ProtocolError* error)
{
    if (error) *error = {};
    if (!validateTimeout(writeTimeoutMs, error)) return false;
    checkDeadline();
    if (m_error.isError()) { if (error) *error = m_error; return false; }
    if (!running()) return failure(error, QStringLiteral("E_WORKER_CRASH"), QStringLiteral("worker is not running"));
    if (m_process.bytesToWrite() != 0)
        return failure(error, QStringLiteral("E_MESSAGE_TOO_LARGE"), QStringLiteral("previous protocol write is still pending"));
    WorkerProtocol candidate = m_protocol;
    QByteArray bytes;
    if (!candidate.send(envelope, &bytes, error)) return false;
    if (m_process.write(bytes) != bytes.size()) {
        latch(QStringLiteral("E_WORKER_CRASH"), QStringLiteral("worker protocol write failed"));
        if (error) *error = m_error;
        return false;
    }
    // Commit before pumping I/O: ready/result may arrive while stdin drains.
    m_protocol = std::move(candidate);
    updateDeadline(envelope, true);
    QElapsedTimer elapsed;
    elapsed.start();
    while (m_process.bytesToWrite() > 0) {
        const int left = writeTimeoutMs - static_cast<int>(elapsed.elapsed());
        if (left <= 0 || !m_process.waitForBytesWritten(left)) {
            latch(QStringLiteral("E_DEADLINE"), QStringLiteral("worker stdin write deadline elapsed"));
            if (error) *error = m_error;
            return false;
        }
        drainOutputs();
    }
    if (m_error.isError()) { if (error) *error = m_error; return false; }
    return true;
}

WorkerPoll WorkerProcess::poll(int timeoutMs)
{
    WorkerPoll result;
    if (!validateTimeout(timeoutMs, &result.error)) return result;
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        drainOutputs();
        checkDeadline();
        if (!m_pending.isEmpty() || m_error.isError() || !running()) break;
        int left = timeoutMs - static_cast<int>(elapsed.elapsed());
        if (left <= 0) break;
        if (m_requestDeadline >= 0) left = static_cast<int>(std::min<qint64>(left, std::max<qint64>(1, m_requestDeadline - m_clock.elapsed())));
        m_process.waitForReadyRead(left);
    } while (elapsed.elapsed() < timeoutMs);
    drainOutputs();
    checkDeadline();
    result.messages = std::move(m_pending);
    m_pending.clear();
    result.error = m_error;
    result.exited = exitConfirmed();
    if (result.exited) { result.exitCode = m_process.exitCode(); result.crashed = m_process.exitStatus() == QProcess::CrashExit; }
    result.timedOut = result.messages.isEmpty() && !result.error.isError() && !result.exited;
    return result;
}

WorkerExit WorkerProcess::exitResult(bool forced) const
{
    WorkerExit result;
    result.exited = exitConfirmed();
    result.forced = forced;
    result.error = m_error;
    if (result.exited) { result.exitCode = m_process.exitCode(); result.crashed = m_process.exitStatus() == QProcess::CrashExit; }
    return result;
}

WorkerExit WorkerProcess::waitForExit(int timeoutMs)
{
    ProtocolError error;
    if (!validateTimeout(timeoutMs, &error)) { auto result = exitResult(); result.error = error; return result; }
    checkDeadline();
    if (running()) m_process.waitForFinished(timeoutMs);
    drainOutputs();
    checkDeadline();
    auto result = exitResult();
    if (running() && !result.error.isError()) result.error = {QStringLiteral("E_DEADLINE"), QStringLiteral("worker exit unconfirmed at deadline")};
    return result;
}

WorkerExit WorkerProcess::stop(int terminateTimeoutMs, int killTimeoutMs)
{
    ProtocolError error;
    if (!validateTimeout(terminateTimeoutMs, &error) || !validateTimeout(killTimeoutMs, &error)) {
        auto result = exitResult(); result.error = error; return result;
    }
    bool forced = false;
    if (running()) {
        forced = true;
        m_process.terminate();
        m_process.waitForFinished(terminateTimeoutMs);
        if (running()) { m_process.kill(); m_process.waitForFinished(killTimeoutMs); }
    }
    drainOutputs();
    return exitResult(forced);
}

WorkerExit WorkerProcess::shutdown(const WorkerEnvelope& envelope, int timeoutMs)
{
    ProtocolError error;
    if (!validateTimeout(timeoutMs, &error)) { auto result = exitResult(); result.error = error; return result; }
    QElapsedTimer elapsed;
    elapsed.start();
    if (envelope.kind != QStringLiteral("shutdown") || !send(envelope, std::min(timeoutMs, 1000), &error)) {
        auto result = stop();
        if (!error.isError()) error = {QStringLiteral("E_MESSAGE_INVALID"), QStringLiteral("shutdown message required")};
        result.error = error;
        return result;
    }
    while (running() && !m_error.isError() && elapsed.elapsed() < timeoutMs) {
        poll(timeoutMs - static_cast<int>(elapsed.elapsed()));
    }
    auto result = waitForExit(std::max(0, timeoutMs - static_cast<int>(elapsed.elapsed())));
    if (!result.exited) {
        latch(QStringLiteral("E_DEADLINE"), QStringLiteral("worker shutdown deadline elapsed"));
        result = stop();
    }
    return result;
}

} // namespace relink::vision
