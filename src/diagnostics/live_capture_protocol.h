#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace relink::diagnostics {
// The service is deliberately narrower than the standalone diagnostic: the
// enclosing batch owns focus, every request captures anew, and no UI/input
// command can be routed through this protocol.
inline constexpr int CaptureRequestMaximumBytes = 65536;
inline constexpr int CaptureRequestMaximumArguments = 64;
struct CaptureServiceRequest {
    QJsonValue id = QJsonValue::Null;
    QStringList arguments;
    QString error;
};

inline CaptureServiceRequest parseCaptureServiceRequest(const QByteArray& line) {
    CaptureServiceRequest request;
    auto fail = [&](const char* code) { request.error = QLatin1String(code); return request; };
    if (line.size() > CaptureRequestMaximumBytes) return fail("E_CAPTURE_SERVER_LINE_LIMIT");
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(line, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return fail("E_CAPTURE_SERVER_JSON");
    const auto packet = document.object();
    const auto id = packet.value("id");
    const bool validStringId = id.isString() && !id.toString().isEmpty()
        && id.toString().toUtf8().size() <= 128 && !id.toString().contains(QChar(0));
    const bool validNumberId = id.isDouble() && std::isfinite(id.toDouble())
        && std::floor(id.toDouble()) == id.toDouble() && std::abs(id.toDouble()) <= 9007199254740991.0;
    if (!validStringId && !validNumberId) return fail("E_CAPTURE_SERVER_ID");
    request.id = id;
    const auto keys = packet.keys();
    if (QSet<QString>(keys.begin(), keys.end()) != QSet<QString>{"id", "arguments"}
        || !packet.value("arguments").isArray()) return fail("E_CAPTURE_SERVER_ENVELOPE");
    const auto arguments = packet.value("arguments").toArray();
    if (arguments.isEmpty() || arguments.size() > CaptureRequestMaximumArguments)
        return fail("E_CAPTURE_SERVER_ARGUMENT_LIMIT");
    for (const auto& value : arguments) {
        if (!value.isString() || value.toString().isEmpty()
            || value.toString().toUtf8().size() > 4096 || value.toString().contains(QChar(0)))
            return fail("E_CAPTURE_SERVER_ARGUMENT");
        request.arguments.append(value.toString());
    }
    const QSet<QString> switches{"live-capture-check", "preview-stdout", "ocr", "ocr-lobby-anchors",
        "ocr-market-anchors", "ocr-ui-regions", "catalog-filter-state", "collection-observation",
        "collection-layout-profile", "collection-layout", "collection-numeric-price",
        "collection-local-price", "collection-local-text", "collection-price-image", "collection-title-image", "collection-catalog-image",
        "reuse-capture-resources", "collection-selection-preflight", "collection-local-hotpath", "collection-ready-stream", "collection-scroll-readiness",
        "collection-pixel-receipt", "purchase-observation", "purchase-countdown-stop-on-button",
        "purchase-countdown-stop-on-green", "purchase-countdown-lean", "purchase-countdown-stop-on-jump",
        "purchase-countdown-stop-on-zero"};
    const QSet<QString> valued{"focus-policy", "target-hwnd", "target-pid", "return-hwnd", "return-pid",
        "frames", "collection-receipt-reference", "collection-card-id", "collection-visible-index",
        "collection-card-index", "expected-page", "collection-selection-point", "purchase-countdown-watch",
        "purchase-countdown-area", "purchase-countdown-expect-zero-ms", "purchase-countdown-stop-on-text"};
    QSet<QString> seen;
    QString focusPolicy;
    for (qsizetype i = 0; i < request.arguments.size(); ++i) {
        const auto token = request.arguments[i];
        if (!token.startsWith("--")) return fail("E_CAPTURE_SERVER_OPTION");
        const auto eq = token.indexOf('=');
        const auto name = token.mid(2, eq < 0 ? -1 : eq - 2);
        if ((!switches.contains(name) && !valued.contains(name)) || seen.contains(name))
            return fail("E_CAPTURE_SERVER_OPTION");
        seen.insert(name);
        if (switches.contains(name)) {
            if (eq >= 0) return fail("E_CAPTURE_SERVER_OPTION");
            continue;
        }
        QString value;
        if (eq >= 0) value = token.mid(eq + 1);
        else if (++i < request.arguments.size()) value = request.arguments[i];
        if (value.isEmpty() || value.startsWith("--")) return fail("E_CAPTURE_SERVER_OPTION");
        if (name == "focus-policy") focusPolicy = value;
    }
    if (!seen.contains("live-capture-check") || focusPolicy != "caller-owned")
        return fail("E_CAPTURE_SERVER_FOCUS_POLICY");
    // QCommandLineParser expects argv[0]. The synthetic value cannot be parsed
    // as a command and is never used to launch a child process.
    request.arguments.prepend(QStringLiteral("live-capture-server-request"));
    return request;
}

inline QJsonObject captureServiceError(const QString& code) {
    return {{"diagnostic", "live_capture"}, {"capture_passed", false},
        {"recognition_performed", false}, {"image_file_writes", 0},
        {"game_input_sent", false}, {"error", code}};
}

// Negative-only gate before expensive OCR after a dispatched card selection.
// A ready gate permits observation to proceed; it is never a page classifier
// or an actionable selection proof. The default is full OCR. An independently
// validated server page context may replace only that page OCR and explicitly
// marks the replacement; current candidate/identity/lease checks still run.
inline QJsonObject collectionSelectionGate(const QJsonObject& layout) {
    int selected=0;
    bool usable=false;
    for(const auto& value:layout["cards"].toArray()) {
        const auto card=value.toObject();
        if(card["selected"]!=true)continue;
        ++selected;
        const auto edges=card["edges"].toObject();
        usable=card["bounds_basis"]=="observed" &&card["fields_bounds"].isArray()
            &&card["fields_bounds"].toArray().size()==4
            &&edges["top"]==true &&edges["bottom"]==true &&edges["left"]==true &&edges["right"]==true;
    }
    const bool ready=layout["complete"]==true &&selected==1 &&usable;
    return {{"schema","collection-selection-preflight-v1"},{"scope","negative_readiness_gate_only"},
        {"frame_id",layout["frame_id"]},{"frame_sha256",layout["frame_sha256"]},{"same_frame",true},
        {"layout_complete",layout["complete"]==true},{"selected_count",selected},{"ready_for_full_ocr",ready},
        {"ocr_deferred",!ready},{"actions_enabled",false},{"page_classified",false},
        {"identity_proven",false},{"requires_original_full_validation",true}};
}

inline bool collectionSelectionPointValid(const QJsonArray& point) {
    if(point.size()!=2)return false;
    for(const auto& v:point)if(!v.isDouble()||!std::isfinite(v.toDouble())
        ||v.toDouble()!=std::floor(v.toDouble())||v.toDouble()<=0||v.toDouble()>=8192)return false;
    return true;
}
// A dispatched target is only a negative readiness hint. An old selected peer
// must not end the watch. The Python coordinator still rebinds actual geometry
// and checks item fields before any star input; this does not authorize input.
inline QJsonObject collectionSelectionTargetGate(const QJsonObject& layout,const QJsonArray& point) {
    auto gate=collectionSelectionGate(layout);
    int matches=0;
    const bool valid=collectionSelectionPointValid(point);
    if(valid)for(const auto& v:layout["cards"].toArray()){
        const auto card=v.toObject();const auto r=card["bounds"].toArray();
        if(card["selected"]!=true||r.size()!=4)continue;
        if(point[0].toInt()>r[0].toInt()+3&&point[0].toInt()<r[0].toInt()+r[2].toInt()-3
           &&point[1].toInt()>r[1].toInt()+3&&point[1].toInt()<r[1].toInt()+r[3].toInt()-3)++matches;
    }
    gate["requested_selection_point"]=point;
    gate["selected_target_contains_point"]=valid&&matches==1;
    gate["selected_target_match_count"]=matches;
    const bool ready=gate["ready_for_full_ocr"]==true&&valid&&matches==1;
    gate["ready_for_full_ocr"]=ready;gate["ocr_deferred"]=!ready;
    gate["target_hint_role"]="negative_gate_only_original_rebind_required";
    return gate;
}

// Pixel-only receipt watch after an already dispatched favorite input. The
// fixed detail-star counts come from the same frame as the layout. It ends a
// bounded watch only; the coordinator still binds the selected card geometry
// to its pre-dispatch candidate and the journal before confirming anything.
inline QJsonObject collectionPixelReceiptGate(const QJsonObject& layout,const QJsonObject& reference,
        int warm,int bright,int total) {
    const double warmFraction=total>0?double(warm)/total:0.0;
    const double brightFraction=total>0?double(bright)/total:1.0;
    const bool gold=total>0 &&warmFraction>=.05 &&brightFraction<=.02;
    const auto expected=reference["bounds"].toArray();
    const auto near=[&](const QJsonValue& value) {
        const auto b=value.toArray();
        if(b.size()!=4 ||expected.size()!=4)return false;
        const int l0=expected[0].toInt(),t0=expected[1].toInt(),r0=l0+expected[2].toInt(),b0=t0+expected[3].toInt();
        const int l1=b[0].toInt(),t1=b[1].toInt(),r1=l1+b[2].toInt(),b1=t1+b[3].toInt();
        return std::abs(l0-l1)<=8 &&std::abs(r0-r1)<=8 &&std::abs(t0-t1)<=5 &&std::abs(b0-b1)<=3;
    };
    const bool complete=layout["complete"]==true;
    int selected=0;bool matched=false;QJsonObject card;
    if(complete) {
        for(const auto& value:layout["cards"].toArray()) {
            const auto c=value.toObject();
            if(c["selected"]!=true)continue;
            ++selected;
            const auto e=c["edges"].toObject();
            if(near(c["bounds"]) &&c["bounds_basis"]=="observed" &&c["fields_bounds"].isArray()
                &&e["top"]==true &&e["bottom"]==true &&e["left"]==true &&e["right"]==true){matched=true;card=c;}
        }
        if(selected!=1){matched=false;card={};}
    } else {
        const auto proof=layout["selected_card_proof"].toObject();
        const auto c=proof["card"].toObject();
        if(proof["proven"]==true &&c["selected"]==true &&near(c["bounds"])){matched=true;card=c;selected=1;}
    }
    const bool acceptable=gold &&matched;
    return {{"schema","collection-pixel-receipt-gate-v1"},{"scope","receipt_observation_only"},
        {"frame_id",layout["frame_id"]},{"frame_sha256",layout["frame_sha256"]},
        {"layout_complete",complete},{"selected_count",selected},{"reference_card_selected",matched},
        {"favorite_warm_fraction",warmFraction},{"favorite_bright_fraction",brightFraction},
        {"favorite_pixel_count",total},{"star_gold",gold},{"acceptable",acceptable},
        {"ready",acceptable &&complete},{"card",card},{"actions_enabled",false},{"ocr_performed",false}};
}

// A listing-region miss caused only by the static header/sort labels (no
// blocker, detail anchors present): the next fresh frame may be read instead.
inline bool collectionListingStaticAnchorMiss(const QJsonObject& semantic) {
    if(semantic["matched"]==true ||semantic["reason"]!="E_LISTING_REGIONS_REQUIRED_ANCHOR"
        ||semantic.contains("blocker"))return false;
    const auto missing=semantic["missing_anchors"].toArray();
    if(missing.isEmpty())return false;
    for(const auto& value:missing)
        if(value!="listing.sale" &&value!="listing.default_sort")return false;
    return true;
}

// Words of two overlapping same-frame halves of one ROI, both already in client
// coordinates. A word read in both halves (same text, origin within 4 px) is
// kept once; every other word is kept as read, never repaired or joined.
inline QJsonArray mergeSplitRegionWords(const QJsonArray& top,const QJsonArray& bottom) {
    QJsonArray words=top;
    for(const auto& value:bottom) {
        const auto word=value.toObject();bool duplicate=false;
        for(const auto& existing:top) {
            const auto other=existing.toObject();
            if(other["text"]==word["text"] &&std::abs(other["x"].toDouble()-word["x"].toDouble())<=4
                &&std::abs(other["y"].toDouble()-word["y"].toDouble())<=4){duplicate=true;break;}
        }
        if(!duplicate)words.append(value);
    }
    return words;
}

// Keep packet-level measurements additive even when the OCR owner survives
// multiple captures. The small cumulative object carries lifetime totals but
// never repeats the previous 128-call timing window in each result packet.
inline QJsonObject captureRequestOcrMetrics(const QJsonObject& before, const QJsonObject& after) {
    auto result = after;
    auto cumulative = after;
    cumulative.remove("calls");
    result["scope"] = "capture_request";
    const int count = std::max(0, after["request_count"].toInt() - before["request_count"].toInt());
    for (const auto* field : {"helper_start_count", "request_count", "success_count", "engine_create_count",
             "frame_mappings_released", "helper_ui_checks"})
        result[field] = std::max(0, after[field].toInt() - before[field].toInt());
    const auto recent = after["calls"].toArray();
    QJsonArray calls;
    for (qsizetype i = std::max<qsizetype>(0, recent.size() - count); i < recent.size(); ++i)
        calls.append(recent[i]);
    result["calls"] = calls;
    result["calls_truncated"] = count > recent.size();
    result["cumulative"] = cumulative;
    return result;
}
} // namespace relink::diagnostics
