#pragma once

#include <QString>

class AppState;

namespace relink::application {

// Resolve the writable configuration destination. An invalid existing file is
// kept intact and redirected to a fresh recovery name before any later save.
// loadExisting=false preserves the headless diagnostic startup behavior.
QString prepareStartupConfig(AppState& state, const QString& requested, bool loadExisting);

} // namespace relink::application
