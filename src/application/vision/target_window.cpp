#include "target_window.h"

#include <QFileInfo>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace relink::vision {
namespace {
void setError(QString* error, const QString& code) { if (error) *error = code; }
#ifdef Q_OS_WIN
class DpiScope {
public:
    DpiScope() : previous(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~DpiScope() { if (previous) SetThreadDpiAwarenessContext(previous); }
    DPI_AWARENESS_CONTEXT previous;
};
QRect rect(const RECT& r) { return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top); }
bool cloaked(HWND hwnd) {
    DWORD value = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &value, sizeof(value))) && value;
}
bool identity(const TargetWindow& t) {
    const auto w = reinterpret_cast<HWND>(t.hwnd);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!t.valid() || !IsWindow(w) || pid != t.pid) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    const bool times = GetProcessTimes(process, &created, &exited, &kernel, &user);
    CloseHandle(process);
    const quint64 stamp = (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    wchar_t name[256]{};
    return alive && times && stamp == t.processCreated && GetClassNameW(w, name, 256)
        && QString::fromWCharArray(name) == t.windowClass;
}
#endif
}

TargetWindow bindTargetWindow(quintptr hwnd, quint32 expectedPid, QString* error) {
    setError(error, {});
    TargetWindow t;
#ifdef Q_OS_WIN
    DpiScope dpi;
    const auto w = reinterpret_cast<HWND>(hwnd);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!hwnd || !expectedPid || !IsWindow(w) || GetAncestor(w, GA_ROOT) != w || pid != expectedPid) {
        setError(error, QStringLiteral("E_WINDOW_IDENTITY")); return {};
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) { setError(error, QStringLiteral("E_PROCESS_QUERY")); return {}; }
    FILETIME created{}, exited{}, kernel{}, user{};
    wchar_t path[32768]{};
    DWORD size = 32768;
    const bool good = GetProcessTimes(process, &created, &exited, &kernel, &user)
        && QueryFullProcessImageNameW(process, 0, path, &size)
        && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    if (!good) { setError(error, QStringLiteral("E_PROCESS_QUERY")); return {}; }
    wchar_t name[256]{};
    RECT client{};
    POINT origin{};
    const HMONITOR monitor = MonitorFromWindow(w, MONITOR_DEFAULTTONULL);
    MONITORINFO info{sizeof(MONITORINFO), {}, {}, 0};
    if (!dpi.previous || !GetClassNameW(w, name, 256) || !GetClientRect(w, &client)
        || !ClientToScreen(w, &origin) || !monitor || !GetMonitorInfoW(monitor, &info)) {
        setError(error, QStringLiteral("E_WINDOW_GEOMETRY")); return {};
    }
    t.hwnd = hwnd; t.pid = pid;
    t.processCreated = (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    t.windowClass = QString::fromWCharArray(name);
    t.executableName = QFileInfo(QString::fromWCharArray(path, size)).fileName();
    t.clientRect = QRect(origin.x, origin.y, client.right, client.bottom);
    t.monitorRect = rect(info.rcMonitor); t.monitor = reinterpret_cast<quintptr>(monitor);
    t.dpi = static_cast<int>(GetDpiForWindow(w));
#else
    Q_UNUSED(hwnd); Q_UNUSED(expectedPid);
    setError(error, QStringLiteral("E_PLATFORM_UNSUPPORTED"));
#endif
    return t;
}

QString validateTargetWindow(const TargetWindow& t, bool requireForeground) {
#ifdef Q_OS_WIN
    DpiScope dpi;
    if (!identity(t)) return QStringLiteral("E_WINDOW_IDENTITY");
    const auto w = reinterpret_cast<HWND>(t.hwnd);
    if (!IsWindowVisible(w) || IsIconic(w) || cloaked(w)) return QStringLiteral("E_WINDOW_NOT_VISIBLE");
    if (requireForeground && GetForegroundWindow() != w) return QStringLiteral("E_WINDOW_NOT_FOREGROUND");
    QString error;
    const TargetWindow now = bindTargetWindow(t.hwnd, t.pid, &error);
    if (!error.isEmpty()) return error;
    if (now.processCreated != t.processCreated || now.windowClass != t.windowClass)
        return QStringLiteral("E_WINDOW_IDENTITY");
    if (now.clientRect != t.clientRect || now.monitor != t.monitor || now.monitorRect != t.monitorRect || now.dpi != t.dpi)
        return QStringLiteral("E_VIEWPORT_CHANGED");
    if (t.clientRect.isEmpty() || !t.monitorRect.contains(t.clientRect)) return QStringLiteral("E_WINDOW_OFF_MONITOR");
    if (t.clientRect.width() > 8192 || t.clientRect.height() > 8192
        || qint64(t.clientRect.width()) * t.clientRect.height() * 4 > 134217728)
        return QStringLiteral("E_FRAME_SIZE_LIMIT");
    if (requireForeground) {
        // Do not accept an always-on-top overlay, popup, taskbar or another
        // app's pixels. DWM extended bounds omit invisible resize shadows.
        HWND other = GetWindow(w, GW_HWNDPREV);
        int count = 0;
        for (; other && count < 2048; other = GetWindow(other, GW_HWNDPREV), ++count) {
            if (!IsWindowVisible(other) || IsIconic(other) || cloaked(other)) continue;
            RECT bounds{};
            if (FAILED(DwmGetWindowAttribute(other, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))
                && !GetWindowRect(other, &bounds)) return QStringLiteral("E_WINDOW_OCCLUSION_UNKNOWN");
            if (t.clientRect.intersects(rect(bounds))) return QStringLiteral("E_WINDOW_OCCLUDED");
        }
        if (other) return QStringLiteral("E_WINDOW_OCCLUSION_UNKNOWN");
    }
    return {};
#else
    Q_UNUSED(t); Q_UNUSED(requireForeground);
    return QStringLiteral("E_PLATFORM_UNSUPPORTED");
#endif
}

quintptr foregroundWindow() {
#ifdef Q_OS_WIN
    return reinterpret_cast<quintptr>(GetForegroundWindow());
#else
    return 0;
#endif
}

bool activateTargetWindow(const TargetWindow& t, QString* error) {
    setError(error, {});
#ifdef Q_OS_WIN
    if (!identity(t)) { setError(error, QStringLiteral("E_WINDOW_IDENTITY")); return false; }
    const auto w = reinterpret_cast<HWND>(t.hwnd);
    if (GetForegroundWindow() == w) return true;
    if (IsIconic(w)) ShowWindowAsync(w, SW_RESTORE);
    // A QCoreApplication-only diagnostic has not necessarily created a Win32
    // message queue. AttachThreadInput fails without one; create it explicitly.
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    const DWORD current = GetCurrentThreadId();
    const DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const bool attached = fg && fg != current && AttachThreadInput(current, fg, TRUE);
    const DWORD destination = GetWindowThreadProcessId(w, nullptr);
    const bool targetAttached = destination && destination != current && destination != fg
        && AttachThreadInput(current, destination, TRUE);
    SetForegroundWindow(w);
    if (targetAttached) AttachThreadInput(current, destination, FALSE);
    if (attached) AttachThreadInput(current, fg, FALSE);
    for (int i = 0; i < 20; ++i) {
        if (GetForegroundWindow() == w) return true;
        Sleep(10);
    }
    setError(error, QStringLiteral("E_FOREGROUND_ACTIVATION"));
#else
    Q_UNUSED(t);
    setError(error, QStringLiteral("E_PLATFORM_UNSUPPORTED"));
#endif
    return false;
}

ForegroundReturnGuard::ForegroundReturnGuard(TargetWindow returnTo) : m_returnTo(std::move(returnTo)) {}
ForegroundReturnGuard::~ForegroundReturnGuard() { if (!m_restored) restore(); }
bool ForegroundReturnGuard::restore(QString* error) {
    m_restored = activateTargetWindow(m_returnTo, error) && foregroundWindow() == m_returnTo.hwnd;
    return m_restored;
}

qint64 qpcTicksToMs(qint64 ticks, qint64 frequency) {
    if (ticks < 0 || frequency <= 0 || frequency > 1000000000000LL || ticks / frequency > 9007199254740LL) return -1;
    const qint64 result = (ticks / frequency) * 1000 + ((ticks % frequency) * 1000) / frequency;
    return result <= 9007199254740991LL ? result : -1;
}
qint64 captureClockMs() {
#ifdef Q_OS_WIN
    LARGE_INTEGER count{}, frequency{};
    if (!QueryPerformanceCounter(&count) || !QueryPerformanceFrequency(&frequency)) return -1;
    return qpcTicksToMs(count.QuadPart, frequency.QuadPart);
#else
    return -1;
#endif
}
QString captureClockDomain() { return QStringLiteral("win32:qpc:milliseconds"); }

} // namespace relink::vision
