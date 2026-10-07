#include "application/vision/lease_pool.h"
#include "application/vision/shared_frame_memory.h"
#include "application/vision/worker_process.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUuid>

#include <iostream>

using namespace relink::vision;

namespace {
int assertions = 0;
int failures = 0;
void check(bool condition, const char* label)
{
    ++assertions;
    if (!condition) { ++failures; std::cerr << "FAIL " << label << '\n'; }
}
#define CHECK(expression) check((expression), #expression)

class FixtureSession final {
public:
    FixtureSession(QString executable, QDir messages)
        : program(std::move(executable)), fixtures(std::move(messages)),
          session(QStringLiteral("process-") + QUuid::createUuid().toString(QUuid::WithoutBraces)),
          pool(20000), process(session)
    {
        CHECK(directory.isValid());
        QString error;
        CHECK(first.create(session, 0, 20000, &error));
        CHECK(second.create(session, 1, 20000, &error));
        pixels.resize(20000);
        for (int i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<char>((i * 11 + 7) % 251);
    }

    WorkerEnvelope message(const char* name)
    {
        QFile file(fixtures.filePath(QString::fromLatin1(name)));
        CHECK(file.open(QIODevice::ReadOnly));
        const auto object = QJsonDocument::fromJson(file.readAll()).object();
        CHECK(!object.isEmpty());
        return {1, session, QStringLiteral("process-message-%1").arg(++messageNumber),
                object.value("kind").toString(), object.value("body").toObject()};
    }

    WorkerEnvelope hello()
    {
        auto hello = message("01_hello.valid.json");
        hello.body.insert("memory_pool", QJsonArray{first.poolEntry(), second.poolEntry()});
        return hello;
    }

    bool start(const QString& mode, int timeout = 3000, ProtocolError* error = nullptr)
    {
        return process.start(program, {QStringLiteral("--synthetic-fixture"), QStringLiteral("--mode"), mode},
                             directory.path(), hello(), timeout, error);
    }

    WorkerEnvelope recognize(int budgetMs = 3000)
    {
        const auto allocated = pool.beginWrite(pixels.size());
        CHECK(allocated.ok);
        lease = allocated.lease;
        CHECK(first.write(pixels));
        CHECK(pool.publish(lease) && pool.acquireForWorker(lease));
        auto request = message("03_recognize.valid.json");
        auto context = request.body.value("context").toObject();
        context.insert("session_id", session);
        request.body.insert("context", context);
        auto frame = request.body.value("frame").toObject();
        frame.insert("context", context);
        frame.insert("capture_session_id", session);
        frame.insert("descriptor", first.descriptor(lease));
        request.body.insert("frame", frame);
        request.body.insert("relative_budget_ms", budgetMs);
        return request;
    }

    QVector<WorkerEnvelope> untilReleased(int timeoutMs = 3000)
    {
        QVector<WorkerEnvelope> messages;
        QElapsedTimer timer;
        timer.start();
        while (!process.protocol().requestReleased(QStringLiteral("request-1")) && timer.elapsed() < timeoutMs) {
            const auto reply = process.poll(timeoutMs - static_cast<int>(timer.elapsed()));
            messages += reply.messages;
            if (reply.error.isError()) {
                std::cerr << "poll " << reply.error.code.toStdString() << ' ' << reply.error.message.toStdString() << '\n';
                CHECK(!reply.error.isError());
                break;
            }
            if (reply.exited) break;
        }
        CHECK(process.protocol().requestReleased(QStringLiteral("request-1")));
        return messages;
    }

    void shutdown()
    {
        const auto result = process.shutdown(message("09_shutdown.valid.json"), 3000);
        if (result.error.isError()) std::cerr << "shutdown " << result.error.code.toStdString() << ' ' << result.error.message.toStdString() << '\n';
        CHECK(result.exited && result.exitCode == 0 && !result.crashed && !result.forced);
        CHECK(!result.error.isError());
        CHECK(process.protocol().state() == SessionState::Closed);
        CHECK(!process.running() && process.exitConfirmed() && !process.quarantineNeeded());
        CHECK(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }

    QString program;
    QDir fixtures;
    QString session;
    QTemporaryDir directory;
    SharedFrameMemory first;
    SharedFrameMemory second;
    LeasePool pool;
    WorkerProcess process;
    QByteArray pixels;
    LeaseHandle lease;
    int messageNumber = 0;
};

void roundTrip(const QString& program, const QDir& messages)
{
    FixtureSession fixture(program, messages);
    CHECK(fixture.start(QStringLiteral("normal")));
    CHECK(fixture.process.running() && fixture.process.protocol().state() == SessionState::Ready);
    CHECK(fixture.process.send(fixture.recognize()));
    const auto replies = fixture.untilReleased();
    bool foundResult = false, foundRelease = false;
    const QString expectedHash = QString::fromLatin1(QCryptographicHash::hash(fixture.pixels, QCryptographicHash::Sha256).toHex());
    for (const auto& reply : replies) {
        if (reply.kind == QStringLiteral("result")) {
            foundResult = true;
            CHECK(reply.body.value("tokens").toArray().first().toObject().value("text").toString() == expectedHash);
            CHECK(reply.body.value("lease_id").toString() == fixture.lease.leaseId);
            CHECK(reply.body.value("context").toObject().value("session_id").toString() == fixture.session);
        }
        if (reply.kind == QStringLiteral("release_frame")) {
            foundRelease = true;
            CHECK(reply.body.value("slot_generation").toInteger() == fixture.lease.generation);
        }
    }
    CHECK(foundResult && foundRelease);
    CHECK(fixture.process.protocol().inFlightCount() == 0);
    CHECK(fixture.pool.release(fixture.lease) && fixture.pool.freeCount() == 2);
    fixture.shutdown();
    ProtocolError error;
    CHECK(!fixture.start(QStringLiteral("normal"), 3000, &error) && error.code == QStringLiteral("E_MESSAGE_INVALID"));
}

void cancellation(const QString& program, const QDir& messages)
{
    FixtureSession fixture(program, messages);
    CHECK(fixture.start(QStringLiteral("cancel")));
    CHECK(fixture.process.send(fixture.recognize()));
    CHECK(fixture.pool.cancel(fixture.lease));
    CHECK(fixture.pool.current(fixture.lease.slotIndex).state == LeaseState::AwaitRelease);
    CHECK(fixture.process.send(fixture.message("06_cancel.valid.json")));
    const auto replies = fixture.untilReleased();
    bool acknowledged = false;
    for (const auto& reply : replies) {
        CHECK(reply.kind != QStringLiteral("result"));
        if (reply.kind == QStringLiteral("cancelled")) acknowledged = true;
    }
    CHECK(acknowledged);
    CHECK(fixture.process.rejectedLateResults() == 1);
    CHECK(!fixture.process.protocol().requestTerminal(QStringLiteral("request-1")));
    CHECK(fixture.pool.freeCount() == 1); // Protocol acknowledgement never frees caller-owned mappings.
    CHECK(fixture.pool.release(fixture.lease) && fixture.pool.freeCount() == 2);
    fixture.shutdown();
}

void invalidLaunch(const QString& program, const QDir& messages)
{
    FixtureSession fixture(program, messages);
    ProtocolError error;
    CHECK(!fixture.process.start(QFileInfo(program).fileName(), {}, fixture.directory.path(), fixture.hello(), 100, &error));
    CHECK(error.code == QStringLiteral("E_WORKER_START") && !fixture.process.running());
    CHECK(!fixture.process.start(fixture.directory.filePath(QStringLiteral("missing.exe")), {}, fixture.directory.path(), fixture.hello(), 100, &error));
    CHECK(error.code == QStringLiteral("E_WORKER_START"));
    CHECK(!fixture.process.start(program, {}, QStringLiteral("."), fixture.hello(), 100, &error));
    CHECK(error.code == QStringLiteral("E_WORKER_START"));
    CHECK(!fixture.process.start(program, {}, fixture.directory.path(), fixture.hello(), 0, &error));
    CHECK(error.code == QStringLiteral("E_DEADLINE"));
    CHECK(!fixture.process.start(program, {}, fixture.directory.path(), fixture.hello(), 1000, &error));
    CHECK(fixture.process.exitConfirmed() && !fixture.process.running());
    CHECK(fixture.process.waitForExit(0).exitCode == 64); // Fixture refuses to run without its explicit marker.
}

void handshakeFailures(const QString& program, const QDir& messages)
{
    for (const QString& mode : {QStringLiteral("stall"), QStringLiteral("malformed"), QStringLiteral("partial"),
                               QStringLiteral("eof"), QStringLiteral("stdout_flood")}) {
        FixtureSession fixture(program, messages);
        ProtocolError error;
        QElapsedTimer elapsed;
        elapsed.start();
        CHECK(!fixture.start(mode, mode == QStringLiteral("stall") ? 150 : 2000, &error));
        CHECK(error.isError());
        CHECK(elapsed.elapsed() < 5000);
        CHECK(fixture.process.exitConfirmed() && !fixture.process.running());
        CHECK(fixture.process.stderrTail().size() <= WorkerProcess::stderrCapacity);
        if (mode == QStringLiteral("stall")) CHECK(error.code == QStringLiteral("E_DEADLINE"));
        if (mode == QStringLiteral("eof")) CHECK(error.code == QStringLiteral("E_WORKER_CRASH"));
        if (mode == QStringLiteral("stdout_flood")) CHECK(error.code == QStringLiteral("E_MESSAGE_TOO_LARGE"));
        if (mode == QStringLiteral("malformed") || mode == QStringLiteral("partial")) CHECK(error.code == QStringLiteral("E_MESSAGE_INVALID"));
        CHECK(QDir(fixture.directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
}

void stderrBound(const QString& program, const QDir& messages)
{
    FixtureSession fixture(program, messages);
    CHECK(fixture.start(QStringLiteral("stderr")));
    QElapsedTimer elapsed;
    elapsed.start();
    while (fixture.process.stderrBytesRead() < 262144 && elapsed.elapsed() < 2000) {
        const auto polled = fixture.process.poll(50);
        if (polled.error.isError() || polled.exited) break;
    }
    CHECK(fixture.process.stderrBytesRead() == 262144);
    CHECK(fixture.process.stderrTail().size() == WorkerProcess::stderrCapacity);
    CHECK(fixture.process.stderrTail() == QByteArray(WorkerProcess::stderrCapacity, 'e'));
    fixture.shutdown();
}

void requestDeadline(const QString& program, const QDir& messages)
{
    FixtureSession fixture(program, messages);
    CHECK(fixture.start(QStringLiteral("request_stall")));
    CHECK(fixture.process.send(fixture.recognize(100)));
    const auto reply = fixture.process.poll(1500);
    CHECK(reply.error.code == QStringLiteral("E_DEADLINE"));
    CHECK(reply.messages.isEmpty() && !reply.exited);
    CHECK(fixture.process.quarantineNeeded());
    CHECK(fixture.pool.workerUnresponsive(fixture.lease));
    CHECK(!fixture.pool.beginWrite(64).ok);
    const auto stopped = fixture.process.stop(50, 1000);
    CHECK(stopped.exited && stopped.forced && fixture.process.exitConfirmed());
    CHECK(!fixture.process.quarantineNeeded());
    CHECK(fixture.pool.processExitConfirmed(fixture.lease));
    fixture.first.close();
    CHECK(!fixture.first.isOpen());
    CHECK(fixture.pool.closeMapping(fixture.lease) && fixture.pool.freeCount() == 2);
    CHECK(!fixture.process.send(fixture.message("06_cancel.valid.json")));
}

void unrelatedMessageDoesNotExtendDeadline(const QString& program, const QDir& messages)
{
    FixtureSession fixture(program, messages);
    CHECK(fixture.start(QStringLiteral("session_error_stall")));
    QElapsedTimer elapsed;
    elapsed.start();
    CHECK(fixture.process.send(fixture.recognize(200)));
    auto result = fixture.process.poll(500);
    CHECK(!result.error.isError() && result.messages.size() == 1);
    CHECK(!result.messages.isEmpty() && result.messages.first().kind == QStringLiteral("error")
        && result.messages.first().body.value("request_id").isNull());
    result = fixture.process.poll(500);
    CHECK(result.error.code == QStringLiteral("E_DEADLINE"));
    CHECK(elapsed.elapsed() < 800);
    CHECK(fixture.process.stop(50, 1000).exited);
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 3) { std::cerr << "expected absolute fixture executable and message directory\n"; return 64; }
    const QString program = QString::fromLocal8Bit(argv[1]);
    const QDir messages(QString::fromLocal8Bit(argv[2]));
    CHECK(QFileInfo(program).isAbsolute() && QFileInfo(program).isFile());
    roundTrip(program, messages);
    cancellation(program, messages);
    invalidLaunch(program, messages);
    handshakeFailures(program, messages);
    stderrBound(program, messages);
    requestDeadline(program, messages);
    unrelatedMessageDoesNotExtendDeadline(program, messages);
    std::cout << "WORKER_PROCESS_TESTS=" << (failures == 0 ? "PASS" : "FAIL")
              << "; assertions=" << assertions << "; failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
