#pragma once
#include <cstdint>
#include <functional>
#include <utility>

namespace relink::diagnostics {
// A probe never owns foreground transitions inside an enclosing batch.
// Standalone probes retain the game through OCR and restore only at the end.
class ForegroundPolicy final {
public:
    using Handle = std::uintptr_t;
    using Current = std::function<Handle()>;
    using Switch = std::function<bool()>;
    ForegroundPolicy(bool callerOwned, Handle target, Handle ide, Current current, Switch activate, Switch restore)
        : m_callerOwned(callerOwned), m_target(target), m_ide(ide), m_current(std::move(current)),
          m_activate(std::move(activate)), m_restore(std::move(restore)) {}
    ForegroundPolicy(const ForegroundPolicy&) = delete;
    ForegroundPolicy& operator=(const ForegroundPolicy&) = delete;
    ~ForegroundPolicy() { if (m_started && !m_finished) finish(); }
    bool begin() {
        if (m_started) return false;
        m_started = true;
        if (m_callerOwned) return m_current() == m_target;
        if (m_current() == m_target) return true;
        ++activationRequests;
        return m_activate() && m_current() == m_target;
    }
    bool finish() {
        if (m_finished) return m_finishResult;
        m_finished = true;
        if (!m_started) return m_finishResult = false;
        if (m_callerOwned) return m_finishResult = m_current() == m_target;
        if (m_current() == m_ide) return m_finishResult = true;
        ++restoreRequests;
        return m_finishResult = m_restore() && m_current() == m_ide;
    }
    int activationRequests = 0;
    int restoreRequests = 0;
private:
    bool m_callerOwned, m_started = false, m_finished = false, m_finishResult = false;
    Handle m_target, m_ide;
    Current m_current;
    Switch m_activate, m_restore;
};
}
