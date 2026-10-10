#pragma once
#include "dxgi_observation_source.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace relink::vision {
// One replaceable, immutable pending frame, not a FIFO. Capture and OCR have
// separate lifetimes; no pixels are persisted. A consumer still requires a
// presentation AFTER its own request barrier, never a convenient old image.
class LatestFramePump final {
public:
    using Pull = std::function<runtime::observation::CaptureReply()>;
    using Cancel = std::function<void()>;
    using Clock = std::function<qint64()>;
    LatestFramePump(Pull pull, Cancel cancel, Clock clock = captureClockMs);
    ~LatestFramePump();
    void start();
    void stop();
    runtime::observation::CaptureReply take(qint64 barrier, qint64 deadline,
        qint64 maxAge, const std::atomic<bool>& cancelled);
    void wake();
    QJsonObject metrics() const;
private:
    void run();
    Pull m_pull;
    Cancel m_cancel;
    Clock m_clock;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::thread m_thread;
    runtime::observation::FrameEnvelope m_latest;
    QString m_error, m_lastFrameId;
    qint64 m_lastSource = -1, m_idleDeadline = -1;
    quint64 m_produced = 0, m_replaced = 0, m_consumed = 0, m_stale = 0;
    bool m_started = false, m_stopped = false, m_hasLatest = false, m_cancelIssued = false;
};

// Owns the duplication on its producer thread. No synchronous duplication
// may coexist for the same output; callers discard the old serial owner first.
class DxgiFrameStream final {
public:
    DxgiFrameStream();
    ~DxgiFrameStream();
    QString ensure(const TargetWindow& target);
    void stop();
    LatestFramePump* pump();
    QJsonObject metrics() const;
private:
    struct State;
    std::unique_ptr<State> m_state;
};

// New demand/lease metadata represents delivery of real captured pixels. The
// acquisition ID and every source/capture timestamp remain untouched.
class LatestFrameObservationSource final : public runtime::observation::IObservationSource {
public:
    LatestFrameObservationSource(LatestFramePump& pump, TargetWindow target,
        runtime::observation::ObservationDemand demand,
        std::function<QString(const TargetWindow&)> validate = [](const TargetWindow& t) { return validateTargetWindow(t); });
    runtime::observation::CaptureReply capture(const runtime::observation::CaptureRequest& request) override;
    void cancel(const QString& demandId, const runtime::RuntimeContext& context) override;
    QString sourceKind() const override { return QStringLiteral("dxgi"); }
private:
    LatestFramePump& m_pump;
    TargetWindow m_target;
    runtime::observation::ObservationDemand m_demand;
    std::function<QString(const TargetWindow&)> m_validate;
    std::atomic<bool> m_cancelled{false}, m_busy{false};
    int m_lastRequest = 0;
};
}
