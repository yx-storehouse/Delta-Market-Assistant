#pragma once

#include "application/runtime/observation/observation_adapter.h"
#include <QJsonObject>
#include <QRect>

namespace relink::vision {

struct OcrReply {
    bool ok = false;
    QString error;
    QJsonObject observation;
    int helperUiChecks = 0;
    bool helperVisibleWindowObserved = false;
    bool helperForegroundObserved = false;
};

// Read-only Windows OCR diagnostic provider; each call owns its mapping and
// hidden helper, closes them before returning, and never saves image bytes.
// No confidence score is fabricated: Windows OCR does not supply one.
class WindowsOcrRecognizer final : public runtime::observation::IObservationRecognizer {
public:
    explicit WindowsOcrRecognizer(QString helperPath, QString language = QStringLiteral("zh-Hans-CN"), int timeoutMs = 8000);
    QString recognize(const runtime::observation::FrameEnvelope& frame, QString* errorCode = nullptr) override;
    OcrReply recognizeFrame(const runtime::observation::FrameEnvelope& frame);
    OcrReply recognizeRegion(const runtime::observation::FrameEnvelope& frame, const QRect& clientRegion,
        int scale = 2, bool invert = false);
private:
    QString m_helperPath;
    QString m_language;
    int m_timeoutMs;
};

// Strict result validation is separately testable with synthetic protocol data.
OcrReply validateOcrReply(const QByteArray& data, const QString& requestId, int width, int height);
QJsonObject summarizeRecognizedPage(const QJsonObject& observation);

} // namespace relink::vision
