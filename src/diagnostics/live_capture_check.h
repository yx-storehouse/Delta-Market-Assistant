#pragma once

namespace relink::diagnostics {
// Opt-in, read-only command-line diagnostic. Does not construct the application
// UI, touch the workspace, dispatch a trade, or write a screenshot.
int runLiveCaptureCheck(int argc, char** argv);
// JSONL read-only requests under an existing caller-owned foreground batch.
// Reuses OCR and optionally a bounded latest-frame producer. Every delivered
// frame still has a presentation timestamp after its own request; leases are
// request-local and no image is persisted or sent unsolicited over the pipe.
int runLiveCaptureServer(int argc, char** argv);
}
