#include "live_capture_check.h"
#include "foreground_policy.h"
#include "application/vision/dxgi_observation_source.h"
#include "application/vision/windows_ocr.h"
#include "application/vision/skin_page_classifier.h"

#include <QBuffer>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QUuid>
#include <cstdio>
#include <limits>

namespace relink::diagnostics {
using namespace vision;
using namespace runtime::observation;
namespace {
quint64 positive(const QString& value, quint64 max) {
    bool ok = false;
    const auto n = value.toULongLong(&ok, 10);
    return ok && n > 0 && n <= max ? n : 0;
}
int emitResult(const QJsonObject& result, int status) {
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fputc('\n', stdout); std::fflush(stdout);
    return status;
}
}

int runLiveCaptureCheck(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"live-capture-check", "Explicit read-only capture; standalone returns IDE once, caller-owned batch never switches focus."});
    parser.addOption({"focus-policy", "standalone (default) or caller-owned for an enclosing foreground batch.", "policy", "standalone"});
    parser.addOption({"target-hwnd", "Exact target window handle (decimal).", "hwnd"});
    parser.addOption({"target-pid", "Exact target process ID (decimal).", "pid"});
    parser.addOption({"return-hwnd", "Exact IDE window to restore (decimal).", "hwnd"});
    parser.addOption({"return-pid", "Exact IDE process ID (decimal).", "pid"});
    parser.addOption({"frames", "Bounded sample count, 1 to 5; default 1.", "count", "1"});
    parser.addOption({"preview-stdout", "Explicit final-frame PNG preview over stdout only; no file output."});
    parser.addOption({"ocr", "Run Windows OCR on the final in-memory frame without intermediate foreground switches."});
    parser.addOption({"ocr-lobby-anchors", "With --ocr: emit text boxes from three fixed lobby button regions only, not full OCR."});
    parser.addOption({"ocr-market-anchors", "With --ocr: emit bounded allowlisted market labels, not full OCR or images."});
    parser.addOption({"ocr-ui-regions", "With --ocr: reread three UI-label regions at 2x from the same memory frame; no extra capture."});
    parser.addOption({"expected-page", "With --ocr: require this page and no blocking overlay; mismatch exits 1.", "page"});
    parser.process(app);
    const quintptr targetHwnd = positive(parser.value("target-hwnd"), std::numeric_limits<quintptr>::max());
    const quint32 targetPid = positive(parser.value("target-pid"), std::numeric_limits<quint32>::max());
    const quintptr returnHwnd = positive(parser.value("return-hwnd"), std::numeric_limits<quintptr>::max());
    const quint32 returnPid = positive(parser.value("return-pid"), std::numeric_limits<quint32>::max());
    const int frameCount = int(positive(parser.value("frames"), 5));
    QJsonObject result{{"diagnostic", "live_capture"}, {"capture_passed", false}, {"frames", QJsonArray{}},
        {"recognition_performed", false}, {"image_file_writes", 0}, {"game_input_sent", false},
        {"ide_foreground_restored", false}, {"game_in_background", false}};
    const auto focusPolicy = parser.value("focus-policy");
    if (focusPolicy != "standalone" && focusPolicy != "caller-owned") {
        result["error"] = "E_DIAGNOSTIC_FOCUS_POLICY"; return emitResult(result, 2);
    }
    const bool callerOwned = focusPolicy == "caller-owned";
    result["focus_policy"] = focusPolicy;
    result["foreground_cleanup_owner"] = callerOwned ? "batch_caller" : "diagnostic";
    const QString expectedPage = parser.value("expected-page");
    const QStringList pageNames{"lobby", "warehouse", "mandel", "skin_home", "empty_watchlist", "catalog_filter",
        "skin_listings", "watchlist_listings", "game_settings", "base"};
    if (parser.isSet("expected-page") && (!parser.isSet("ocr") || !pageNames.contains(expectedPage))) {
        result["error"] = "E_DIAGNOSTIC_EXPECTED_PAGE"; return emitResult(result, 2);
    }
    if (!expectedPage.isEmpty()) { result["expected_page"] = expectedPage; result["page_match_passed"] = false; }
    if (!frameCount || !targetHwnd || !returnHwnd || targetHwnd == returnHwnd || targetPid == returnPid
        || ((parser.isSet("ocr-lobby-anchors") || parser.isSet("ocr-market-anchors") || parser.isSet("ocr-ui-regions")) && !parser.isSet("ocr"))) {
        result["error"] = "E_DIAGNOSTIC_ARGUMENTS"; return emitResult(result, 2);
    }
    QString error;
    const auto ide = bindTargetWindow(returnHwnd, returnPid, &error);
    if (!error.isEmpty()) { result["error"] = error; return emitResult(result, 2); }
    auto target = bindTargetWindow(targetHwnd, targetPid, &error);
    if (!error.isEmpty()) { result["error"] = error; return emitResult(result, 2); }
    // Diagnostic allowlist prevents accidentally capturing an unrelated browser
    // or arbitrary desktop. The reusable backend itself binds caller identities.
    if (target.executableName.compare(QStringLiteral("DeltaForceClient-Win64-Shipping.exe"), Qt::CaseInsensitive) != 0
        || ide.executableName.compare(QStringLiteral("Mirasim.exe"), Qt::CaseInsensitive) != 0) {
        result["error"] = "E_DIAGNOSTIC_TARGET"; return emitResult(result, 2);
    }
    QString restoreError;
    ForegroundPolicy focus(callerOwned, targetHwnd, returnHwnd, foregroundWindow,
        [&]() { return activateTargetWindow(target, &error); },
        [&]() { return activateTargetWindow(ide, &restoreError); });
    QJsonArray frames;
    QByteArray finalPixels;
    FrameEnvelope ocrFrame;
    int finalWidth = 0, finalHeight = 0, finalStride = 0;
    try {
        if (focus.begin()) {
            if (!callerOwned) QThread::msleep(350);
            target = bindTargetWindow(targetHwnd, targetPid, &error);
        } else if (error.isEmpty()) error = QStringLiteral("E_BATCH_FOREGROUND_NOT_OWNED");
        if (error.isEmpty()) error = validateTargetWindow(target);
        if (error.isEmpty()) {
            const QString session = QUuid::createUuid().toString(QUuid::WithoutBraces);
            ObservationDemand demand;
            demand.demandId = QStringLiteral("live-check:") + session;
            demand.context = {QStringLiteral("diagnostic"), session, captureClockDomain(), QStringLiteral("page-check"), 0, 1};
            demand.createdMonoMs = demand.notBeforeMonoMs = captureClockMs();
            demand.deadlineMonoMs = demand.createdMonoMs + 2500 * frameCount;
            demand.maxFrameAgeMs = 1000;
            demand.frameBudget = frameCount;
            demand.mode = frameCount == 1 ? ObservationMode::OneShot : ObservationMode::BoundedWatch;
            demand.minIntervalMs = frameCount == 1 ? 0 : 150;
            demand.roiSpecs = {{QStringLiteral("client"), QStringLiteral("client_physical_px"), 0, 0,
                target.clientRect.width(), target.clientRect.height(), 1}};
            DxgiObservationSource source(target, demand);
            ObservationAdapter adapter(&source);
            if (adapter.start(demand, captureClockMs(), &error)) {
                for (int i = 0; i < frameCount && error.isEmpty(); ++i) {
                    if (i) QThread::msleep(150);
                    const auto request = adapter.beginAcquire(captureClockMs(), &error);
                    if (!error.isEmpty()) break;
                    const auto raw = source.capture(request);
                    const auto reply = adapter.completeCapture(raw, captureClockMs());
                    if (reply.status != CaptureStatus::Captured) { error = reply.errorCode; break; }
                    const auto accepted = adapter.consumePending(captureClockMs());
                    if (!accepted.accepted) { error = accepted.errorCode; break; }
                    const auto& f = accepted.frame;
                    const auto pixels = summarizeBgra(f.pixels, f.width, f.height, f.strideBytes);
                    frames.append(QJsonObject{{"width", f.width}, {"height", f.height}, {"dpi", f.dpiX},
                        {"bytes", f.validBytes}, {"source", f.sourceKind}, {"freshness", toString(f.freshnessBasis)},
                        {"capture_ms", f.captureEndMonoMs - f.captureStartMonoMs},
                        {"source_age_ms", f.captureEndMonoMs - f.sourceMonoMs},
                        {"source_uncertainty_ms", f.sourceUncertaintyMs}, {"near_black", pixels.nearBlack},
                        {"pixel_min", pixels.minimum}, {"pixel_max", pixels.maximum},
                        {"sha256", QString::fromLatin1(QCryptographicHash::hash(f.pixels, QCryptographicHash::Sha256).toHex())}});
                    if (parser.isSet("preview-stdout")) {
                        finalPixels = f.pixels; finalWidth = f.width; finalHeight = f.height; finalStride = f.strideBytes;
                    }
                    if (parser.isSet("ocr")) ocrFrame = f;
                    if (!adapter.releaseFrame(f.leaseId, f.slotGeneration)) error = QStringLiteral("E_DIAGNOSTIC_RELEASE");
                }
                adapter.close();
                result["resources_drained"] = adapter.snapshot().resourcesDrained;
                result["adapter_state"] = toString(adapter.state());
            }
        }
    } catch (...) { error = QStringLiteral("E_DIAGNOSTIC_EXCEPTION"); }
    result["return_hwnd"] = QString::number(returnHwnd);
    result["frames"] = frames;
    result["capture_passed"] = error.isEmpty() && frames.size() == frameCount;
    if (!error.isEmpty()) result["error"] = error;
    if (error.isEmpty() && parser.isSet("ocr")) {
        WindowsOcrRecognizer recognizer(QCoreApplication::applicationDirPath() + QStringLiteral("/vision/windows_ocr_worker.ps1"));
        auto recognized = recognizer.recognizeFrame(ocrFrame);
        if(recognized.ok && parser.isSet("ocr-ui-regions")) {
            QJsonArray refinements;
            const QRect regions[]={QRect(int(ocrFrame.width*.055),int(ocrFrame.height*.035),int(ocrFrame.width*.23),int(ocrFrame.height*.085)),
                QRect(int(ocrFrame.width*.845),int(ocrFrame.height*.16),int(ocrFrame.width*.11),int(ocrFrame.height*.11)),
                QRect(int(ocrFrame.width*.42),int(ocrFrame.height*.58),int(ocrFrame.width*.17),int(ocrFrame.height*.07))};
            auto words=recognized.observation.value("words").toArray();
            int regionIndex=0;
            for(const auto& region:regions){
                const bool emptyMessage=regionIndex++==2;
                const auto refined=recognizer.recognizeRegion(ocrFrame,region,emptyMessage?3:2,emptyMessage);
                refinements.append(QJsonObject{{"ok",refined.ok},{"error",refined.error},
                    {"x",region.x()},{"y",region.y()},{"width",region.width()},{"height",region.height()},
                    {"source_text_angle_degrees",refined.observation.value("source_text_angle_degrees")},
                    {"roi_scale",refined.observation.value("roi_scale")},
                    {"roi_preprocess",refined.observation.value("roi_preprocess")},
                    {"replacement_applied",refined.ok && !refined.observation.value("words").toArray().isEmpty()},
                    {"word_count",refined.observation.value("words").toArray().size()}});
                if(!refined.ok){recognized.ok=false;recognized.error=refined.error;break;}
                if(refined.observation.value("words").toArray().isEmpty())continue;
                QJsonArray merged;
                for(const auto& value:words){const auto word=value.toObject();
                    const QPointF center(word.value("x").toDouble()+word.value("width").toDouble()/2,
                        word.value("y").toDouble()+word.value("height").toDouble()/2);
                    if(!QRectF(region).contains(center))merged.append(value);
                }
                for(const auto& value:refined.observation.value("words").toArray())merged.append(value);
                words=merged;
            }
            recognized.observation["words"]=words;
            result["ocr_ui_regions"]=refinements;
            result["ocr_ui_regions_same_frame"]=true;
        }
        result["recognition_performed"] = true;
        result["ocr_passed"] = recognized.ok;
        if (recognized.ok) {
            auto pageObservation = recognized.observation;
            // This diagnostic requested the entire bound client, not a ROI.
            pageObservation["coverage"] = "full_client";
            const auto classified = classifySkinPage(pageObservation);
            result["startup_page"] = classified.toJson();
            if (!expectedPage.isEmpty()) {
                const bool matched = classified.validInput && toString(classified.page) == expectedPage
                    && classified.overlay == PageOverlay::None;
                result["page_match_passed"] = matched;
                if (!matched) { error = QStringLiteral("E_DIAGNOSTIC_PAGE_MISMATCH"); result["page_error"] = error; }
            }
            if (parser.isSet("ocr-lobby-anchors"))
                result["lobby_anchor_diagnostics"] = projectLobbyAnchorDiagnostics(pageObservation);
            if (parser.isSet("ocr-market-anchors"))
                result["market_anchor_diagnostics"] = projectMarketAnchorDiagnostics(pageObservation);
            auto summary = summarizeRecognizedPage(recognized.observation);
            summary["provider"] = "Windows.Media.Ocr";
            summary["language"] = recognized.observation.value("language");
            summary["source_text_angle_degrees"] = recognized.observation.value("source_text_angle_degrees");
            summary["coordinate_space"] = recognized.observation.value("coordinate_space");
            summary["coordinate_transform"] = recognized.observation.value("coordinate_transform");
            summary["outside_frame_tokens_dropped"] = recognized.observation.value("outside_frame_tokens_dropped");
            summary["frame_age_at_result_ms"] = captureClockMs() - ocrFrame.sourceMonoMs;
            summary["frame_sha256"] = QString::fromLatin1(QCryptographicHash::hash(ocrFrame.pixels, QCryptographicHash::Sha256).toHex());
            result["ocr"] = summary;
        } else {
            error = recognized.error;
            result["ocr_error"] = error;
        }
    }
    if (error.isEmpty() && !finalPixels.isEmpty()) {
        QImage view(reinterpret_cast<const uchar*>(finalPixels.constData()), finalWidth, finalHeight, finalStride, QImage::Format_RGB32);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (view.scaled(1600, 1000, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "PNG"))
            result["preview_png_base64"] = QString::fromLatin1(png.toBase64());
        else { result["preview_error"] = "E_PREVIEW_ENCODE"; error = QStringLiteral("E_PREVIEW_ENCODE"); }
    }
    const bool focusFinished = focus.finish();
    const bool finalRestored = foregroundWindow() == returnHwnd;
    result["ide_foreground_restored"] = finalRestored;
    result["game_in_background"] = foregroundWindow() != targetHwnd;
    result["target_foreground_retained"] = foregroundWindow() == targetHwnd;
    result["focus_activation_requests"] = focus.activationRequests;
    result["focus_restore_requests"] = focus.restoreRequests;
    result["foreground_hwnd"] = QString::number(foregroundWindow());
    if (!restoreError.isEmpty()) result["restore_error"] = restoreError;
    if (!focusFinished && callerOwned) {
        if (error.isEmpty()) error = QStringLiteral("E_BATCH_FOREGROUND_LOST");
        result["error"] = error;
    }
    return emitResult(result, !focusFinished && !callerOwned ? 3 : error.isEmpty() ? 0 : 1);
}
} // namespace relink::diagnostics
