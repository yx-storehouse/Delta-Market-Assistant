#include "application/collection_hotkey.h"

#include "domain.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace relink::application {

CollectionRunnerPaths locateCollectionRunner(const QString& applicationDir) {
    CollectionRunnerPaths paths;
    QDir dir(applicationDir);
    for (int depth = 0; depth < 5; ++depth) {
        const QString python = dir.filePath(QStringLiteral(".tools/ocr-runtime/Scripts/python.exe"));
        if (QFileInfo(python).isFile() && QFileInfo(dir.filePath(QStringLiteral("tests/manual"))).isDir()
            && QFileInfo(dir.filePath(QStringLiteral("dist"))).isDir()) {
            paths.projectRoot = dir.absolutePath();
            paths.python = QDir::toNativeSeparators(python);
            break;
        }
        if (!dir.cdUp()) break;
    }
    if (paths.projectRoot.isEmpty()) return {};
    // The delivered copy next to the program wins; a build-tree binary uses
    // the development sources of the same project.
    const QString delivered = QDir(applicationDir).filePath(QStringLiteral("collection/run_collection_hotkey.py"));
    const QString development = QDir(paths.projectRoot).filePath(QStringLiteral("tests/manual/run_collection_hotkey.py"));
    if (QFileInfo(delivered).isFile()) paths.script = QDir::toNativeSeparators(delivered);
    else if (QFileInfo(development).isFile()) paths.script = QDir::toNativeSeparators(development);
    else return {};
    return paths;
}

QString collectionLogDirectory(const QString& applicationDir) {
    const auto paths = locateCollectionRunner(applicationDir);
    return paths.valid() ? QDir(paths.projectRoot).filePath(QStringLiteral("artifacts/logs")) : QString();
}

unsigned hotkeyVirtualKey(const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^F([1-9]|1[0-2])$"));
    const auto match = pattern.match(name);
    if (!match.hasMatch()) return 0;
    return 0x70u + unsigned(match.captured(1).toInt() - 1);  // VK_F1 = 0x70
}


QStringList collectionRunnerArguments(const CollectionRunnerPaths& paths, const QString& configPath,
    const QString& hotkey, const QString& overlayExecutable) {
    return {QStringLiteral("-B"), QStringLiteral("-X"), QStringLiteral("utf8"), paths.script,
        QStringLiteral("--standby"), QStringLiteral("--config"), QDir::toNativeSeparators(configPath),
        QStringLiteral("--hotkey"), hotkey, QStringLiteral("--overlay-exe"), QDir::toNativeSeparators(overlayExecutable)};
}

#ifdef Q_OS_WIN
namespace {
constexpr wchar_t SinkClass[] = L"RelinkStudioHotkeySink";
constexpr int HotkeyId = 0x5253;

LRESULT CALLBACK sinkProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_HOTKEY && int(wparam) == HotkeyId) {
        auto* controller = reinterpret_cast<CollectionHotkeyController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (controller) QMetaObject::invokeMethod(controller, "toggle", Qt::QueuedConnection);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace
#endif

CollectionHotkeyController::CollectionHotkeyController(AppState* state, QObject* parent, CollectionRunnerPaths paths)
    : QObject(parent), m_state(state), m_paths(std::move(paths)) {
#ifdef Q_OS_WIN
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(WNDCLASSEXW);
    cls.lpfnWndProc = sinkProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = SinkClass;
    if (RegisterClassExW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        HWND sink = CreateWindowExW(0, SinkClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
        if (sink) {
            SetWindowLongPtrW(sink, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            m_sink = reinterpret_cast<quintptr>(sink);
        }
    }
#endif
    syncHotkey();
}

CollectionHotkeyController::~CollectionHotkeyController() {
    shutdown(3000);
#ifdef Q_OS_WIN
    if (m_sink) {
        if (m_registeredKey) UnregisterHotKey(reinterpret_cast<HWND>(m_sink), HotkeyId);
        SetWindowLongPtrW(reinterpret_cast<HWND>(m_sink), GWLP_USERDATA, 0);
        DestroyWindow(reinterpret_cast<HWND>(m_sink));
    }
#endif
}

bool CollectionHotkeyController::runnerAlive() const {
    return m_process && m_process->state() != QProcess::NotRunning;
}

void CollectionHotkeyController::log(const QString& level, const QString& message) {
    trace(level, message);
    if (!m_state) return;
    m_state->addLog(level, message);
    m_state->notifyChanged();
}

void CollectionHotkeyController::trace(const QString& level, const QString& message) {
    const QString directory = m_paths.valid() ? QDir(m_paths.projectRoot).filePath(QStringLiteral("artifacts/logs"))
                                              : collectionLogDirectory(QCoreApplication::applicationDirPath());
    if (directory.isEmpty() || !QDir().mkpath(directory)) return;
    const QDateTime now = QDateTime::currentDateTime();
    QFile file(QDir(directory).filePath(now.toString(QStringLiteral("yyyy-MM-dd")) + QStringLiteral(".gui.log")));
    if (!file.open(QIODevice::Append | QIODevice::Text)) return;
    QString text = message;
    text.replace(QLatin1Char('\n'), QStringLiteral(" | "));
    file.write(QStringLiteral("%1 %2 [gui] %3\n").arg(now.toString(QStringLiteral("HH:mm:ss.zzz")),
        level.leftJustified(5), text.left(2000)).toUtf8());
}

void CollectionHotkeyController::syncHotkey() {
    // Only a changed key is (re)registered, so a key taken by another program
    // is reported once, not on every configuration refresh.
    const QString wanted = m_state ? m_state->run.hotkey : QStringLiteral("F2");
    if (wanted == m_hotkey) return;
#ifdef Q_OS_WIN
    HWND sink = reinterpret_cast<HWND>(m_sink);
    if (!sink) { m_hotkey = wanted; return; }
    if (m_registeredKey) UnregisterHotKey(sink, HotkeyId);
    m_registeredKey = 0;
    m_hotkey = wanted;
    const unsigned key = hotkeyVirtualKey(wanted);
    if (key && RegisterHotKey(sink, HotkeyId, MOD_NOREPEAT, key)) {
        m_registeredKey = key;
        log(QStringLiteral("INFO"), QStringLiteral("运行快捷键 %1 已生效：在游戏里按一下开始收藏+购买（真实购买），再按一下停止").arg(wanted));
    } else {
        log(QStringLiteral("WARN"), QStringLiteral("运行快捷键 %1 没有生效：可能被其他程序占用，请换一个按键").arg(wanted));
    }
#else
    m_hotkey = wanted;
#endif
}

void CollectionHotkeyController::preload() {
    if (runnerAlive()) return;
    if (launch()) log(QStringLiteral("INFO"), QStringLiteral("正在后台加载识别模型，加载完按 %1 即可开始收藏").arg(m_hotkey));
}

bool CollectionHotkeyController::launch() {
    const auto paths = m_paths.valid() ? m_paths : locateCollectionRunner(QCoreApplication::applicationDirPath());
    if (!paths.valid()) {
        log(QStringLiteral("ERROR"), QStringLiteral("找不到收藏运行环境（.tools/ocr-runtime 与 collection 目录），快捷键收藏不可用"));
        return false;
    }
    delete m_process;
    m_process = new QProcess(this);
    m_pending.clear();
    m_startQueued = m_stopRequested = false;
    m_runner = Runner::Loading;
    m_process->setProgram(paths.python);
    m_process->setArguments(collectionRunnerArguments(paths, m_state->configPath, m_hotkey,
        QCoreApplication::applicationFilePath()));
    m_process->setWorkingDirectory(paths.projectRoot);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    m_process->setProcessEnvironment(environment);
    const QString logs = QDir(paths.projectRoot).filePath(QStringLiteral("artifacts/hotkey_runs"));
    QDir().mkpath(logs);
    m_process->setStandardErrorFile(QDir(logs).filePath(QStringLiteral("runner_stderr.log")), QIODevice::Append);
#ifdef Q_OS_WIN
    m_process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
#endif
    connect(m_process, &QProcess::readyReadStandardOutput, this, &CollectionHotkeyController::readOutput);
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) { finished(code); });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError failure) {
        if (failure == QProcess::FailedToStart) {
            m_runner = Runner::Off;
            log(QStringLiteral("ERROR"), QStringLiteral("收藏程序没有启动：") + m_process->errorString());
        }
    });
    trace(QStringLiteral("INFO"), QStringLiteral("runner launch: %1 %2").arg(paths.python, m_process->arguments().join(QLatin1Char(' '))));
    m_process->start();
    return true;
}

void CollectionHotkeyController::toggle() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastPressMs < 400) return;
    m_lastPressMs = now;
    trace(QStringLiteral("INFO"), QStringLiteral("hotkey %1 pressed; runner alive=%2 state=%3 queued=%4 stopRequested=%5")
        .arg(m_hotkey).arg(runnerAlive()).arg(int(m_runner)).arg(m_startQueued).arg(m_stopRequested));
    if (!runnerAlive()) {
        if (!launch()) return;
        queueStart();
        return;
    }
    if (m_runner == Runner::Running || m_startQueued) requestStop();
    else queueStart();
}

void CollectionHotkeyController::queueStart() {
    QString error;
    if (m_save && !m_save(&error)) {
        log(QStringLiteral("ERROR"), QStringLiteral("保存当前任务失败，未开始收藏：") + error);
        return;
    }
    quintptr foreground = 0;
#ifdef Q_OS_WIN
    foreground = reinterpret_cast<quintptr>(GetForegroundWindow());
#endif
    m_process->write(QStringLiteral("start %1 %2\n").arg(foreground).arg(m_hotkey).toUtf8());
    trace(QStringLiteral("INFO"), QStringLiteral("sent start foreground_hwnd=%1").arg(foreground));
    m_startQueued = true;
    m_stopRequested = false;
    if (m_runner == Runner::Loading)
        log(QStringLiteral("INFO"), QStringLiteral("已按 %1：识别模型还在加载，加载完自动开始收藏").arg(m_hotkey));
    else
        log(QStringLiteral("INFO"), QStringLiteral("已按 %1：开始收藏+购买（真实购买），再按 %1 停止").arg(m_hotkey));
}

void CollectionHotkeyController::requestStop() {
    if (m_runner != Runner::Running) {
        // Still loading: the queued start is withdrawn, nothing ran.
        m_process->write("stop\n");
        m_startQueued = false;
        log(QStringLiteral("INFO"), QStringLiteral("已按 %1：取消开始").arg(m_hotkey));
        return;
    }
    if (m_stopRequested) {
        log(QStringLiteral("INFO"), QStringLiteral("正在停止收藏，请稍候"));
        return;
    }
    m_stopRequested = true;
    m_process->write("stop\n");
    log(QStringLiteral("INFO"), QStringLiteral("已按 %1：会在当前这一步完成后停止，不会再按购买").arg(m_hotkey));
}

void CollectionHotkeyController::readOutput() {
    if (!m_process) return;
    m_pending += m_process->readAllStandardOutput();
    for (int end = m_pending.indexOf('\n'); end >= 0; end = m_pending.indexOf('\n')) {
        const QByteArray line = m_pending.left(end).trimmed();
        m_pending.remove(0, end + 1);
        QJsonParseError parseError;
        const QJsonObject message = QJsonDocument::fromJson(line, &parseError).object();
        if (parseError.error != QJsonParseError::NoError && !line.isEmpty())
            trace(QStringLiteral("DEBUG"), QStringLiteral("runner output (not JSON): ") + QString::fromUtf8(line.left(500)));
        const QString type = message.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("state")) {
            const QString value = message.value(QStringLiteral("state")).toString();
            trace(QStringLiteral("INFO"), QStringLiteral("runner state %1%2").arg(value,
                message.contains(QStringLiteral("load_ms")) ? QStringLiteral(" load_ms=%1").arg(message.value(QStringLiteral("load_ms")).toInt()) : QString()));
            const bool wasRunning = m_runner == Runner::Running;
            if (value == QStringLiteral("loading")) m_runner = Runner::Loading;
            else if (value == QStringLiteral("running")) { m_runner = Runner::Running; m_startQueued = false; }
            else if (value == QStringLiteral("ready")) {
                m_runner = Runner::Ready;
                if (wasRunning) m_stopRequested = false;
            } else if (value == QStringLiteral("failed")) m_runner = Runner::Off;
            if (wasRunning != (m_runner == Runner::Running)) emit runningChanged(m_runner == Runner::Running);
        } else if (type == QStringLiteral("status") && message.value(QStringLiteral("log")).toBool()) {
            const QString tone = message.value(QStringLiteral("tone")).toString();
            log(tone == QStringLiteral("error") ? QStringLiteral("ERROR") : tone == QStringLiteral("warn") ? QStringLiteral("WARN") : QStringLiteral("INFO"),
                message.value(QStringLiteral("text")).toString());
        } else if (type == QStringLiteral("result")) {
            const QString directory = message.value(QStringLiteral("run_directory")).toString();
            if (!directory.isEmpty())
                log(QStringLiteral("INFO"), QStringLiteral("本次收藏记录：") + QDir::toNativeSeparators(directory));
        }
    }
    if (m_pending.size() > 1024 * 1024) m_pending.clear();
}

void CollectionHotkeyController::finished(int exitCode) {
    readOutput();
    trace(exitCode ? QStringLiteral("WARN") : QStringLiteral("INFO"),
          QStringLiteral("runner exited code=%1 status=%2 planned=%3").arg(exitCode)
              .arg(m_process ? int(m_process->exitStatus()) : -1).arg(m_shuttingDown));
    const bool wasRunning = m_runner == Runner::Running;
    m_runner = Runner::Off;
    m_startQueued = m_stopRequested = false;
    if (!m_shuttingDown)
        log(QStringLiteral("WARN"), QStringLiteral("收藏程序已退出（代码 %1），下次按 %2 会重新加载").arg(exitCode).arg(m_hotkey));
    if (wasRunning) emit runningChanged(false);
}

void CollectionHotkeyController::shutdown(int waitMs) {
    if (!runnerAlive()) return;
    m_shuttingDown = true;
    // EOF: a run stops before its next action, then the runner exits.
    m_process->closeWriteChannel();
    if (!m_process->waitForFinished(waitMs)) {
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

} // namespace relink::application
