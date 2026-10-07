#include "application/vision/shared_frame_memory.h"
#include "application/vision/worker_protocol.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QThread>

#include <cstdio>
#include <memory>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace relink::vision;

namespace {
QByteArray readStdin()
{
    QByteArray bytes(16384, '\0');
#ifdef Q_OS_WIN
    DWORD count = 0;
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) return {};
    bytes.resize(count);
#else
    const auto count = ::read(STDIN_FILENO, bytes.data(), bytes.size());
    if (count <= 0) return {};
    bytes.resize(count);
#endif
    return bytes;
}

bool writeStdout(const QByteArray& bytes)
{
    return std::fwrite(bytes.constData(), 1, bytes.size(), stdout) == static_cast<size_t>(bytes.size())
        && std::fflush(stdout) == 0;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    const auto args = app.arguments();
    if (args.size() != 4 || args.at(1) != QStringLiteral("--synthetic-fixture") || args.at(2) != QStringLiteral("--mode")) return 64;
    const QString mode = args.at(3);
    if (!QStringList{"normal", "cancel", "stall", "request_stall", "malformed", "partial", "eof", "stderr", "stdout_flood", "session_error_stall"}.contains(mode)) return 64;
    NdjsonFramer framer;
    std::unique_ptr<WorkerProtocol> protocol;
    QJsonArray pool;
    WorkerEnvelope active;
    ReadOnlyFrameMemory reader;
    QByteArray activeBytes;
    int messageNumber = 0;
    auto message = [&](const QString& kind, const QJsonObject& body) {
        return WorkerEnvelope{1, protocol->sessionId(), QStringLiteral("fixture-response-%1").arg(++messageNumber), kind, body};
    };
    auto send = [&](const WorkerEnvelope& envelope) {
        QByteArray wire;
        return protocol->send(envelope, &wire) && writeStdout(wire);
    };
    auto result = [&] {
        const auto frame = active.body.value("frame").toObject();
        const auto descriptor = frame.value("descriptor").toObject();
        const auto roi = active.body.value("roi_specs").toArray().first().toObject();
        const QByteArray hash = QCryptographicHash::hash(activeBytes, QCryptographicHash::Sha256).toHex();
        const QJsonArray box{QJsonArray{0, 0}, QJsonArray{1, 0},
                             QJsonArray{1, 1}, QJsonArray{0, 1}};
        const QJsonObject token{{"roi_id", roi.value("roi_id")},
                                {"text", QString::fromLatin1(hash)},
                                {"score", 1.0}, {"box_px", box}};
        const QJsonObject body{{"request_id", active.body.value("request_id")},
                               {"demand_id", active.body.value("demand_id")},
                               {"frame_id", frame.value("frame_id")},
                               {"lease_id", descriptor.value("lease_id")},
                               {"context", active.body.value("context")},
                               {"status", "ok"}, {"reason_code", QJsonValue::Null},
                               {"provider", "synthetic"}, {"model_id", "no-model"},
                               {"elapsed_ms", 0}, {"tokens", QJsonArray{token}}};
        return message(QStringLiteral("result"), body);
    };
    auto release = [&] {
        const auto descriptor = active.body.value("frame").toObject().value("descriptor").toObject();
        reader.close();
        activeBytes.clear();
        return !reader.isOpen() && send(message(QStringLiteral("release_frame"),
            {{"request_id", active.body.value("request_id")}, {"lease_id", descriptor.value("lease_id")},
             {"slot_index", descriptor.value("slot_index")}, {"slot_generation", descriptor.value("slot_generation")}}));
    };
    while (true) {
        const QByteArray bytes = readStdin();
        if (bytes.isEmpty()) return protocol && protocol->state() == SessionState::Closed ? 0 : 65;
        const auto batch = framer.feed(bytes);
        if (!batch.ok()) return 66;
        for (const auto& envelope : batch.messages) {
            if (!protocol) protocol = std::make_unique<WorkerProtocol>(envelope.sessionId, WorkerRole::Worker);
            if (!protocol->receive(envelope)) return 67;
            if (envelope.kind == QStringLiteral("hello")) {
                if (mode == QStringLiteral("stall")) { QThread::msleep(10000); return 68; }
                if (mode == QStringLiteral("malformed")) return writeStdout("{broken\n") ? 0 : 69;
                if (mode == QStringLiteral("partial")) return writeStdout("{\"protocol_version\":") ? 0 : 69;
                if (mode == QStringLiteral("eof")) return 0;
                if (mode == QStringLiteral("stdout_flood")) {
                    const QByteArray flood(16384, 'x');
                    for (int i = 0; i < 256; ++i) if (!writeStdout(flood)) break;
                    return 0;
                }
                if (mode == QStringLiteral("stderr")) {
                    const QByteArray flood(16384, 'e');
                    for (int i = 0; i < 16; ++i) std::fwrite(flood.constData(), 1, flood.size(), stderr);
                    std::fflush(stderr);
                }
                pool = envelope.body.value("memory_pool").toArray();
                if (!send(message(QStringLiteral("ready"), {{"selected_version", 1}, {"worker_build", "fixture-process-1"},
                    {"provider", "synthetic"}, {"model_id", "no-model"}, {"model_sha256", QString(64, '0')},
                    {"capabilities", QJsonArray{"BGRA8", "win32_named_shared_memory", "cancel", "release_frame"}}}))) return 70;
            } else if (envelope.kind == QStringLiteral("recognize")) {
                active = envelope;
                QString error;
                if (!reader.open(protocol->sessionId(), active.body.value("frame").toObject().value("descriptor").toObject(), pool, &error)) return 71;
                activeBytes = reader.copyBytes(&error);
                if (!error.isEmpty() || activeBytes.isEmpty()) return 72;
                if (mode == QStringLiteral("cancel") || mode == QStringLiteral("request_stall")) continue;
                if (mode == QStringLiteral("session_error_stall")) {
                    if (!send(message(QStringLiteral("error"), {{"request_id", QJsonValue::Null},
                            {"code", "E_OCR_PROVIDER"}, {"retryable", false}, {"message", "synthetic unrelated session diagnostic"}}))) return 79;
                    continue;
                }
                if (!send(result()) || !release()) return 73;
                active = {};
            } else if (envelope.kind == QStringLiteral("cancel")) {
                if (!send(message(QStringLiteral("cancelled"), {{"target_kind", envelope.body.value("target_kind")},
                    {"target_id", envelope.body.value("target_id")}, {"registered", true}}))) return 74;
                if (!active.kind.isEmpty()) {
                    if (mode == QStringLiteral("cancel")) {
                        // Deliberately inject a validly encoded, cancelled late
                        // callback outside the worker state machine to exercise
                        // the supervisor's ignore-and-drain behavior.
                        if (!writeStdout(WorkerProtocol::encodeLine(result()))) return 75;
                    }
                    if (!release()) return 76;
                    active = {};
                }
            } else if (envelope.kind == QStringLiteral("shutdown")) {
                if (!active.kind.isEmpty() && !release()) return 77;
                if (!send(message(QStringLiteral("bye"), {{"all_leases_closed", true}}))) return 78;
                return 0;
            }
        }
    }
}
