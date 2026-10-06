#pragma once

#include <QString>

class QApplication;
class MainWindow;
class AppState;
namespace relink::workspace { class WorkspaceController; }

namespace relink::diagnostics {

// Startup resolves paths and configures the offscreen platform before invoking
// the runner. The diagnostic runner does not parse or mutate CLI arguments.
struct UiSelfTestOptions {
    QString configPath;
    QString snapshotDirectory;
    bool runInteractionChecks = false;
};

// Runs the existing packaged geometry/render checks and optional interactions.
// This call owns its application event loop and returns only after checks exit;
// app, window, state and workspace must remain alive for the duration of the call.
// Keep this runner linked into the desktop executable for --self-test and
// --snapshot-dir compatibility; extraction does not yet reduce release size.
int runUiSelfTest(QApplication& app, MainWindow& window, AppState& state,
                  workspace::WorkspaceController& workspace, const UiSelfTestOptions& options);

} // namespace relink::diagnostics
