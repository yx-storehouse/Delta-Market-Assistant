#pragma once

#include "application/vision/windows_ocr.h"
#include <QJsonArray>
#include <functional>

namespace relink::diagnostics {
struct LabelRefinement {
    vision::OcrReply reply;
    QJsonArray attempts;
};

// Retry only an empty successful OCR reading, using fixed preprocessing of
// the SAME pixels. Stop on the first nonempty reading (even wrong text), any
// provider error, or frame mismatch. No expected label/product/price is an
// input, and this helper neither classifies a page nor acquires another frame.
inline LabelRefinement readCollectionLabel(const QString& frameId, const QRect& bounds,
        const std::function<vision::OcrReply(int, bool)>& read) {
    LabelRefinement result;
    struct Variant { int scale; bool invert; };
    const Variant variants[] = {{2, true}, {1, false}, {3, true}};
    for (const auto& variant : variants) {
        auto reply = read(variant.scale, variant.invert);
        if (reply.ok && (reply.observation["frame_id"] != frameId
                || reply.observation["coverage"] != "roi"
                || !reply.observation["words"].isArray())) {
            reply.ok = false;
            reply.error = "E_COLLECTION_LABEL_FRAME_BINDING";
        }
        result.attempts.append(QJsonObject{{"attempt", result.attempts.size() + 1},
            {"scale", variant.scale}, {"invert", variant.invert},
            {"bounds", QJsonArray{bounds.x(), bounds.y(), bounds.width(), bounds.height()}},
            {"frame_id", frameId}, {"same_frame", reply.ok}, {"ok", reply.ok},
            {"error", reply.error}, {"words", reply.observation["words"]}, {"timing", reply.timing}});
        result.reply = reply;
        if (!reply.ok || !reply.observation["words"].toArray().isEmpty()) break;
    }
    return result;
}
}
