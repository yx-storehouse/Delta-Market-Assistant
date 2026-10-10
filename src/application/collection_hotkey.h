#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

class AppState;
class QProcess;

namespace relink::application {

// Where the delivered collection runner lives relative to the running program:
// dist/RelinkStudio/collection/run_collection_hotkey.py (or tests/manual for a
// build-tree binary) and the project's .tools/ocr-runtime Python.
struct CollectionRunnerPaths {
    QString projectRoot, python, script;
    bool valid() const { return !projectRoot.isEmpty() && !python.isEmpty() && !script.isEmpty(); }
};
CollectionRunnerPaths locateCollectionRunner(const QString& applicationDir);

// <project>/artifacts/logs, shared by the runner (YYYY-MM-DD.log) and the
// program (YYYY-MM-DD.gui.log); empty when no project root is found.
QString collectionLogDirectory(const QString& applicationDir);

// "F1".."F12" -> virtual-key code, 0 for anything else.
unsigned hotkeyVirtualKey(const QString& name);

// Standby runner arguments; pure, for tests.
QStringList collectionRunnerArguments(const CollectionRunnerPaths& paths, const QString& configPath,
    const QString& hotkey, const QString& overlayExecutable);

// The run hotkey of the main program (no modifier, no auto-repeat). The runner
// is started in standby as soon as the program opens, so its recognition
// engines and capture service are loaded before the first press. A press sends
// "start" (queued while still loading); the next press sends "stop", which the
// runner honours before its next action. Collection only: nothing here can
// start a purchase.
class CollectionHotkeyController final : public QObject {
    Q_OBJECT
public:
    enum class Runner { Off, Loading, Ready, Running };

    // paths: tests fix the runner (and so the log folder) before anything is logged.
    explicit CollectionHotkeyController(AppState* state, QObject* parent = nullptr, CollectionRunnerPaths paths = {});
    ~CollectionHotkeyController() override;

    // Persist the current tasks before a run reads them from disk.
    void setSaveConfiguration(std::function<bool(QString*)> save) { m_save = std::move(save); }
    // Tests: a fixed runner instead of the one next to the program.
    void setRunnerPaths(const CollectionRunnerPaths& paths) { m_paths = paths; }
    bool hotkeyRegistered() const { return m_registeredKey != 0; }
    QString hotkeyName() const { return m_hotkey; }
    Runner runnerState() const { return m_runner; }
    bool runnerAlive() const;
    bool running() const { return m_runner == Runner::Running; }
    // Stop any run, let the runner exit, wait for it (program exit).
    void shutdown(int waitMs = 10000);

public slots:
    // Start the standby runner now (program start); no-op when already alive.
    void preload();
    void syncHotkey();
    void toggle();

signals:
    void runningChanged(bool running);

private:
    bool launch();
    void queueStart();
    void requestStop();
    void readOutput();
    void finished(int exitCode);
    void log(const QString& level, const QString& message);
    // Development log only (artifacts/logs/YYYY-MM-DD.gui.log), not the 日志 page.
    void trace(const QString& level, const QString& message);

    AppState* m_state = nullptr;
    std::function<bool(QString*)> m_save;
    CollectionRunnerPaths m_paths;
    QProcess* m_process = nullptr;
    Runner m_runner = Runner::Off;
    QString m_hotkey;
    unsigned m_registeredKey = 0;
    quintptr m_sink = 0;
    qint64 m_lastPressMs = 0;
    bool m_startQueued = false;
    bool m_stopRequested = false;
    bool m_shuttingDown = false;
    QByteArray m_pending;
};

} // namespace relink::application
