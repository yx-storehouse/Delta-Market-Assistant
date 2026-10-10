#include "status_overlay.h"
#include "application/vision/target_window.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace relink::overlay {

bool parseStatusMessage(const QByteArray& raw, StatusMessage* out) {
    if (!out || raw.isEmpty() || raw.size() > 4096) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const auto object = document.object();
    StatusMessage message;
    if (object.contains("cmd")) {
        message.command = object["cmd"].toString();
        if (message.command == "quit") {
            message.afterMs = std::clamp(object["after_ms"].toInt(0), 0, 60000);
        } else if (message.command == "monitor") {
            const double hwnd = object["hwnd"].toDouble(0);
            if (!(hwnd > 0) || hwnd > 9007199254740991.0 || std::floor(hwnd) != hwnd) return false;
            message.hwnd = quintptr(hwnd);
        } else if (message.command != "hide") return false;
        *out = message;
        return true;
    }
    const auto text = object["text"].toString().simplified();
    const auto tone = object["tone"].toString(QStringLiteral("info"));
    if (text.isEmpty() || text.size() > 240
        || !QStringList{"info", "ok", "warn", "error"}.contains(tone)) return false;
    message.line = StatusLine{text, tone, std::clamp(object["ttl_ms"].toInt(0), 0, 600000)};
    *out = message;
    return true;
}

namespace {
QColor toneColor(const QString& tone) {
    if (tone == "warn") return QColor(255, 176, 32);
    if (tone == "error") return QColor(255, 77, 79);
    return QColor(72, 227, 106);
}
QFont bannerFont(double scale) {
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setBold(true);
    font.setPixelSize(std::max(10, int(std::lround(24 * scale))));
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}
}

QImage renderStatusBanner(const StatusLine& line, int monitorHeight, int maxWidth) {
    const double scale = std::clamp(monitorHeight / 1440.0, 0.5, 3.0);
    const QFont font = bannerFont(scale);
    const QFontMetrics metrics(font);
    const int height = int(std::lround(44 * scale));
    const int padding = int(std::lround(20 * scale));
    const int border = std::max(1, int(std::lround(2 * scale)));
    const int limit = std::max(4 * padding, maxWidth);
    const QString text = metrics.elidedText(line.text, Qt::ElideRight, limit - 2 * padding);
    const int width = std::min(limit, metrics.horizontalAdvance(text) + 2 * padding);
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const QRectF frame(border / 2.0, border / 2.0, width - border, height - border);
    QPainterPath path;
    path.addRoundedRect(frame, 8 * scale, 8 * scale);
    painter.fillPath(path, QColor(8, 10, 12, 205));
    painter.setPen(QPen(toneColor(line.tone), border));
    painter.drawPath(path);
    painter.setFont(font);
    painter.setPen(QColor(0, 0, 0, 170));
    painter.drawText(QRectF(padding + scale, scale, width - 2 * padding, height), Qt::AlignVCenter | Qt::AlignLeft, text);
    painter.setPen(QColor(250, 250, 250));
    painter.drawText(QRectF(padding, 0, width - 2 * padding, height), Qt::AlignVCenter | Qt::AlignLeft, text);
    painter.end();
    return image;
}

QRect statusBannerRect(const QRect& monitor, const QSize& banner) {
    const double scale = std::clamp(monitor.height() / 1440.0, 0.5, 3.0);
    const int top = monitor.top() + int(std::lround(4 * scale));
    return QRect(monitor.left() + (monitor.width() - banner.width()) / 2, top, banner.width(), banner.height());
}

#ifdef Q_OS_WIN
namespace {
constexpr DWORD OverlayExStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

LRESULT CALLBACK overlayProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    default: return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

class OverlayWindow {
public:
    bool create() {
        const auto instance = GetModuleHandleW(nullptr);
        const auto name = QString::fromLatin1(vision::StatusOverlayWindowClass).toStdWString();
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(WNDCLASSEXW);
        cls.lpfnWndProc = overlayProc;
        cls.hInstance = instance;
        cls.lpszClassName = name.c_str();
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        m_hwnd = CreateWindowExW(OverlayExStyle, name.c_str(), L"Relink Studio status", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
        return m_hwnd != nullptr;
    }
    ~OverlayWindow() { if (m_hwnd) DestroyWindow(m_hwnd); }
    HWND handle() const { return m_hwnd; }
    QRect monitorRect() const {
        HMONITOR monitor = m_monitorHwnd ? MonitorFromWindow(reinterpret_cast<HWND>(m_monitorHwnd), MONITOR_DEFAULTTOPRIMARY)
                                         : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{sizeof(MONITORINFO), {}, {}, 0};
        if (!GetMonitorInfoW(monitor, &info)) return QRect(0, 0, 2560, 1440);
        return QRect(info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
                     info.rcMonitor.bottom - info.rcMonitor.top);
    }
    void setMonitorWindow(quintptr hwnd) { m_monitorHwnd = hwnd; }
    bool present(const QImage& image, bool show) {
        const QRect monitor = monitorRect();
        const QRect target = statusBannerRect(monitor, image.size());
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = image.width();
        info.bmiHeader.biHeight = -image.height();
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        bool ok = false;
        if (bitmap && bits) {
            for (int y = 0; y < image.height(); ++y)
                memcpy(static_cast<uchar*>(bits) + qsizetype(y) * image.width() * 4, image.constScanLine(y), size_t(image.width()) * 4);
            HGDIOBJ previous = SelectObject(memory, bitmap);
            POINT position{target.left(), target.top()};
            SIZE size{image.width(), image.height()};
            POINT source{0, 0};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            ok = UpdateLayeredWindow(m_hwnd, screen, &position, &size, memory, &source, 0, &blend, ULW_ALPHA);
            SelectObject(memory, previous);
        }
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        m_rect = target;
        if (ok && show) {
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            raise();
        }
        return ok;
    }
    void hide() { ShowWindow(m_hwnd, SW_HIDE); }
    void raise() {
        if (IsWindowVisible(m_hwnd))
            SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    }
    QRect rect() const { return m_rect; }
private:
    HWND m_hwnd = nullptr;
    quintptr m_monitorHwnd = 0;
    QRect m_rect;
};

// Overlapped inbound pipe; one writer at a time, many in sequence. Complete
// lines are handed to the Qt thread. No reply is ever written to the client.
class PipeServer {
public:
    explicit PipeServer(std::wstring name) : m_name(std::move(name)), m_stop(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~PipeServer() { stop(); if (m_stop) CloseHandle(m_stop); }
    bool start(std::function<void(QByteArray)> deliver, std::function<void(bool)> connected) {
        HANDLE first = createInstance(true);
        if (first == INVALID_HANDLE_VALUE) return false;
        m_thread = std::thread([this, first, deliver, connected] { run(first, deliver, connected); });
        return true;
    }
    void stop() {
        if (m_stop) SetEvent(m_stop);
        if (m_thread.joinable()) m_thread.join();
    }
private:
    HANDLE createInstance(bool first) {
        return CreateNamedPipeW(m_name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 65536, 0, nullptr);
    }
    bool wait(OVERLAPPED& overlapped, HANDLE pipe, DWORD* transferred) {
        HANDLE events[2] = {overlapped.hEvent, m_stop};
        if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0) { CancelIo(pipe); return false; }
        return GetOverlappedResult(pipe, &overlapped, transferred, FALSE);
    }
    void run(HANDLE pipe, std::function<void(QByteArray)> deliver, std::function<void(bool)> connected) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        while (pipe != INVALID_HANDLE_VALUE && WaitForSingleObject(m_stop, 0) != WAIT_OBJECT_0) {
            ResetEvent(overlapped.hEvent);
            DWORD transferred = 0;
            bool ok = ConnectNamedPipe(pipe, &overlapped);
            const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
            if (!ok && error == ERROR_IO_PENDING) ok = wait(overlapped, pipe, &transferred);
            // A writer that connected, wrote and closed before this call still
            // left its lines in the pipe buffer: read them (ERROR_NO_DATA).
            else if (!ok && (error == ERROR_PIPE_CONNECTED || error == ERROR_NO_DATA)) ok = true;
            if (WaitForSingleObject(m_stop, 0) == WAIT_OBJECT_0) break;
            if (ok) {
                connected(true);
                QByteArray buffer;
                char chunk[4096];
                while (WaitForSingleObject(m_stop, 0) != WAIT_OBJECT_0) {
                    ResetEvent(overlapped.hEvent);
                    DWORD read = 0;
                    bool readOk = ReadFile(pipe, chunk, sizeof(chunk), &read, &overlapped);
                    if (!readOk && GetLastError() == ERROR_IO_PENDING) readOk = wait(overlapped, pipe, &read);
                    if (!readOk || read == 0) break;
                    buffer.append(chunk, int(read));
                    for (int end = buffer.indexOf('\n'); end >= 0; end = buffer.indexOf('\n')) {
                        deliver(buffer.left(end).trimmed());
                        buffer.remove(0, end + 1);
                    }
                    if (buffer.size() > 65536) break;  // A line limit violation drops this client.
                }
                connected(false);
            }
            DisconnectNamedPipe(pipe);
        }
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        CloseHandle(overlapped.hEvent);
    }
    std::wstring m_name;
    HANDLE m_stop = nullptr;
    std::thread m_thread;
};
} // namespace
#endif

int runStatusOverlay(int argc, char** argv) {
    bool selfTest = false;
    for (int i = 1; i < argc; ++i) selfTest |= QByteArray(argv[i]) == "--self-test";
    if (selfTest) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (selfTest) {
        // The offscreen font database does not enumerate Windows fonts.
        const QString fonts = qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/";
        for (const auto& name : {QStringLiteral("msyh.ttc"), QStringLiteral("msyhbd.ttc")})
            QFontDatabase::addApplicationFont(fonts + name);
    }
    QCommandLineParser parser;
    parser.addOptions({{"status-overlay", "Run the top-centre status banner."},
        {"self-test", "Hidden window + private pipe protocol check; never shown."},
        {"pipe-name", "Pipe name override (tests).", "name"},
        {"monitor-of-hwnd", "Show on the monitor of this window.", "hwnd"},
        {"exit-with-client", "Exit 8 s after the last writer disconnects."},
        {"idle-exit-seconds", "Exit after this many seconds without messages.", "seconds", "900"},
        {"no-show", "Verification: serve the real pipe but never show the window."},
        {"preview-dir", "Self-test only: write synthetic banner PNG previews.", "dir"}});
    if (!parser.parse(app.arguments())) { std::fprintf(stderr, "E_STATUS_OVERLAY_ARGUMENTS\n"); return 2; }
#ifndef Q_OS_WIN
    std::fprintf(stderr, "E_PLATFORM_UNSUPPORTED\n");
    return 2;
#else
    const QString pipeName = parser.isSet("pipe-name") ? parser.value("pipe-name") : QString::fromLatin1(StatusOverlayPipeName);
    if (!pipeName.startsWith(QStringLiteral("\\\\.\\pipe\\RelinkStudioStatusOverlay"))) {
        std::fprintf(stderr, "E_STATUS_OVERLAY_PIPE_NAME\n"); return 2;
    }
    OverlayWindow window;
    if (!window.create()) { std::fprintf(stderr, "E_STATUS_OVERLAY_WINDOW\n"); return 1; }
    window.setMonitorWindow(quintptr(parser.value("monitor-of-hwnd").toULongLong()));
    const int idleSeconds = std::clamp(parser.value("idle-exit-seconds").toInt(), 5, 86400);
    const bool noShow = parser.isSet("no-show");
    QTimer ttl, quit, idle, topmost, linger;
    ttl.setSingleShot(true); quit.setSingleShot(true); idle.setSingleShot(true); linger.setSingleShot(true);
    QObject::connect(&quit, &QTimer::timeout, &app, &QCoreApplication::quit);
    QObject::connect(&idle, &QTimer::timeout, &app, &QCoreApplication::quit);
    QObject::connect(&linger, &QTimer::timeout, &app, &QCoreApplication::quit);
    QObject::connect(&ttl, &QTimer::timeout, &app, [&] { window.hide(); });
    QObject::connect(&topmost, &QTimer::timeout, &app, [&] { window.raise(); });
    topmost.start(1500);
    idle.start(idleSeconds * 1000);
    QList<StatusLine> received;
    int malformed = 0;
    const auto handle = [&](const QByteArray& raw) {
        StatusMessage message;
        if (!parseStatusMessage(raw, &message)) { ++malformed; return; }
        idle.start(idleSeconds * 1000);
        if (message.command == "quit") { quit.start(message.afterMs); return; }
        if (message.command == "hide") { ttl.stop(); window.hide(); return; }
        if (message.command == "monitor") { window.setMonitorWindow(message.hwnd); return; }
        received.append(message.line);
        const QRect monitor = window.monitorRect();
        window.present(renderStatusBanner(message.line, monitor.height(), monitor.width() * 7 / 10), !selfTest && !noShow);
        if (message.line.ttlMs > 0) ttl.start(message.line.ttlMs); else ttl.stop();
    };
    PipeServer server(pipeName.toStdWString());
    const bool exitWithClient = parser.isSet("exit-with-client");
    const bool started = server.start(
        [&](QByteArray line) { QMetaObject::invokeMethod(&app, [&, line] { handle(line); }, Qt::QueuedConnection); },
        [&](bool connected) {
            QMetaObject::invokeMethod(&app, [&, connected] {
                if (connected) linger.stop();
                else if (exitWithClient && !quit.isActive()) linger.start(8000);
            }, Qt::QueuedConnection);
        });
    if (!started) {
        // Another overlay already owns the pipe: it keeps serving the writers.
        std::printf("STATUS_OVERLAY_ALREADY_RUNNING\n");
        return 0;
    }
    if (!selfTest) {
        const int status = app.exec();
        server.stop();
        std::printf("STATUS_OVERLAY_EXIT=received:%d;malformed:%d;shown:%d\n", int(received.size()), malformed,
            int(!noShow && !received.isEmpty()));
        return status;
    }
    // Self-test: a private pipe, a hidden window, real overlapped I/O.
    std::atomic<bool> clientOk{false};
    std::thread client([&] {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        for (int i = 0; i < 100 && pipe == INVALID_HANDLE_VALUE; ++i) {
            pipe = CreateFileW(pipeName.toStdWString().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) Sleep(20);
        }
        if (pipe == INVALID_HANDLE_VALUE) return;
        const QByteArray lines = QStringLiteral(
            "{\"text\":\"正在执行收藏任务：筛选皮肤[AS Val突击步枪-黑银先锋] → 筛选成色[成色S] · 价格 10-300\"}\n"
            "not json\n"
            "{\"text\":\"已停止：游戏失去前台\",\"tone\":\"error\",\"ttl_ms\":5000}\n"
            "{\"cmd\":\"quit\",\"after_ms\":50}\n").toUtf8();
        DWORD written = 0;
        clientOk = WriteFile(pipe, lines.constData(), DWORD(lines.size()), &written, nullptr) && written == DWORD(lines.size());
        CloseHandle(pipe);
    });
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &app, &QCoreApplication::quit);
    guard.start(10000);
    app.exec();
    client.join();
    server.stop();
    const LONG_PTR style = GetWindowLongPtrW(window.handle(), GWL_EXSTYLE);
    const QRect monitor(0, 0, 2560, 1440);
    const auto banner = renderStatusBanner(StatusLine{QStringLiteral("筛选条件：成色S 磨损 0.207886 ≤ 5 · 价格 230 ∈ 10-300 → 已点收藏")}, 1440, 1792);
    const QRect placed = statusBannerRect(monitor, banner.size());
    const bool allowed = vision::statusOverlayWindowAllowed(QString::fromLatin1(vision::StatusOverlayWindowClass),
        quint32(style), QStringLiteral("RelinkStudio.exe"), placed, monitor);
    const bool styles = (DWORD(style) & OverlayExStyle) == OverlayExStyle && !IsWindowVisible(window.handle());
    const bool protocol = clientOk && received.size() == 2 && malformed == 1 && received[1].tone == "error"
        && received[0].text.contains(QStringLiteral("黑银先锋"));
    const bool rendering = banner.height() == 44 && banner.width() > 200 && banner.width() <= 1792
        && qAlpha(banner.pixel(0, 0)) == 0 && qAlpha(banner.pixel(banner.width() / 2, banner.height() / 2)) > 150;
    if (parser.isSet("preview-dir")) {
        QDir().mkpath(parser.value("preview-dir"));
        const QList<StatusLine> samples{
            {QStringLiteral("正在执行收藏任务：筛选皮肤[AS Val突击步枪-黑银先锋] → 筛选成色[成色S] · 价格 10-300 · 磨损 ≤ 5")},
            {QStringLiteral("符合条件：成色S 磨损 0.207886 ≤ 5 · 价格 230 ∈ 10-300 → 已点收藏")},
            {QStringLiteral("收藏成功：AS Val突击步枪-黑银先锋 230 · 本条 12 把 · 本轮 60 把"), QStringLiteral("ok")},
            {QStringLiteral("完成：P90冲锋枪-黑银先锋 读到 310 > 300，本条收藏 28 把"), QStringLiteral("ok")},
            {QStringLiteral("已停止：检测到鼠标被移动，未发送点击"), QStringLiteral("error")}};
        int index = 0;
        for (const auto& sample : samples)
            renderStatusBanner(sample, 1440, 1792).save(QDir(parser.value("preview-dir")).filePath(
                QStringLiteral("status_banner_%1.png").arg(++index)));
    }
    const bool passed = styles && protocol && rendering && allowed;
    std::printf("STATUS_OVERLAY_SELF_TEST=%s; styles=%d; protocol=%d; rendering=%d; capture_band=%d; shown=0\n",
        passed ? "PASS" : "FAIL", styles, protocol, rendering, allowed);
    return passed ? 0 : 1;
#endif
}

} // namespace relink::overlay
