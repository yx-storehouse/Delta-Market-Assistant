#pragma once

#include <QRect>
#include <QString>

namespace relink::vision {

// No window titles or full process paths are retained. HWND + PID alone are
// insufficient after process reuse, so the process creation timestamp is bound.
struct TargetWindow {
    quintptr hwnd = 0;
    quint32 pid = 0;
    quint64 processCreated = 0;
    QString windowClass;
    QString executableName;
    QRect clientRect;
    QRect monitorRect;
    quintptr monitor = 0;
    int dpi = 96;
    bool valid() const { return hwnd && pid && processCreated && !windowClass.isEmpty(); }
};

TargetWindow bindTargetWindow(quintptr hwnd, quint32 pid, QString* error = nullptr);
// Revalidates identity/viewport before AND after a frame transfer. This DXGI
// backend deliberately requires an unobscured foreground client on one monitor.
QString validateTargetWindow(const TargetWindow& target, bool requireForeground = true);
quintptr foregroundWindow();
bool activateTargetWindow(const TargetWindow& target, QString* error = nullptr);

class ForegroundReturnGuard final {
public:
    explicit ForegroundReturnGuard(TargetWindow returnTo);
    ~ForegroundReturnGuard();
    ForegroundReturnGuard(const ForegroundReturnGuard&) = delete;
    ForegroundReturnGuard& operator=(const ForegroundReturnGuard&) = delete;
    bool restore(QString* error = nullptr);
private:
    TargetWindow m_returnTo;
    bool m_restored = false;
};

// QPC desktop-present timestamps and demand timestamps use exactly this domain.
qint64 captureClockMs();
qint64 qpcTicksToMs(qint64 ticks, qint64 frequency);
QString captureClockDomain();

} // namespace relink::vision
