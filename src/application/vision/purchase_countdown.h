#pragma once

#include "application/runtime/observation/observation_adapter.h"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QRegularExpression>
#include <QString>
#include <QVector>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace relink::vision {
// The countdown line above the detail buy button (2560x1440). Live
// 2026-10-09: "0分10秒后解锁购买" x 2096..2282, y 1171..1192, centred on the
// button (x 1978..2371); after the public notice "剩余：2天23小时" takes the
// same line. Light text (~220) on a flat dark panel (~30); the clock icon
// left of the text is static.
inline const QRect PurchaseCountdownRoi(1990, 1160, 380, 40);
// The buy button below the line (live05_preview: x 1978..2371, y 1206..1280).
// User 2026-10-09: within the last ~10 s the 公示中 button turns into the
// price, and the purchase dialog can already be opened. White 公示中 text to a
// red price removes most bright strokes: far more than this many pixels.
inline const QRect PurchaseBuyButtonRect(1978, 1206, 393, 74);
// The 外观购买 dialog repeats the line ("0分3秒后解锁购买", user screenshot
// 2026-10-09) centred above its own price button; dialog03 read "0剩余：2天23
// 小时" there at x 1527..1724, y 850..873 and the button text at y 924..949.
inline const QRect PurchaseDialogCountdownRoi(1430, 842, 395, 40);
inline const QRect PurchaseDialogButtonRect(1428, 896, 397, 76);
// The result toast after a press, watched frame by frame for the server's
// answer (user 2026-10-10: its timing measures this machine's server
// latency). It slides up into place from about 40 px below its rest band
// (y ~200-250; review wf_cf31784b-664), so the watch covers y 150-299: above
// the dialog title 外观购买 (y ~371), over only static labels on 我的关注.
inline const QRect PurchaseToastRect(896, 150, 768, 150);
struct CountdownArea {
    QString name;
    QRect line, button;
};
inline bool countdownArea(const QString& name, CountdownArea* area) {
    if (name.isEmpty() || name == QStringLiteral("footer")) {
        *area = {QStringLiteral("footer"), PurchaseCountdownRoi, PurchaseBuyButtonRect};
        return true;
    }
    if (name == QStringLiteral("dialog")) {
        *area = {QStringLiteral("dialog"), PurchaseDialogCountdownRoi, PurchaseDialogButtonRect};
        return true;
    }
    if (name == QStringLiteral("toast")) {
        *area = {QStringLiteral("toast"), PurchaseToastRect, PurchaseDialogButtonRect};
        return true;
    }
    return false;
}
// User screenshot 2026-10-09: 公示中 (white on dark) turns into a green filled
// button with the price; that is a level change (dark -> fill) over most of
// the button, not a bright/dark swap. A static button changes no pixel.
inline constexpr int PurchaseButtonChangePixels = 200;
// Share of green fill that counts as the green price button.
inline constexpr double PurchaseButtonGreenFill = 0.4;
// Frames still examined after a button change before the watch ends, so the
// final (fully read) frame shows the settled button.
inline constexpr int PurchaseButtonSettleFrames = 2;
// The countdown line as read by the watch's line OCR: seconds left,
// CountdownLineUnread, or CountdownLineRemaining ("剩余…": past the notice).
// Minutes are taken from the two digits right before 分, so a clock icon
// read as a leading digit ("626分0秒") still gives 26. With a single-digit
// minute the icon read as 6 makes 60..69 ("62分45秒" for 2分45秒, live
// clock06/jump02, wf_8d55a691-779): minutes never reach 60, so the 6 is dropped.
inline constexpr int CountdownLineUnread = -1;
inline constexpr int CountdownLineRemaining = -2;
inline int countdownLineSeconds(QString text) {
    text.remove(QRegularExpression(QStringLiteral("\\s+")));
    if (text.contains(QStringLiteral("剩余"))) return CountdownLineRemaining;
    if (!text.contains(QStringLiteral("后解锁")) || text.contains(QStringLiteral("小时"))) return CountdownLineUnread;
    static const QRegularExpression pattern(QStringLiteral("(\\d{1,2})分(\\d{1,2})秒"));
    const auto match = pattern.match(text);
    if (!match.hasMatch()) return CountdownLineUnread;
    int minutes = match.captured(1).toInt();
    const int seconds = match.captured(2).toInt();
    if (minutes >= 60 && minutes < 70) minutes -= 60;
    if (minutes >= 60 || seconds >= 60) return CountdownLineUnread;
    return minutes * 60 + seconds;
}
// The line's words without a clock icon read as a character, by position
// (as purchase_observation.strip_line_icon): the words centred as icon+text,
// the first character dropped (its own word, or the start of the first word),
// the rest then centred as the text alone. Otherwise the words as read.
struct CountdownLineLayout { double groupX, textX, advance; };
inline CountdownLineLayout countdownLineLayout(const QString& area) {
    return area == QStringLiteral("dialog") ? CountdownLineLayout{1625.5, 1640.0, 28.0}
                                            : CountdownLineLayout{2176.5, 2189.5, 24.6};
}
inline QString countdownLineWithoutIcon(const QJsonArray& words, const CountdownLineLayout& layout) {
    struct Word { QString text; double x, width; };
    QVector<Word> ordered;
    for (const auto& value : words) {
        const auto w = value.toObject();
        ordered.append({w["text"].toString(), w["x"].toDouble(), w["width"].toDouble()});
    }
    std::sort(ordered.begin(), ordered.end(), [](const Word& a, const Word& b) { return a.x < b.x; });
    const auto joined = [](const QVector<Word>& list) {
        QString text;
        for (const auto& word : list) text += word.text;
        return text;
    };
    const auto centre = [](const QVector<Word>& list) {
        double left = list.first().x, right = list.first().x + list.first().width;
        for (const auto& word : list) { left = std::min(left, word.x); right = std::max(right, word.x + word.width); }
        return (left + right) / 2.0;
    };
    if (ordered.size() < 2 || std::abs(centre(ordered) - layout.groupX) > 5) return joined(ordered);
    QVector<Word> kept = ordered.mid(1);
    const auto& first = ordered.first();
    if (first.text.size() != 1) {
        if (first.width <= layout.advance + 4) return joined(ordered);
        kept.prepend({first.text.mid(1), first.x + layout.advance, first.width - layout.advance});
    }
    if (kept.isEmpty() || std::abs(centre(kept) - layout.textX) > 5) return joined(ordered);
    return joined(kept);
}
// The line of ANOTHER listing (user 2026-10-10: the selection changed and the
// follow took long to notice): a readable value more than 1.5 s away from the
// last readable one minus the time since it, or the remaining-time line where
// seconds were still left. The elapsed time matters: reads in between may be
// unreadable (live jump02: "63分16秒" for 3分16秒, the icon read as 6, for 7 s).
// The first readable line of a follow watch against the caller's followed
// zero (QPC ms; the display shows about ceil(remaining)): another listing
// selected between two watches (review wf_f9f407ac-b6e). 2.5 s allows the
// display rounding, a whole-second re-sync and a single-read zero.
inline bool countdownLineOffClock(double zeroMs, int current, qint64 currentMs) {
    const double expected = (zeroMs - currentMs) / 1000.0 + 0.5;
    if (current == CountdownLineRemaining) return expected > 2.5;
    if (current < 0) return false;
    return std::abs(current - expected) > 2.5;
}
inline bool countdownLineJumped(int previous, qint64 previousMs, int current, qint64 currentMs) {
    if (previous < 0) return false;
    const double expected = previous - std::max<qint64>(0, currentMs - previousMs) / 1000.0;
    if (current == CountdownLineRemaining) return expected > 1.5;
    if (current < 0) return false;
    return std::abs(current - expected) > 1.5;
}

// A static line has no strong change at all (anti-aliased edges never cross
// both thresholds). Live 2026-10-09 clock03: 9->8 and 6->5 changed fewer than
// 12 strong pixels and were caught a second late; a change that leaves the
// read value unchanged is dropped as a non-boundary in Python.
inline constexpr int PurchaseCountdownChangePixels = 4;
inline constexpr int PurchaseCountdownInkBright = 150;
inline constexpr int PurchaseCountdownInkDark = 70;

// Per pixel: 2 = bright ink, 0 = dark panel, 1 = in between (never counted).
struct CountdownInk {
    QByteArray levels;
    int bright = 0;
    bool valid = false;
};

inline CountdownInk countdownInk(const runtime::observation::FrameEnvelope& frame, const QRect& roi) {
    CountdownInk ink;
    if (frame.width <= 0 || frame.height <= 0 || frame.strideBytes < frame.width * 4
        || frame.pixels.size() < qint64(frame.strideBytes) * frame.height
        || !QRect(0, 0, frame.width, frame.height).contains(roi) || roi.isEmpty()) return ink;
    ink.levels.resize(qsizetype(roi.width()) * roi.height());
    auto* out = ink.levels.data();
    for (int y = roi.top(); y <= roi.bottom(); ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(frame.pixels.constData())
            + qint64(y) * frame.strideBytes + qint64(roi.left()) * 4;
        for (int x = 0; x < roi.width(); ++x, row += 4) {
            const int luminance = (int(row[0]) * 29 + int(row[1]) * 150 + int(row[2]) * 77) >> 8;
            const char level = luminance >= PurchaseCountdownInkBright ? 2 : luminance <= PurchaseCountdownInkDark ? 0 : 1;
            ink.bright += level == 2;
            *out++ = level;
        }
    }
    ink.valid = true;
    return ink;
}

// Pixels that turned from bright ink to dark panel or back; -1 if the two
// reads are not comparable.
inline int countdownStrongDifference(const CountdownInk& a, const CountdownInk& b) {
    if (!a.valid || !b.valid || a.levels.size() != b.levels.size()) return -1;
    int changed = 0;
    for (qsizetype i = 0; i < a.levels.size(); ++i) {
        const char p = a.levels[i], q = b.levels[i];
        changed += (p == 2 && q == 0) || (p == 0 && q == 2);
    }
    return changed;
}

// The line starts with a clock icon: a filled light disc (~20 px, far more
// ink than any glyph). Live 2026-10-09 clock03: line OCR read it as a digit,
// "6" + "26分0秒" -> "626分0秒". Read only the text right of it. Without a
// recognised icon the whole line is read and reported as such.
struct CountdownTextRegion {
    QRect region;
    bool iconExcluded = false;
    QRect icon;
    // First ink run on the text rows, for calibration: live clock04/05 never
    // accepted the countdown-state icon, and the reason was not recorded.
    int runStart = -1, runEnd = -1, nextRun = -1, runPixels = 0, runHeight = 0;
};

inline CountdownTextRegion countdownTextRegion(const CountdownInk& ink, const QRect& roi) {
    CountdownTextRegion out{roi};
    if (!ink.valid || ink.levels.size() != qsizetype(roi.width()) * roi.height()) return out;
    QVector<int> column(roi.width(), 0);
    int top = roi.height(), bottom = -1;
    // Text rows only (client y 1168..1196): nothing above or below the line.
    const int firstRow = std::max(0, 8), lastRow = std::min(roi.height(), 37);
    for (int y = firstRow; y < lastRow; ++y)
        for (int x = 0; x < roi.width(); ++x)
            if (ink.levels[qsizetype(y) * roi.width() + x] == 2) ++column[x];
    int start = 0;
    while (start < roi.width() && !column[start]) ++start;
    int end = start;
    while (end < roi.width() && column[end]) ++end;          // [start, end): first ink run
    int next = end;
    while (next < roi.width() && !column[next]) ++next;      // first column of the text
    if (start >= roi.width()) return out;
    int pixels = 0;
    for (int x = start; x < end; ++x) pixels += column[x];
    for (int y = firstRow; y < lastRow; ++y)
        for (int x = start; x < end; ++x)
            if (ink.levels[qsizetype(y) * roi.width() + x] == 2) { top = std::min(top, y); bottom = std::max(bottom, y); }
    const int width = end - start, height = bottom - top + 1;
    out.runStart = roi.x() + start; out.runEnd = roi.x() + end; out.nextRun = next < roi.width() ? roi.x() + next : -1;
    out.runPixels = pixels; out.runHeight = height;
    if (next >= roi.width() || next - end < 2) return out;
    // A disc fills ~3/4 of its box; glyph strokes (even two touching
    // digits) fill well under half of theirs.
    if (width < 14 || width > 30 || height < 14 || height > 32 || pixels < 150
        || pixels * 2 < width * height) return out;
    const int left = roi.x() + (end + next) / 2;
    out.region = QRect(left, roi.y(), roi.x() + roi.width() - left, roi.height());
    out.iconExcluded = true;
    out.icon = QRect(roi.x() + start, roi.y() + top, width, height);
    return out;
}

// Share of the button's pixels that are the green fill of a price button
// (user screenshot 2026-10-09: the watchlist button turns solid green with the
// price in the last seconds; 公示中 is white text on a dark button). Positive
// evidence, independent of OCR; -1 if the frame cannot be read.
inline double buttonGreenFraction(const runtime::observation::FrameEnvelope& frame, const QRect& rect) {
    if (frame.width <= 0 || frame.height <= 0 || frame.strideBytes < frame.width * 4
        || frame.pixels.size() < qint64(frame.strideBytes) * frame.height
        || !QRect(0, 0, frame.width, frame.height).contains(rect) || rect.isEmpty()) return -1;
    qint64 green = 0;
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const auto* p = reinterpret_cast<const unsigned char*>(frame.pixels.constData())
            + qint64(y) * frame.strideBytes + qint64(rect.left()) * 4;
        for (int x = 0; x < rect.width(); ++x, p += 4)
            green += p[1] >= 110 && p[1] - p[2] >= 45 && p[1] - p[0] >= 10;
    }
    return double(green) / (double(rect.width()) * rect.height());
}

// Mean blue, green, red of a rectangle, to calibrate the green fill (live
// preentry01: the changed button measured only 3 % by the rule above).
inline QJsonArray rectMeanBgr(const runtime::observation::FrameEnvelope& frame, const QRect& rect) {
    if (frame.width <= 0 || frame.height <= 0 || frame.strideBytes < frame.width * 4
        || frame.pixels.size() < qint64(frame.strideBytes) * frame.height
        || !QRect(0, 0, frame.width, frame.height).contains(rect) || rect.isEmpty()) return {};
    double sum[3] = {0, 0, 0};
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const auto* p = reinterpret_cast<const unsigned char*>(frame.pixels.constData())
            + qint64(y) * frame.strideBytes + qint64(rect.left()) * 4;
        for (int x = 0; x < rect.width(); ++x, p += 4) { sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; }
    }
    const double n = double(rect.width()) * rect.height();
    return {sum[0] / n, sum[1] / n, sum[2] / n};
}

// Pixels whose ink level (dark / between / bright) changed; -1 if the two
// reads are not comparable. Used for the button, whose fill changes.
inline int countdownLevelDifference(const CountdownInk& a, const CountdownInk& b) {
    if (!a.valid || !b.valid || a.levels.size() != b.levels.size()) return -1;
    int changed = 0;
    for (qsizetype i = 0; i < a.levels.size(); ++i) changed += a.levels[i] != b.levels[i];
    return changed;
}

// The same instant on the capture clock (QPC, as source_mono_ms) and on the
// system clock (Unix ms). Read only; the system time is never changed.
inline QJsonObject purchaseClockPair() {
#ifdef Q_OS_WIN
    LARGE_INTEGER before{}, after{}, frequency{};
    FILETIME system{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&before);
    GetSystemTimePreciseAsFileTime(&system);
    QueryPerformanceCounter(&after);
    if (frequency.QuadPart <= 0) return {};
    const double qpcMs = (double(before.QuadPart) + double(after.QuadPart)) / 2.0 * 1000.0 / double(frequency.QuadPart);
    const quint64 ticks = (quint64(system.dwHighDateTime) << 32) | system.dwLowDateTime;
    // FILETIME counts 100 ns since 1601-01-01; Unix epoch is 11644473600 s later.
    const double unixMs = (double(ticks) - 116444736000000000.0) / 10000.0;
    return {{"qpc_ms", qpcMs}, {"unix_ms", unixMs},
        {"read_span_ms", double(after.QuadPart - before.QuadPart) * 1000.0 / double(frequency.QuadPart)}};
#else
    return {};
#endif
}
}
