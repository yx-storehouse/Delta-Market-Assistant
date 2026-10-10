#pragma once

#include "application/runtime/observation/observation_adapter.h"
#include <QJsonObject>

namespace relink::vision {
// Read-only calibration features from the original in-memory client frame.
// The historical column ranges are search bands, not detected card bounds.
// A profile does not establish a complete layout or provide an action point.
QJsonObject collectionLayoutProfile(const runtime::observation::FrameEnvelope& frame);
// Numerical feature replay is diagnostic only: it cannot establish vertical
// card edges without the original frame and never enables an action.
QJsonObject collectionLayoutEdgeCandidates(const QJsonObject& profile);
// Same-frame visible-edge detector. Unsupported/ambiguous frames fail closed.
// An optional reference to a previously selected, fully measured card requests
// an independent receipt-only proof. That proof does not complete an unresolved
// viewport, prove global selection uniqueness, or authorize another action.
QJsonObject detectCollectionLayout(const runtime::observation::FrameEnvelope& frame,
    const QJsonObject& receiptReference = {});
} // namespace relink::vision
