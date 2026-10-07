#include "live_capture_check.h"
#include "application/vision/dxgi_observation_source.h"
#include "application/vision/windows_ocr.h"

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
    parser.addOption({"live-capture-check", "Explicit read-only foreground game capture; always return to the specified IDE."});
    parser.addOption({"target-hwnd", "Exact target window handle (decimal).", "hwnd"});
    parser.addOption({"target-pid", "Exact target process ID (decimal).", "pid"});
    parser.addOption({"return-hwnd", "Exact IDE window to restore (decimal).", "hwnd"});
    parser.addOption({"return-pid", "Exact IDE process ID (decimal).", "pid"});
    parser.addOption({"frames", "Bounded sample count, 1 to 5; default 1.", "count", "1"});
    parser.addOption({"preview-stdout", "Explicit final-frame PNG preview over stdout only; no file output."});
    parser.addOption({"ocr", "Run Windows OCR on the final in-memory frame after returning the IDE to foreground."});
    parser.process(app);
    const quintptr targetHwnd = positive(parser.value("target-hwnd"), std::numeric_limits<quintptr>::max());
    const quint32 targetPid = positive(parser.value("target-pid"), std::numeric_limits<quint32>::max());
    const quintptr returnHwnd = positive(parser.value("return-hwnd"), std::numeric_limits<quintptr>::max());
    const quint32 returnPid = positive(parser.value("return-pid"), std::numeric_limits<quint32>::max());
    const int frameCount = int(positive(parser.value("frames"), 5));
    QJsonObject result{{"diagnostic", "live_capture"}, {"capture_passed", false}, {"frames", QJsonArray{}},
        {"recognition_performed", false}, {"image_file_writes", 0}, {"game_input_sent", false},
        {"ide_foreground_restored", false}, {"game_in_background", false}};
    if (!frameCount || !targetHwnd || !returnHwnd || targetHwnd == returnHwnd || targetPid == returnPid) {
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
    ForegroundReturnGuard guard(ide);
    QJsonArray frames;
    QByteArray finalPixels;
    FrameEnvelope ocrFrame;
    int finalWidth = 0, finalHeight = 0, finalStride = 0;
    try {
        if (activateTargetWindow(target, &error)) {
            QThread::msleep(350);
            target = bindTargetWindow(targetHwnd, targetPid, &error);
        }
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
    QString restoreError;
    const bool restored = guard.restore(&restoreError);
    result["ide_foreground_restored"] = restored;
    result["game_in_background"] = restored && foregroundWindow() != targetHwnd;
    result["foreground_hwnd"] = QString::number(foregroundWindow());
    result["return_hwnd"] = QString::number(returnHwnd);
    result["frames"] = frames;
    result["capture_passed"] = error.isEmpty() && frames.size() == frameCount;
    if (!error.isEmpty()) result["error"] = error;
    if (!restoreError.isEmpty()) result["restore_error"] = restoreError;
    if (restored && error.isEmpty() && parser.isSet("ocr")) {
        WindowsOcrRecognizer recognizer(QCoreApplication::applicationDirPath() + QStringLiteral("/vision/windows_ocr_worker.ps1"));
        const auto recognized = recognizer.recognizeFrame(ocrFrame);
        result["recognition_performed"] = true;
        result["ocr_passed"] = recognized.ok;
        if (recognized.ok) {
            auto summary = summarizeRecognizedPage(recognized.observation);
            summary["provider"] = "Windows.Media.Ocr";
            summary["language"] = recognized.observation.value("language");
            summary["frame_age_at_result_ms"] = captureClockMs() - ocrFrame.sourceMonoMs;
            summary["frame_sha256"] = QString::fromLatin1(QCryptographicHash::hash(ocrFrame.pixels, QCryptographicHash::Sha256).toHex());
            result["ocr"] = summary;
        } else {
            error = recognized.error;
            result["ocr_error"] = error;
        }
    }
    if (restored && error.isEmpty() && !finalPixels.isEmpty()) {
        QImage view(reinterpret_cast<const uchar*>(finalPixels.constData()), finalWidth, finalHeight, finalStride, QImage::Format_RGB32);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (view.scaled(1600, 1000, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "PNG"))
            result["preview_png_base64"] = QString::fromLatin1(png.toBase64());
        else { result["preview_error"] = "E_PREVIEW_ENCODE"; error = QStringLiteral("E_PREVIEW_ENCODE"); }
    }
    const bool finalRestored = guard.restore(&restoreError);
    result["ide_foreground_restored"] = finalRestored;
    result["game_in_background"] = finalRestored && foregroundWindow() != targetHwnd;
    result["foreground_hwnd"] = QString::number(foregroundWindow());
    if (!restoreError.isEmpty()) result["restore_error"] = restoreError;
    return emitResult(result, !finalRestored ? 3 : error.isEmpty() ? 0 : 1);
}
} // namespace relink::diagnostics
