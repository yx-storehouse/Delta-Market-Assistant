#pragma once
#include "application/runtime/observation/observation_adapter.h"
#include <QJsonObject>
#include <QMap>
#include <QRect>

namespace relink::vision {
enum class CheckState { Unknown, Unchecked, Checked };
QString toString(CheckState value);
struct FilterCheckbox {
    QString label;
    QString observedLabel;
    CheckState state = CheckState::Unknown;
    QRect bounds;
    QString reason;
    QJsonObject measurement;
};
struct CatalogFilterState {
    bool validPage = false;
    bool complete = false;
    QString reason;
    QString frameId;
    QString frameSha256;
    QString seasonLabel;
    QMap<QString, FilterCheckbox> boxes;
    QJsonObject toJson() const;
};
// Pure, read-only, same-frame extraction. The current geometry profile is
// deliberately limited to the observed 2560x1440 / 144 DPI client. Unknown
// geometry or ambiguous pixels stay Unknown; no false/unselected default.
CatalogFilterState readCatalogFilter(
    const runtime::observation::FrameEnvelope& frame, const QJsonObject& fullClientOcr);
// Request a narrow re-read only for an actually missing season label on a
// validated, bound filter frame. Merge only actual same-frame ROI OCR words;
// no expected season value is accepted by this interface.
QRect catalogSeasonLabelRefinementRegion(
    const runtime::observation::FrameEnvelope& frame, const QJsonObject& fullClientOcr);
QJsonObject refineCatalogSeasonLabel(const runtime::observation::FrameEnvelope& frame,
    const QJsonObject& fullClientOcr, const QJsonObject& regionOcr, const QRect& region,
    QJsonObject* evidence = nullptr);
// All samples are summarized in memory. No pixel/patch serialization or I/O.
FilterCheckbox measureFilterCheckbox(const runtime::observation::FrameEnvelope& frame,
    const QRect& bounds, const QString& label);
// Numeric signal replay only. This alone is not page/frame/label evidence.
CheckState classifyFilterCheckboxMeasurement(const QJsonObject& signal);
} // namespace relink::vision
