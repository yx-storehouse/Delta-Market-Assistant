#pragma once
#include "application/runtime/observation/observation_adapter.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QRect>

namespace relink::vision {
// The caller must already own a valid full-listing calibration for this target
// and age. These current-frame ROIs are only a continuation observation; they
// never replace that prerequisite or authorize candidate/star input.
QList<QPair<QString,QRect>> collectionListingRegionSpecs();
QJsonObject classifyCollectionListingRegions(
    const runtime::observation::FrameEnvelope& frame,
    const QString& frameSha256, const QJsonArray& regions);
// Project already-recognized CURRENT parent-ROI tokens into a contained ROI.
// A word crossing the requested edge or an empty projection requires the
// original dedicated OCR instead. This is not a historical recognition cache.
QJsonObject projectCurrentCollectionRegion(const QJsonObject& sourceRegion,
    const QString& frameId,const QString& frameSha256,const QString& desiredKind,
    const QRect& bounds);
}
