#pragma once

#include "application/runtime/observation/observation_adapter.h"
#include <QJsonObject>
#include <QRect>
#include <memory>

namespace relink::vision {

struct OcrReply {
    bool ok = false;
    QString error;
    QJsonObject observation;
    int helperUiChecks = 0;
    bool helperVisibleWindowObserved = false;
    bool helperForegroundObserved = false;
    QJsonObject timing = {};
};

class WindowsOcrSession;

// Read-only Windows OCR provider. Recognizers on one thread share a hidden
// helper while any recognizer retains the session. Each frame mapping is
// released before return; the last owner drains/exits the helper. This does
// not make separate diagnostic EXE invocations a persistent capture service.
// No confidence score is fabricated: Windows OCR does not supply one.
class WindowsOcrRecognizer final : public runtime::observation::IObservationRecognizer {
public:
    explicit WindowsOcrRecognizer(QString helperPath, QString language = QStringLiteral("zh-Hans-CN"), int timeoutMs = 8000);
    ~WindowsOcrRecognizer() override;
    QString recognize(const runtime::observation::FrameEnvelope& frame, QString* errorCode = nullptr) override;
    OcrReply recognizeFrame(const runtime::observation::FrameEnvelope& frame);
    OcrReply recognizeRegion(const runtime::observation::FrameEnvelope& frame, const QRect& clientRegion,
        int scale = 2, bool invert = false);
    OcrReply recognizeNumericRegion(const runtime::observation::FrameEnvelope& frame,
        const QRect& clientRegion,int scale=3,int padding=5);
    QJsonObject sessionMetrics() const;
    // True once this thread's helper session failed for good (it never restarts).
    bool sessionPoisoned() const;
private:
    QString m_helperPath;
    QString m_language;
    int m_timeoutMs;
    std::shared_ptr<WindowsOcrSession> m_session;
};

// One same-frame ROI recognition request for WindowsOcrPool.
struct OcrRegionJob {
    QString language = QStringLiteral("zh-Hans-CN");
    QRect region;
    int scale = 2;
    bool invert = true;
    bool numeric = false;
};

// Persistent helper threads; each thread owns its own hidden OCR helper (the
// session is thread-local), so independent ROIs of ONE frame are recognized
// concurrently instead of queueing on a single helper. Results keep job order
// and carry the same per-call timing as the serial recognizers.
class WindowsOcrPool final {
public:
    WindowsOcrPool(QString helperPath, int workers);
    ~WindowsOcrPool();
    WindowsOcrPool(const WindowsOcrPool&) = delete;
    WindowsOcrPool& operator=(const WindowsOcrPool&) = delete;
    // The calling thread also takes jobs, using its own recognizers.
    QList<OcrReply> run(const runtime::observation::FrameEnvelope& frame, const QList<OcrRegionJob>& jobs,
        WindowsOcrRecognizer& callerChinese, WindowsOcrRecognizer& callerEnglish);
    QJsonObject metrics() const;
    // True once every worker finished its helper warm-up (or retired).
    bool waitWarm(int timeoutMs) const;
    struct State;
private:
    std::unique_ptr<State> m_state;
};
OcrReply runOcrRegionJob(const runtime::observation::FrameEnvelope& frame, const OcrRegionJob& job,
    WindowsOcrRecognizer& chinese, WindowsOcrRecognizer& english);

// Hidden helpers exit on their own after helperIdleMs without a request; a
// session retires its helper gracefully once idle for restartAfterMs and starts
// a fresh one on the next request. Defaults 600000 / 570000 keep a standby
// capture service warm for ten minutes. Applies to helpers started afterwards.
void setWindowsOcrHelperIdlePolicy(int helperIdleMs, int restartAfterMs);
// A helper refuses its 4097th request (replay set) and exits 1, which would
// poison the session. Sessions retire a helper gracefully after this many
// requests (default 4000) and start a fresh one. Tests may lower it.
void setWindowsOcrHelperRequestLimit(int requests);

// Strict result validation is separately testable with synthetic protocol data.
OcrReply validateOcrReply(const QByteArray& data, const QString& requestId, int width, int height);
QJsonObject summarizeRecognizedPage(const QJsonObject& observation);

} // namespace relink::vision
