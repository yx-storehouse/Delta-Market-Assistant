#pragma once

#include "application/runtime/observation/observation_adapter.h"
#include "target_window.h"
#include <atomic>

namespace relink::vision {

struct PixelSummary {
    bool valid = false;
    bool nearBlack = false;
    int minimum = 255;
    int maximum = 0;
    int sampledPixels = 0;
};
PixelSummary summarizeBgra(const QByteArray& pixels, int width, int height, int stride);
bool sameObservationDemand(const runtime::observation::ObservationDemand& a,
                           const runtime::observation::ObservationDemand& b);

// One source instance is bound to one immutable demand and one window viewport.
// capture() is single-flight; cancel() is thread-safe, sticky and context-bound.
// No timers, background thread, global hooks, input injection or disk I/O.
class DxgiObservationSource final : public runtime::observation::IObservationSource {
public:
    DxgiObservationSource(TargetWindow target, runtime::observation::ObservationDemand demand);
    runtime::observation::CaptureReply capture(const runtime::observation::CaptureRequest& request) override;
    void cancel(const QString& demandId, const runtime::RuntimeContext& context) override;
    QString sourceKind() const override { return QStringLiteral("dxgi"); }
private:
    TargetWindow m_target;
    runtime::observation::ObservationDemand m_demand;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_busy{false};
    int m_lastRequestNumber = 0;
};

} // namespace relink::vision
