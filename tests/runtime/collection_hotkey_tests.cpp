#include "application/collection_hotkey.h"
#include "domain.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>

using namespace relink::application;
namespace {
int failures = 0;
void check(bool value, const char* label) {
    if (!value) ++failures;
    std::cout << (value ? "PASS " : "FAIL ") << label << '\n';
}
void touch(const QString& path, const QByteArray& bytes = {}) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(bytes);
}
int logCount(const AppState& state, const QString& text) {
    int count = 0;
    for (const auto& entry : state.logs) count += entry.message.contains(text);
    return count;
}
bool hasLog(const AppState& state, const QString& text) { return logCount(state, text) > 0; }
bool waitFor(const std::function<bool()>& done, int ms = 8000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return done();
}
void pause() {  // past the 400 ms press debounce
    QThread::msleep(450);
    QCoreApplication::processEvents();
}

// Same standby JSON-lines protocol as run_collection_hotkey.py; never touches a window.
constexpr char FakeRunner[] = R"(import json, os, sys, time
assert '--standby' in sys.argv and '--config' in sys.argv and '--overlay-exe' in sys.argv
def say(**v): print(json.dumps(v, ensure_ascii=False), flush=True)
say(type='state', state='loading')
time.sleep(float(os.environ.get('FAKE_RUNNER_LOADING', '0.3')))
say(type='state', state='ready', load_ms=300)
say(type='status', text='收藏程序已就绪 pid=%d' % os.getpid(), tone='ok', log=True)
running = False
for raw in sys.stdin:
    parts = raw.split()
    if parts and parts[0] == 'start' and not running:
        running = True
        say(type='state', state='running')
        say(type='status', text='收到 %s：开始收藏' % parts[2], tone='info', log=True)
        say(type='status', text='符合条件：逐把检查中', tone='info', log=False)
    elif parts == ['stop'] and running:
        running = False
        say(type='status', text='已停止：已按键停止 · 本次新收藏 0 把', tone='warn', log=True)
        say(type='result', run_directory='C:/fake/run')
        say(type='state', state='ready')
)";

using Runner = CollectionHotkeyController::Runner;

void standbyTest(const QString& sourceRoot) {
    const QString python = QDir(sourceRoot).filePath(QStringLiteral(".tools/ocr-runtime/Scripts/python.exe"));
    if (!QFileInfo(python).isFile()) { check(false, "project_python_available_for_runner_protocol_test"); return; }
    QTemporaryDir temporary;
    const QDir root(temporary.path());
    touch(root.filePath("fake_runner.py"), QByteArray(FakeRunner));
    const CollectionRunnerPaths paths{root.absolutePath(), python, root.filePath("fake_runner.py")};
    AppState state;
    state.run.hotkey = QStringLiteral("none");  // a test never registers a global key
    state.configPath = root.filePath("config.json");
    int saves = 0;
    {
        CollectionHotkeyController controller(&state, nullptr, paths);
        check(!controller.hotkeyRegistered(), "invalid_key_registers_nothing");
        controller.setSaveConfiguration([&saves](QString*) { ++saves; return true; });
        controller.preload();
        check(controller.runnerAlive() && controller.runnerState() == Runner::Loading && saves == 0,
              "program_start_preloads_runner_without_starting_a_run");
        check(waitFor([&] { return controller.runnerState() == Runner::Ready; }) && hasLog(state, QStringLiteral("已就绪")),
              "runner_reports_ready_after_loading");
        controller.toggle();
        check(saves == 1 && waitFor([&] { return controller.running(); })
              && waitFor([&] { return hasLog(state, QStringLiteral("收到 none")); }),
              "press_saves_tasks_and_starts_immediately");
        check(!hasLog(state, QStringLiteral("逐把检查中")), "per_card_lines_stay_out_of_the_log");
        controller.toggle();
        check(!hasLog(state, QStringLiteral("当前这一步完成后停止")), "bounce_within_400ms_is_ignored");
        pause();
        controller.toggle();
        check(hasLog(state, QStringLiteral("当前这一步完成后停止")), "second_press_requests_stop");
        check(waitFor([&] { return controller.runnerState() == Runner::Ready; })
              && waitFor([&] { return hasLog(state, QStringLiteral("本次收藏记录")); })
              && hasLog(state, QStringLiteral("已停止：已按键停止")), "run_stops_and_runner_returns_to_ready");
        check(controller.runnerAlive() && logCount(state, QStringLiteral("已就绪")) == 1,
              "engines_stay_loaded_after_a_run");
        pause();
        controller.toggle();
        check(waitFor([&] { return controller.running(); })
              && waitFor([&] { return logCount(state, QStringLiteral("收到 none")) == 2; }),
              "next_press_starts_again_without_reloading");
        controller.shutdown(8000);
        check(!controller.runnerAlive(), "program_exit_stops_the_run_and_the_runner_exits");
        check(!hasLog(state, QStringLiteral("收藏程序已退出")), "planned_exit_is_not_a_warning");
        check(QFileInfo(root.filePath("artifacts/hotkey_runs/runner_stderr.log")).exists(), "runner_stderr_is_kept_in_project_artifacts");
        QString guiLog;
        for (const auto& entry : QDir(root.filePath("artifacts/logs")).entryInfoList({"*.gui.log"}, QDir::Files)) {
            QFile file(entry.absoluteFilePath());
            if (file.open(QIODevice::ReadOnly)) guiLog += QString::fromUtf8(file.readAll());
        }
        check(guiLog.contains("runner launch:") && guiLog.contains("hotkey none pressed") && guiLog.contains("sent start")
              && guiLog.contains("runner state ready") && guiLog.contains("runner exited code=0")
              && guiLog.contains("[gui]"), "program_writes_its_development_log_file");
    }
    qputenv("FAKE_RUNNER_LOADING", "2.0");
    {
        AppState cold;
        cold.run.hotkey = QStringLiteral("none");
        cold.configPath = root.filePath("config.json");
        CollectionHotkeyController controller(&cold, nullptr, paths);
        controller.toggle();
        check(controller.runnerAlive() && hasLog(cold, QStringLiteral("识别模型还在加载")),
              "press_before_preload_launches_and_queues_the_start");
        pause();
        controller.toggle();
        check(hasLog(cold, QStringLiteral("取消开始")) && !controller.running(), "press_while_loading_cancels_the_queued_start");
        pause();
        controller.toggle();
        check(waitFor([&] { return controller.running(); }), "queued_start_runs_once_loading_finishes");
        controller.shutdown(8000);
        check(!controller.runnerAlive(), "cold_runner_exits_with_the_program");
    }
    qunsetenv("FAKE_RUNNER_LOADING");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc > 1) standbyTest(QString::fromLocal8Bit(argv[1]));
    check(hotkeyVirtualKey(QStringLiteral("F1")) == 0x70 && hotkeyVirtualKey(QStringLiteral("F2")) == 0x71
          && hotkeyVirtualKey(QStringLiteral("F12")) == 0x7B, "function_keys_map_to_virtual_keys");
    check(hotkeyVirtualKey(QStringLiteral("F13")) == 0 && hotkeyVirtualKey(QStringLiteral("f2")) == 0
          && hotkeyVirtualKey(QStringLiteral("F0")) == 0 && hotkeyVirtualKey(QString()) == 0, "other_names_are_rejected");

    QTemporaryDir temporary;
    const QDir root(temporary.path());
    touch(root.filePath(".tools/ocr-runtime/Scripts/python.exe"));
    touch(root.filePath("tests/manual/run_collection_hotkey.py"));
    QDir().mkpath(root.filePath("dist/RelinkStudio"));
    QDir().mkpath(root.filePath("build"));
    const auto build = locateCollectionRunner(root.filePath("build"));
    check(build.valid() && QDir::fromNativeSeparators(build.script).endsWith("tests/manual/run_collection_hotkey.py"),
          "build_tree_binary_uses_development_runner");
    touch(root.filePath("dist/RelinkStudio/collection/run_collection_hotkey.py"));
    const auto delivered = locateCollectionRunner(root.filePath("dist/RelinkStudio"));
    check(delivered.valid() && QDir::fromNativeSeparators(delivered.script).endsWith("dist/RelinkStudio/collection/run_collection_hotkey.py")
          && QDir::fromNativeSeparators(delivered.python).endsWith(".tools/ocr-runtime/Scripts/python.exe")
          && QDir(delivered.projectRoot) == root, "delivered_program_uses_its_own_collection_copy");
    QFile::remove(root.filePath(".tools/ocr-runtime/Scripts/python.exe"));
    check(!locateCollectionRunner(root.filePath("dist/RelinkStudio")).valid(), "missing_python_runtime_is_not_started");

    const auto arguments = collectionRunnerArguments(delivered, root.filePath("config.json"), QStringLiteral("F2"),
                                                     root.filePath("dist/RelinkStudio/RelinkStudio.exe"));
    check(arguments.mid(0, 5) == QStringList{QStringLiteral("-B"), QStringLiteral("-X"), QStringLiteral("utf8"),
                                             delivered.script, QStringLiteral("--standby")},
          "runner_starts_in_standby_without_bytecode_writes");
    check(arguments.contains(QStringLiteral("--overlay-exe"))
          && arguments.at(arguments.indexOf(QStringLiteral("--hotkey")) + 1) == QStringLiteral("F2"),
          "runner_arguments_carry_hotkey_and_banner_program");
    std::cout << (failures ? "COLLECTION_HOTKEY_TESTS=FAIL" : "COLLECTION_HOTKEY_TESTS=PASS") << '\n';
    return failures ? 1 : 0;
}
