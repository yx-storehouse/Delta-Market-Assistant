#pragma once

namespace relink::diagnostics {
// Opt-in, read-only command-line diagnostic. Does not construct the application
// UI, touch the workspace, dispatch a trade, or write a screenshot.
int runLiveCaptureCheck(int argc, char** argv);
}
