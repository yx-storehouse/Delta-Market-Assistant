#include "windows_ocr.h"
#include "dxgi_observation_source.h"
#include "shared_frame_memory.h"
#include "numeric_roi.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <QRectF>
#include <thread>
#include <utility>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace relink::vision {
namespace {
OcrReply failure(const QString& code) { return {false, code, {}}; }
#ifdef Q_OS_WIN
struct HelperUiState { DWORD pid = 0; int checks = 0; bool visible = false; bool foreground = false; };
void inspectHelperUi(HelperUiState& state) {
    ++state.checks;
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    state.foreground |= foregroundPid == state.pid;
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        auto& s = *reinterpret_cast<HelperUiState*>(param);
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid == s.pid && IsWindowVisible(window)) s.visible = true;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
}
#endif
}

// Helper idle policy (see setWindowsOcrHelperIdlePolicy).
std::atomic<int> g_helperIdleMs{600000}, g_helperRestartAfterMs{570000}, g_helperRequestLimit{4000};

// Deliberately separate from WorkerProcess: the existing vision-worker schema
// requires confidence/model hashes that Windows OCR does not expose. This
// explicit session wraps the unchanged one-shot result and adds lease release.
class WindowsOcrSession final {
public:
    explicit WindowsOcrSession(QString path) : m_path(std::move(path)), m_state(new State) {}
    ~WindowsOcrSession() {
#ifdef Q_OS_WIN
        if (!stop(false)) {
            // Never release a mapping while an unconfirmed reader may use it.
            // This exceptional state remains quarantined until process exit.
            // The OS reclaims these handles when this coordinator exits.
            (void)m_state.release();
        }
#endif
    }

    bool poisoned() const { return m_poisoned; }

    QJsonObject metrics() const {
        return {{"protocol", "windows-ocr-session-v1"}, {"scope", "current_diagnostic_process"},
            {"helper_start_count", m_starts}, {"request_count", m_requests},
            {"success_count", m_successes}, {"engine_create_count", m_engineCreates},
            {"frame_mappings_released", m_released}, {"helper_pid", QString::number(m_pid)},
            {"helper_running", m_state->process.state() != QProcess::NotRunning},
            {"mapping_quarantined", bool(m_state->mapping)}, {"session_poisoned", m_poisoned},
            {"helper_ui_checks", m_uiChecks}, {"helper_visible_window_observed", m_visible},
            {"helper_foreground_observed", m_foreground}, {"calls", m_calls}};
    }

    void amend(const QString& requestId, const QJsonObject& values) {
        if (m_calls.isEmpty()) return;
        auto last = m_calls.last().toObject();
        if (last.value("request_id") != requestId) return;
        for (auto i = values.begin(); i != values.end(); ++i) last[i.key()] = i.value();
        m_calls[m_calls.size()-1] = last;
    }

    OcrReply run(const runtime::observation::FrameEnvelope& frame, const QString& language, int timeoutMs) {
#ifndef Q_OS_WIN
        Q_UNUSED(frame); Q_UNUSED(language); Q_UNUSED(timeoutMs);
        return failure(QStringLiteral("E_PLATFORM_UNSUPPORTED"));
#else
        if (m_poisoned || m_state->mapping) return failure(QStringLiteral("E_OCR_SESSION_POISONED"));
        // No event loop runs in this thread, so QProcess notices the helper's
        // own idle exit only inside a wait. Refresh first: an idle-exited helper
        // is restarted, never written to (that poisoned a pool session after a
        // long standby, 2026-10-08). Close to the helper's idle limit, retire it
        // gracefully instead of racing a request against its exit.
        bool retiredIdle = false;
        if (m_state->process.state() != QProcess::NotRunning) m_state->process.waitForFinished(0);
        if (m_state->process.state() == QProcess::Running
            && ((m_idle.isValid() && m_idle.elapsed() >= g_helperRestartAfterMs.load())
                || m_helperRequests >= g_helperRequestLimit.load()))
            retiredIdle = stop(false);
        QElapsedTimer total;
        total.start();
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString frameId = frame.frameId.isEmpty() ? QStringLiteral("request:") + id : frame.frameId;
        if (frameId.size() > 256 || frameId.contains(QChar(0))) return failure(QStringLiteral("E_OCR_FRAME"));
        ++m_requests;
        const bool reused = m_state->process.state() == QProcess::Running;
        QJsonObject timing{{"request_id", id}, {"frame_id", frameId}, {"language", language},
            {"helper_reused", reused}, {"helper_start_ms", 0}, {"frame_released", false}};
        const int checksBefore = m_uiChecks;
        auto complete = [&](OcrReply reply) {
            timing["frame_total_ms"] = total.elapsed();
            timing["helper_pid"] = QString::number(m_pid);
            timing["helper_idle_retired"] = retiredIdle;
            m_idle.start();
            timing["ok"] = reply.ok;
            timing["error"] = reply.error;
            reply.timing = timing;
            reply.helperUiChecks = m_uiChecks - checksBefore;
            reply.helperVisibleWindowObserved = m_visible;
            reply.helperForegroundObserved = m_foreground;
            m_calls.append(timing);
            if (m_calls.size() > 128) m_calls.removeFirst();
            return reply;
        };
        auto abort = [&](const QString& error) {
            m_poisoned = true;
            const bool exited = stop(true);
            timing["helper_exit_confirmed"] = exited;
            timing["frame_released"] = exited;
            return complete(failure(exited ? error : QStringLiteral("E_OCR_EXIT_UNCONFIRMED")));
        };
        m_state->mapping = std::make_unique<SharedFrameMemory>();
        QString error;
        if (!m_state->mapping->create(id, 0, frame.validBytes, &error)
            || !m_state->mapping->write(frame.pixels, 0, &error)) {
            m_state->mapping.reset();
            return complete(failure(error));
        }
        timing["mapping_prepare_ms"] = total.elapsed();
        const QString mappingName = m_state->mapping->poolEntry().value("mapping_name").toString();
        timing["mapping_name"] = mappingName;
        if (!reused) {
            // An idle helper may have exited cleanly. A failed prior request
            // poisons the session instead and never restarts implicitly.
            if (m_starts && !retiredIdle
                && (m_state->process.exitStatus() != QProcess::NormalExit || m_state->process.exitCode() != 0))
                return abort(QStringLiteral("E_OCR_CRASH"));
            wchar_t systemDirectory[MAX_PATH]{};
            if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) return abort(QStringLiteral("E_OCR_HOST"));
            const QString program = QString::fromWCharArray(systemDirectory)
                + QStringLiteral("/WindowsPowerShell/v1.0/powershell.exe");
            if (!QFileInfo::exists(program)) return abort(QStringLiteral("E_OCR_HOST"));
            auto& process = m_state->process;
            process.setProgram(program);
            process.setArguments({QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"), m_path,
                QStringLiteral("-Session"), QStringLiteral("-OverlappedStdin"),
                QStringLiteral("-IdleTimeoutMs"), QString::number(g_helperIdleMs.load())});
            process.setWorkingDirectory(QFileInfo(m_path).absolutePath());
            process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
                a->flags |= CREATE_NO_WINDOW;
                a->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
                a->startupInfo->wShowWindow = SW_HIDE;
            });
            QElapsedTimer startup; startup.start();
            process.start();
            if (!process.waitForStarted(std::min(2000, timeoutMs))) return abort(QStringLiteral("E_OCR_START"));
            ++m_starts;
            m_helperRequests = 0;
            m_pid = process.processId();
            m_ui = {static_cast<DWORD>(m_pid), 0, false, false};
            timing["helper_start_ms"] = startup.elapsed();
        }
        auto& process = m_state->process;
        QByteArray output;
        qint64 stderrBytes = 0;
        auto drain = [&]() {
            inspectHelperUi(m_ui);
            ++m_uiChecks;
            m_visible |= m_ui.visible;
            m_foreground |= m_ui.foreground;
            process.setReadChannel(QProcess::StandardError);
            while (process.bytesAvailable() > 0) {
                stderrBytes += process.read(16384).size();
                if (stderrBytes > 65536) { error = QStringLiteral("E_OCR_OUTPUT_LIMIT"); break; }
            }
            process.setReadChannel(QProcess::StandardOutput);
            while (process.bytesAvailable() > 0 && error.isEmpty()) {
                output += process.read(16384);
                if (output.size() > 1048576) error = QStringLiteral("E_OCR_OUTPUT_LIMIT");
            }
            if (m_visible || m_foreground) error = QStringLiteral("E_OCR_HELPER_VISIBLE");
        };
        drain();
        if (!error.isEmpty()) return abort(error);
        if (!output.isEmpty()) return abort(QStringLiteral("E_OCR_RESPONSE"));
        const QJsonObject input{{"protocol", "windows-ocr-once-v1"}, {"request_id", id},
            {"mapping_name", mappingName}, {"width", frame.width}, {"height", frame.height},
            {"bytes", frame.validBytes}, {"language", language}};
        const QJsonObject envelope{{"protocol", "windows-ocr-session-v1"}, {"session_id", m_sessionId},
            {"frame_id", frameId}, {"request", input}};
        const QByteArray request = QJsonDocument(envelope).toJson(QJsonDocument::Compact) + '\n';
        QElapsedTimer roundtrip; roundtrip.start();
        if (request.size() > 4096 || process.write(request) != request.size()) return abort(QStringLiteral("E_OCR_WRITE"));
        ++m_helperRequests;
        timing["helper_request_index"] = m_helperRequests;
        // QProcess buffers writes. In this blocking diagnostic there is no Qt
        // event loop to deliver a later write notifier to an already-running
        // child. Flush the request explicitly before waiting for its reply.
        // Startup masked this requirement for the first request.
        while (process.bytesToWrite() > 0) {
            const int remaining = timeoutMs - int(total.elapsed());
            if (remaining <= 0) return abort(QStringLiteral("E_OCR_TIMEOUT"));
            if (!process.waitForBytesWritten(std::min(1000, remaining)) && process.bytesToWrite() > 0)
                return abort(QStringLiteral("E_OCR_WRITE"));
            drain();
            if (!error.isEmpty()) return abort(error);
        }
        timing["request_write_ms"] = roundtrip.elapsed();
        while (error.isEmpty() && !output.contains('\n') && total.elapsed() < timeoutMs) {
            process.waitForReadyRead(std::min(20, std::max(1, timeoutMs-int(total.elapsed()))));
            drain();
            if (process.state() == QProcess::NotRunning && !output.contains('\n')) { error = QStringLiteral("E_OCR_CRASH"); break; }
        }
        timing["helper_roundtrip_ms"] = roundtrip.elapsed();
        if (!error.isEmpty()) return abort(error);
        if (!output.contains('\n')) return abort(QStringLiteral("E_OCR_TIMEOUT"));
        const int end = output.indexOf('\n');
        if (end != output.size()-1) return abort(QStringLiteral("E_OCR_RESPONSE"));
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(output.left(end), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) return abort(QStringLiteral("E_OCR_RESPONSE"));
        const auto replyEnvelope = document.object();
        const auto keys = replyEnvelope.keys();
        if (QSet<QString>(keys.begin(), keys.end()) != QSet<QString>{"protocol","session_id","request_id","frame_id",
                "mapping_name","frame_released","result","timing"}
            || replyEnvelope.value("protocol") != "windows-ocr-session-v1" || replyEnvelope.value("session_id") != m_sessionId
            || replyEnvelope.value("request_id") != id || replyEnvelope.value("frame_id") != frameId
            || replyEnvelope.value("mapping_name") != mappingName)
            return abort(QStringLiteral("E_OCR_CORRELATION"));
        if (!replyEnvelope.value("frame_released").isBool() || !replyEnvelope.value("frame_released").toBool())
            return abort(QStringLiteral("E_OCR_RELEASE"));
        if (!replyEnvelope.value("result").isObject() || !replyEnvelope.value("timing").isObject())
            return abort(QStringLiteral("E_OCR_RESPONSE"));
        const auto workerTiming = replyEnvelope.value("timing").toObject();
        for (const auto key : {"runtime_init_ms","engine_init_ms","bitmap_copy_ms","recognize_ms","helper_request_ms"}) {
            const auto value = workerTiming.value(key);
            if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble()<0 || value.toDouble()>60000)
                return abort(QStringLiteral("E_OCR_RESPONSE"));
            timing[QStringLiteral("worker_") + QLatin1String(key)] = value;
        }
        if (!workerTiming.value("engine_cache_hit").isBool() || !workerTiming.value("engine_created").isBool()
            || !workerTiming.value("engine_create_count").isDouble()
            || workerTiming.value("engine_create_count").toDouble() < 0
            || workerTiming.value("engine_create_count").toDouble() > 2
            || std::floor(workerTiming.value("engine_create_count").toDouble()) != workerTiming.value("engine_create_count").toDouble())
            return abort(QStringLiteral("E_OCR_RESPONSE"));
        timing["engine_cache_hit"] = workerTiming.value("engine_cache_hit");
        timing["engine_created"] = workerTiming.value("engine_created");
        const auto body = replyEnvelope.value("result").toObject();
        auto reply = validateOcrReply(QJsonDocument(body).toJson(QJsonDocument::Compact), id, frame.width, frame.height);
        const bool negative = body.value("protocol") == "windows-ocr-once-v1" && body.value("request_id") == id
            && body.value("ok").isBool() && !body.value("ok").toBool()
            && QStringList{"E_OCR_LANGUAGE","E_OCR_IMAGE_SIZE","E_OCR_WORKER_INPUT","E_OCR_ENGINE","E_OCR_TOKEN_LIMIT"}
                .contains(body.value("error").toString());
        if (!reply.ok && !negative) return abort(reply.error);
        if (reply.ok && reply.observation.value("language") != language) return abort(QStringLiteral("E_OCR_LANGUAGE"));
        // The worker's finally has disposed this exact mapping before emitting
        // the correlated acknowledgement. The owner can now release its view.
        m_state->mapping->close();
        m_state->mapping.reset();
        ++m_released;
        if (workerTiming.value("engine_created").toBool()) ++m_engineCreates;
        timing["frame_released"] = true;
        if (reply.ok) { ++m_successes; reply.observation["frame_id"] = frameId; }
        return complete(reply);
#endif
    }

private:
    struct State {
        std::unique_ptr<SharedFrameMemory> mapping;
        QProcess process;
    };
    bool stop(bool forced) {
        auto& process = m_state->process;
        if (process.state() != QProcess::NotRunning) {
            if (!forced) { process.closeWriteChannel(); process.waitForFinished(1000); }
            if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(1500); }
        }
        if (process.state() != QProcess::NotRunning) return false;
        if (m_state->mapping) { m_state->mapping->close(); m_state->mapping.reset(); ++m_released; }
        return true;
    }
    QString m_path;
    QString m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::unique_ptr<State> m_state;
    qint64 m_pid = 0;
    int m_starts = 0, m_requests = 0, m_successes = 0, m_engineCreates = 0, m_released = 0, m_uiChecks = 0;
    bool m_poisoned = false, m_visible = false, m_foreground = false;
    QElapsedTimer m_idle;
    int m_helperRequests = 0;
    QJsonArray m_calls;
#ifdef Q_OS_WIN
    HelperUiState m_ui;
#endif
};

namespace {
std::shared_ptr<WindowsOcrSession> acquireSession(const QString& helper) {
    // A temporary English recognizer in the live ROI loop reuses the Chinese
    // recognizer's session, but unrelated threads never share a QProcess.
    static thread_local QHash<QString, std::weak_ptr<WindowsOcrSession>> sessions;
    for (auto it=sessions.begin(); it!=sessions.end();) {
        if (it.value().expired()) it=sessions.erase(it); else ++it;
    }
    const QString path = QFileInfo(helper).canonicalFilePath();
    if (auto existing = sessions.value(path).lock()) return existing;
    auto session = std::make_shared<WindowsOcrSession>(path);
    sessions.insert(path, session);
    return session;
}
}

void setWindowsOcrHelperRequestLimit(int requests) {
    g_helperRequestLimit = std::clamp(requests, 1, 4000);
}

void setWindowsOcrHelperIdlePolicy(int helperIdleMs, int restartAfterMs) {
    g_helperIdleMs = std::clamp(helperIdleMs, 100, 3600000);
    g_helperRestartAfterMs = std::max(0, restartAfterMs);
}

OcrReply validateOcrReply(const QByteArray& data, const QString& requestId, int width, int height) {
    if (data.isEmpty() || data.size() > 1048576 || requestId.isEmpty() || width < 1 || height < 1) return failure(QStringLiteral("E_OCR_RESPONSE"));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return failure(QStringLiteral("E_OCR_RESPONSE"));
    auto object = document.object();
    if (object.value("request_id") != requestId || object.value("protocol") != "windows-ocr-once-v1")
        return failure(QStringLiteral("E_OCR_CORRELATION"));
    if (!object.value("ok").isBool()) return failure(QStringLiteral("E_OCR_RESPONSE"));
    if (!object.value("ok").toBool()) {
        const auto code = object.value("error").toString();
        if (code == "E_OCR_LANGUAGE" || code == "E_OCR_IMAGE_SIZE" || code == "E_OCR_WORKER_INPUT"
            || code == "E_OCR_ENGINE" || code == "E_OCR_TOKEN_LIMIT") return failure(code);
        return failure(QStringLiteral("E_OCR_ENGINE"));
    }
    if (object.value("width").toInteger() != width || object.value("height").toInteger() != height
        || !object.value("words").isArray() || object.value("words").toArray().size() > 2000
        || !object.value("language").isString() || object.value("language").toString().size() > 50)
        return failure(QStringLiteral("E_OCR_RESPONSE"));
    const bool rotatedCoordinates = object.value("coordinate_space") == "windows_ocr_rotated";
    double angle = 0;
    if (object.contains("coordinate_space") && !rotatedCoordinates) return failure(QStringLiteral("E_OCR_COORDINATES"));
    if (rotatedCoordinates) {
        if (!object.contains("text_angle")) return failure(QStringLiteral("E_OCR_COORDINATES"));
        const auto value = object.value("text_angle");
        if (!value.isNull()) {
            if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble()) > 180)
                return failure(QStringLiteral("E_OCR_ANGLE"));
            angle = value.toDouble();
        }
    } else if (object.contains("text_angle")) return failure(QStringLiteral("E_OCR_COORDINATES"));
    const double radians = angle * 3.14159265358979323846 / 180.0;
    const double cosine = std::cos(radians), sine = std::sin(radians);
    QJsonArray mappedWords;
    int droppedOutside = 0;
    int textBytes = 0;
    for (const auto& value : object.value("words").toArray()) {
        if (!value.isObject()) return failure(QStringLiteral("E_OCR_TOKEN"));
        auto word = value.toObject();
        const auto text = word.value("text").toString();
        if (text.isEmpty() || text.size() > 1024) return failure(QStringLiteral("E_OCR_TOKEN"));
        textBytes += text.toUtf8().size();
        if (textBytes > 65536) return failure(QStringLiteral("E_OCR_TOKEN_LIMIT"));
        const auto x = word.value("x"), y = word.value("y"), w = word.value("width"), h = word.value("height");
        for (const auto& field : {x, y, w, h})
            if (!field.isDouble() || !std::isfinite(field.toDouble())) return failure(QStringLiteral("E_OCR_BOUNDS"));
        if (w.toDouble() <= 0 || h.toDouble() <= 0) return failure(QStringLiteral("E_OCR_BOUNDS"));
        if (!rotatedCoordinates) {
            if (x.toDouble()<0 || y.toDouble()<0 || x.toDouble()+w.toDouble()>width || y.toDouble()+h.toDouble()>height)
                return failure(QStringLiteral("E_OCR_BOUNDS"));
        } else {
            // Microsoft OcrResult.TextAngle: rotate word boxes clockwise around
            // the ORIGINAL image center, not around each word or the origin.
            const double limit = 2.0 * std::max(width,height);
            if (std::abs(x.toDouble())>limit || std::abs(y.toDouble())>limit || w.toDouble()>limit || h.toDouble()>limit)
                return failure(QStringLiteral("E_OCR_BOUNDS"));
            const QRectF original(x.toDouble(),y.toDouble(),w.toDouble(),h.toDouble());
            double left=limit*3, top=limit*3, right=-limit*3, bottom=-limit*3;
            for (const auto& point : {original.topLeft(),original.topRight(),original.bottomLeft(),original.bottomRight()}) {
                const double dx=point.x()-width/2.0, dy=point.y()-height/2.0;
                const double px=width/2.0+dx*cosine-dy*sine, py=height/2.0+dx*sine+dy*cosine;
                left=std::min(left,px);right=std::max(right,px);top=std::min(top,py);bottom=std::max(bottom,py);
            }
            const QRectF clipped=QRectF(left,top,right-left,bottom-top).intersected(QRectF(0,0,width,height));
            if (clipped.isEmpty()) { ++droppedOutside; continue; }
            word["x"]=clipped.x();word["y"]=clipped.y();word["width"]=clipped.width();word["height"]=clipped.height();
        }
        mappedWords.append(word);
    }
    object["words"]=mappedWords;
    object["coordinate_space"]="client_physical_px";
    object["source_text_angle_degrees"]=rotatedCoordinates ? QJsonValue(angle) : QJsonValue(QJsonValue::Null);
    object["coordinate_transform"]=rotatedCoordinates ? "clockwise_about_image_center" : "legacy_axis_aligned";
    object["outside_frame_tokens_dropped"]=droppedOutside;
    return {true, {}, object};
}

QJsonObject summarizeRecognizedPage(const QJsonObject& observation) {
    // Navigation labels occur on every game page: "交易行" by itself is not a
    // market-page proof. Only collect known, non-personal body anchors here.
    QString body;
    const int height = observation.value("height").toInt();
    for (const auto& value : observation.value("words").toArray()) {
        const auto word = value.toObject();
        const double y = word.value("y").toDouble();
        if (y >= height * 0.15 && y < height * 0.80) body += word.value("text").toString();
    }
    QJsonArray anchors;
    for (const auto& text : {QStringLiteral("技术中心"), QStringLiteral("工作台"), QStringLiteral("指挥中心"),
                            QStringLiteral("训练中心"), QStringLiteral("靶场"), QStringLiteral("净水中心"), QStringLiteral("收藏室")})
        if (body.contains(text)) anchors.append(text);
    return {{"page_hint", anchors.size() >= 3 ? "base" : "unknown"}, {"body_anchors", anchors},
        {"word_count", observation.value("words").toArray().size()}, {"business_fields_validated", false},
        {"confidence_available", false}};
}

WindowsOcrRecognizer::WindowsOcrRecognizer(QString helperPath, QString language, int timeoutMs)
    : m_helperPath(std::move(helperPath)), m_language(std::move(language)), m_timeoutMs(timeoutMs) {}

WindowsOcrRecognizer::~WindowsOcrRecognizer() = default;

bool WindowsOcrRecognizer::sessionPoisoned() const {
    return m_session && m_session->poisoned();
}

QJsonObject WindowsOcrRecognizer::sessionMetrics() const {
    return m_session ? m_session->metrics() : QJsonObject{{"protocol", "windows-ocr-session-v1"},
        {"scope", "current_diagnostic_process"}, {"helper_start_count", 0}, {"request_count", 0},
        {"success_count", 0}, {"engine_create_count", 0}, {"helper_running", false}, {"calls", QJsonArray{}}};
}

OcrReply WindowsOcrRecognizer::recognizeRegion(const runtime::observation::FrameEnvelope& frame, const QRect& region,int scale,bool invert) {
    QElapsedTimer regionClock; regionClock.start();
    if (!summarizeBgra(frame.pixels,frame.width,frame.height,frame.strideBytes).valid
        || frame.pixelFormat!="BGRA8" || frame.validBytes!=frame.pixels.size()
        || region.isEmpty() || !QRect(0,0,frame.width,frame.height).contains(region)
        || frame.strideBytes != frame.width*4 || scale<1 || scale>3
        || qint64(region.width())*scale>4096 || qint64(region.height())*scale>4096)
        return failure(QStringLiteral("E_OCR_REGION"));
    auto cropped=frame;
    cropped.width=region.width()*scale;cropped.height=region.height()*scale;cropped.strideBytes=cropped.width*4;
    cropped.validBytes=qint64(cropped.strideBytes)*cropped.height;cropped.pixels.resize(cropped.validBytes);
    int minimum=255,maximum=0;
    for(int y=0;y<region.height();++y){
        const auto* row=reinterpret_cast<const unsigned char*>(frame.pixels.constData())+qsizetype(y+region.y())*frame.strideBytes+region.x()*4;
        for(int x=0;x<region.width();++x){const int gray=(row[x*4]+2*row[x*4+1]+row[x*4+2])/4;minimum=std::min(minimum,gray);maximum=std::max(maximum,gray);}
    }
    for(int y=0;y<region.height();++y){
        char* row=cropped.pixels.data()+qsizetype(y*scale)*cropped.strideBytes;
        const char* original=frame.pixels.constData()+qsizetype(y+region.y())*frame.strideBytes+region.x()*4;
        for(int x=0;x<region.width();++x){
            const auto* pixel=reinterpret_cast<const unsigned char*>(original+x*4);
            const int gray=(pixel[0]+2*pixel[1]+pixel[2])/4;
            const int contrasted=maximum-minimum>=8 ? (gray-minimum)*255/(maximum-minimum) : gray;
            const unsigned char value=invert ? 255-contrasted : contrasted;
            for(int i=0;i<scale;++i){auto* out=reinterpret_cast<unsigned char*>(row+(x*scale+i)*4);out[0]=out[1]=out[2]=value;out[3]=255;}
        }
        for(int i=1;i<scale;++i)std::memcpy(row+i*cropped.strideBytes,row,cropped.strideBytes);
    }
    const qint64 preprocessMs = regionClock.elapsed();
    auto result=recognizeFrame(cropped);
    result.timing["roi_preprocess_ms"] = preprocessMs;
    result.timing["roi_total_ms"] = regionClock.elapsed();
    if (m_session) m_session->amend(result.timing.value("request_id").toString(),
        {{"roi_preprocess_ms", preprocessMs}, {"roi_total_ms", regionClock.elapsed()}});
    if(!result.ok)return result;
    auto words=result.observation.value("words").toArray();
    for(int i=0;i<words.size();++i){auto word=words[i].toObject();
        word["x"]=word.value("x").toDouble()/scale+region.x();word["y"]=word.value("y").toDouble()/scale+region.y();
        word["width"]=word.value("width").toDouble()/scale;word["height"]=word.value("height").toDouble()/scale;words[i]=word;}
    result.observation["words"]=words;result.observation["width"]=frame.width;result.observation["height"]=frame.height;
    result.observation["coverage"]="roi";
    result.observation["roi_scale"]=scale;
    result.observation["roi_preprocess"]=invert?"grayscale_minmax_contrast_inverted":"grayscale_minmax_contrast";
    return result;
}

OcrReply WindowsOcrRecognizer::recognizeNumericRegion(const runtime::observation::FrameEnvelope& frame,const QRect& region,int scale,int padding) {
    QElapsedTimer clock;clock.start();
    const auto prepared=prepareNumericRegion(frame,region,scale,padding);
    if(!prepared.ok){auto reply=failure(prepared.error);reply.observation["numeric_preprocess"]=prepared.metadata;return reply;}
    const qint64 preprocessMs=clock.elapsed();
    auto reply=recognizeFrame(prepared.frame);
    reply.timing["numeric_preprocess_ms"]=preprocessMs;
    if(reply.ok){QString error;reply.observation=mapNumericRegionObservation(prepared,reply.observation,&error);
        if(!error.isEmpty()){reply.ok=false;reply.error=error;}}
    else reply.observation["numeric_preprocess"]=prepared.metadata;
    reply.timing["numeric_total_ms"]=clock.elapsed();
    if(m_session)m_session->amend(reply.timing["request_id"].toString(),{{"numeric_preprocess_ms",preprocessMs},{"numeric_total_ms",clock.elapsed()}});
    return reply;
}

QString WindowsOcrRecognizer::recognize(const runtime::observation::FrameEnvelope& frame, QString* errorCode) {
    const auto reply = recognizeFrame(frame);
    if (errorCode) *errorCode = reply.error;
    return reply.ok ? QString::fromUtf8(QJsonDocument(reply.observation).toJson(QJsonDocument::Compact)) : QString();
}

OcrReply WindowsOcrRecognizer::recognizeFrame(const runtime::observation::FrameEnvelope& frame) {
    if (m_timeoutMs < 100 || m_timeoutMs > 15000 || (m_language != "zh-Hans-CN" && m_language != "en-US"))
        return failure(QStringLiteral("E_OCR_OPTIONS"));
    const auto summary = summarizeBgra(frame.pixels, frame.width, frame.height, frame.strideBytes);
    if (!summary.valid || frame.pixelFormat != "BGRA8" || frame.validBytes != frame.pixels.size()
        || frame.strideBytes != frame.width * 4) return failure(QStringLiteral("E_OCR_FRAME"));
    if (summary.nearBlack) return failure(QStringLiteral("E_FRAME_BLACK_OR_INVALID"));
    const QFileInfo helper(m_helperPath);
    if (!helper.isAbsolute() || !helper.isFile() || !helper.isReadable() || helper.isSymLink())
        return failure(QStringLiteral("E_OCR_HELPER"));
    if (!m_session) m_session = acquireSession(helper.absoluteFilePath());
    return m_session->run(frame, m_language, m_timeoutMs);
}

OcrReply runOcrRegionJob(const runtime::observation::FrameEnvelope& frame, const OcrRegionJob& job,
        WindowsOcrRecognizer& chinese, WindowsOcrRecognizer& english) {
    if (job.language != QStringLiteral("zh-Hans-CN") && job.language != QStringLiteral("en-US"))
        return failure(QStringLiteral("E_OCR_OPTIONS"));
    auto& recognizer = job.language == QStringLiteral("en-US") ? english : chinese;
    return job.numeric ? recognizer.recognizeNumericRegion(frame, job.region, job.scale)
                       : recognizer.recognizeRegion(frame, job.region, job.scale, job.invert);
}

struct WindowsOcrPool::State {
    struct Batch {
        const runtime::observation::FrameEnvelope* frame = nullptr;
        const QList<OcrRegionJob>* jobs = nullptr;
        std::vector<OcrReply>* replies = nullptr;
        std::vector<int>* owners = nullptr;
        std::atomic<int> next{0};
        int remaining = 0, users = 0;
    };
    QString helper;
    mutable std::mutex mutex;
    std::condition_variable wake, finished;
    Batch* batch = nullptr;
    quint64 generation = 0, batches = 0, workerJobs = 0, callerJobs = 0, callerReruns = 0;
    bool stopping = false;
    int workers = 0, warmed = 0, retired = 0;
    QStringList warmErrors;
    QList<int> retiredOwners;
    std::vector<std::thread> threads;

    // Returns false when this worker must retire: its helper session failed
    // for good and would otherwise fail every job it grabs (2026-10-08 hotkey
    // run: poisoned workers kept taking jobs). The caller re-reads them.
    bool work(Batch* b, int owner, WindowsOcrRecognizer& chinese, WindowsOcrRecognizer& english) {
        while (true) {
            const int index = b->next.fetch_add(1);
            if (index >= b->jobs->size()) return true;
            auto reply = runOcrRegionJob(*b->frame, b->jobs->at(index), chinese, english);
            reply.timing["ocr_pool_executor"] = owner;
            const bool retire = owner && (chinese.sessionPoisoned() || english.sessionPoisoned());
            std::lock_guard<std::mutex> lock(mutex);
            (*b->replies)[size_t(index)] = std::move(reply);
            (*b->owners)[size_t(index)] = owner;
            if (owner) ++workerJobs; else ++callerJobs;
            if (--b->remaining == 0) finished.notify_all();
            if (retire) {
                ++retired;
                retiredOwners.append(owner);
                return false;
            }
        }
    }
    void worker(int owner) {
        WindowsOcrRecognizer chinese(helper, QStringLiteral("zh-Hans-CN")), english(helper, QStringLiteral("en-US"));
        // Start this thread's hidden helper and both engines off the critical
        // path. A worker whose helper fails retires; the caller and remaining
        // workers still complete every batch, so no request depends on it.
        runtime::observation::FrameEnvelope warm;
        warm.frameId = QStringLiteral("ocr-pool-warmup:%1").arg(owner);
        warm.width = 96; warm.height = 48; warm.strideBytes = warm.width * 4;
        warm.validBytes = qint64(warm.strideBytes) * warm.height;
        warm.pixels = QByteArray(int(warm.validBytes), char(0xff));
        warm.pixelFormat = QStringLiteral("BGRA8");
        const auto first = chinese.recognizeFrame(warm);
        const auto second = english.recognizeFrame(warm);
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++warmed;
            finished.notify_all();
            if (!first.ok || !second.ok) {
                warmErrors.append(first.ok ? second.error : first.error);
                ++retired;
                return;
            }
        }
        quint64 seen = 0;
        while (true) {
            Batch* b = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stopping || (batch && generation != seen); });
                if (stopping) return;
                seen = generation; b = batch; ++b->users;
            }
            const bool keep = work(b, owner, chinese, english);
            std::lock_guard<std::mutex> lock(mutex);
            --b->users;
            finished.notify_all();
            if (!keep) return;
        }
    }
};

WindowsOcrPool::WindowsOcrPool(QString helperPath, int workers) : m_state(new State) {
    m_state->helper = std::move(helperPath);
    m_state->workers = std::clamp(workers, 0, 8);
    for (int i = 0; i < m_state->workers; ++i)
        m_state->threads.emplace_back([state = m_state.get(), owner = i + 1] { state->worker(owner); });
}

WindowsOcrPool::~WindowsOcrPool() {
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->stopping = true;
    }
    m_state->wake.notify_all();
    for (auto& thread : m_state->threads)
        if (thread.joinable()) thread.join();
}

QList<OcrReply> WindowsOcrPool::run(const runtime::observation::FrameEnvelope& frame, const QList<OcrRegionJob>& jobs,
        WindowsOcrRecognizer& callerChinese, WindowsOcrRecognizer& callerEnglish) {
    if (jobs.isEmpty()) return {};
    std::vector<OcrReply> replies(size_t(jobs.size()));
    std::vector<int> owners(size_t(jobs.size()), -1);
    State::Batch batch;
    batch.frame = &frame; batch.jobs = &jobs; batch.replies = &replies; batch.owners = &owners;
    batch.remaining = int(jobs.size());
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        ++m_state->batches; ++m_state->generation;
        m_state->batch = &batch;
    }
    m_state->wake.notify_all();
    m_state->work(&batch, 0, callerChinese, callerEnglish);
    {
        // Workers hold only a pointer to this stack batch: wait until every
        // taken job finished AND every worker released the batch.
        std::unique_lock<std::mutex> lock(m_state->mutex);
        m_state->finished.wait(lock, [&] { return batch.remaining == 0 && batch.users == 0; });
        m_state->batch = nullptr;
    }
    // A worker-side transport failure is not evidence about the pixels: read
    // the same job again on the caller's own session, exactly as the serial
    // path would have. Content results (including empty text) are kept.
    static const QSet<QString> transport{"E_OCR_SESSION_POISONED", "E_OCR_CRASH", "E_OCR_TIMEOUT", "E_OCR_WRITE",
        "E_OCR_START", "E_OCR_RESPONSE", "E_OCR_CORRELATION", "E_OCR_RELEASE", "E_OCR_EXIT_UNCONFIRMED",
        "E_OCR_HELPER_VISIBLE", "E_OCR_OUTPUT_LIMIT", "E_OCR_HOST", "E_OCR_WORKER_INPUT"};
    for (size_t index = 0; index < replies.size(); ++index) {
        if (owners[index] <= 0 || replies[index].ok || !transport.contains(replies[index].error)) continue;
        const QString failed = replies[index].error;
        auto reply = runOcrRegionJob(frame, jobs.at(int(index)), callerChinese, callerEnglish);
        reply.timing["ocr_pool_executor"] = 0;
        reply.timing["ocr_pool_rerun_after_worker_error"] = failed;
        reply.timing["ocr_pool_failed_executor"] = owners[index];
        replies[index] = std::move(reply);
        std::lock_guard<std::mutex> lock(m_state->mutex);
        ++m_state->callerReruns;
    }
    return QList<OcrReply>(replies.begin(), replies.end());
}

bool WindowsOcrPool::waitWarm(int timeoutMs) const {
    std::unique_lock<std::mutex> lock(m_state->mutex);
    return m_state->finished.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)),
        [&] { return m_state->warmed >= m_state->workers; });
}

QJsonObject WindowsOcrPool::metrics() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return {{"schema", "windows-ocr-pool-v1"}, {"workers", m_state->workers}, {"warmed", m_state->warmed},
        {"retired", m_state->retired}, {"warm_errors", QJsonArray::fromStringList(m_state->warmErrors)},
        {"batches", double(m_state->batches)}, {"worker_jobs", double(m_state->workerJobs)},
        {"caller_jobs", double(m_state->callerJobs)}, {"caller_reruns", double(m_state->callerReruns)},
        {"retired_after_session_failure", [&] { QJsonArray owners; for (int owner : m_state->retiredOwners) owners.append(owner); return owners; }()},
        {"image_file_writes", 0}};
}
} // namespace relink::vision
