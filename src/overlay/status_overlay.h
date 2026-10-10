#pragma once

#include <QImage>
#include <QJsonObject>
#include <QString>

namespace relink::overlay {

// One status line as drawn by the top-centre banner. Tone selects only the
// border colour: info/ok green, warn amber, error red.
struct StatusLine {
    QString text;
    QString tone = QStringLiteral("info");
    int ttlMs = 0;  // 0 keeps the line until the next one.
};

// Parse one JSON-lines message. Returns false for anything malformed; the
// overlay ignores it. Commands: {"cmd":"quit","after_ms":N},
// {"cmd":"hide"}, {"cmd":"monitor","hwnd":N}; otherwise {"text":...}.
struct StatusMessage {
    QString command;  // empty for a status line
    StatusLine line;
    int afterMs = 0;
    quintptr hwnd = 0;
};
bool parseStatusMessage(const QByteArray& line, StatusMessage* out);

// Premultiplied ARGB banner sized for a monitor of the given physical height
// (1440 px is the reference). The image never exceeds maxWidth; long text is
// elided. Pure rendering: no window, no file.
QImage renderStatusBanner(const StatusLine& line, int monitorHeight, int maxWidth);

// Physical geometry: top-centre of the monitor, inside the top 4% band that
// the capture pipeline tolerates (see vision::statusOverlayWindowAllowed).
QRect statusBannerRect(const QRect& monitor, const QSize& banner);

inline constexpr char StatusOverlayPipeName[] = "\\\\.\\pipe\\RelinkStudioStatusOverlay";

// --status-overlay [--self-test] [--pipe-name NAME] [--monitor-of-hwnd N]
//   [--exit-with-client] [--idle-exit-seconds N] [--preview-dir DIR]
int runStatusOverlay(int argc, char** argv);

} // namespace relink::overlay
