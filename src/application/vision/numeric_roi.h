#pragma once
#include "application/runtime/observation/observation_adapter.h"
#include <QJsonObject>
#include <QRect>

namespace relink::vision {
struct PreparedNumericRegion {
    bool ok=false;
    QString error;
    runtime::observation::FrameEnvelope frame;
    QRect sourceRegion;
    QRect cropBounds;
    QRect inkBounds;
    int sourceWidth=0;
    int sourceHeight=0;
    int scale=1;
    int paddingPixels=0;
    QJsonObject metadata;
};
// Read-only pixel preparation, not a digit classifier. No configured price,
// expected text, prior frame, game input, model file, or image path is accepted.
PreparedNumericRegion prepareNumericRegion(const runtime::observation::FrameEnvelope& frame,
    const QRect& region,int scale=3,int padding=5);
// Map actual prepared-image OCR boxes back through the precise crop/padding
// transform. Reject unsupported or incomplete observations without fixing text.
QJsonObject mapNumericRegionObservation(const PreparedNumericRegion& prepared,
    const QJsonObject& observation,QString* error=nullptr);
} // namespace relink::vision
