#include "windows_ocr.h"
#include "dxgi_observation_source.h"
#include "shared_frame_memory.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QUuid>
#include <cmath>
#include <cstring>
#include <QRectF>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace relink::vision {
namespace {
OcrReply failure(const QString& code) { return {false, code, {}}; }
#ifdef Q_OS_WIN
struct HelperUiState { DWORD pid = 0; int checks = 0; bool visible = false; bool foreground = false; };
void inspectHelperUi(HelperUiState& state) {
    ++state.checks;
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    state.foreground |= foregroundPid == state.pid;
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        auto& s = *reinterpret_cast<HelperUiState*>(param);
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid == s.pid && IsWindowVisible(window)) s.visible = true;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
}
#endif
}

OcrReply validateOcrReply(const QByteArray& data, const QString& requestId, int width, int height) {
    if (data.isEmpty() || data.size() > 1048576 || requestId.isEmpty() || width < 1 || height < 1) return failure(QStringLiteral("E_OCR_RESPONSE"));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return failure(QStringLiteral("E_OCR_RESPONSE"));
    auto object = document.object();
    if (object.value("request_id") != requestId || object.value("protocol") != "windows-ocr-once-v1")
        return failure(QStringLiteral("E_OCR_CORRELATION"));
    if (!object.value("ok").isBool()) return failure(QStringLiteral("E_OCR_RESPONSE"));
    if (!object.value("ok").toBool()) {
        const auto code = object.value("error").toString();
        if (code == "E_OCR_LANGUAGE" || code == "E_OCR_IMAGE_SIZE" || code == "E_OCR_WORKER_INPUT"
            || code == "E_OCR_ENGINE" || code == "E_OCR_TOKEN_LIMIT") return failure(code);
        return failure(QStringLiteral("E_OCR_ENGINE"));
    }
    if (object.value("width").toInteger() != width || object.value("height").toInteger() != height
        || !object.value("words").isArray() || object.value("words").toArray().size() > 2000
        || !object.value("language").isString() || object.value("language").toString().size() > 50)
        return failure(QStringLiteral("E_OCR_RESPONSE"));
    const bool rotatedCoordinates = object.value("coordinate_space") == "windows_ocr_rotated";
    double angle = 0;
    if (object.contains("coordinate_space") && !rotatedCoordinates) return failure(QStringLiteral("E_OCR_COORDINATES"));
    if (rotatedCoordinates) {
        if (!object.contains("text_angle")) return failure(QStringLiteral("E_OCR_COORDINATES"));
        const auto value = object.value("text_angle");
        if (!value.isNull()) {
            if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble()) > 180)
                return failure(QStringLiteral("E_OCR_ANGLE"));
            angle = value.toDouble();
        }
    } else if (object.contains("text_angle")) return failure(QStringLiteral("E_OCR_COORDINATES"));
    const double radians = angle * 3.14159265358979323846 / 180.0;
    const double cosine = std::cos(radians), sine = std::sin(radians);
    QJsonArray mappedWords;
    int droppedOutside = 0;
    int textBytes = 0;
    for (const auto& value : object.value("words").toArray()) {
        if (!value.isObject()) return failure(QStringLiteral("E_OCR_TOKEN"));
        auto word = value.toObject();
        const auto text = word.value("text").toString();
        if (text.isEmpty() || text.size() > 1024) return failure(QStringLiteral("E_OCR_TOKEN"));
        textBytes += text.toUtf8().size();
        if (textBytes > 65536) return failure(QStringLiteral("E_OCR_TOKEN_LIMIT"));
        const auto x = word.value("x"), y = word.value("y"), w = word.value("width"), h = word.value("height");
        for (const auto& field : {x, y, w, h})
            if (!field.isDouble() || !std::isfinite(field.toDouble())) return failure(QStringLiteral("E_OCR_BOUNDS"));
        if (w.toDouble() <= 0 || h.toDouble() <= 0) return failure(QStringLiteral("E_OCR_BOUNDS"));
        if (!rotatedCoordinates) {
            if (x.toDouble()<0 || y.toDouble()<0 || x.toDouble()+w.toDouble()>width || y.toDouble()+h.toDouble()>height)
                return failure(QStringLiteral("E_OCR_BOUNDS"));
        } else {
            // Microsoft OcrResult.TextAngle: rotate word boxes clockwise around
            // the ORIGINAL image center, not around each word or the origin.
            const double limit = 2.0 * std::max(width,height);
            if (std::abs(x.toDouble())>limit || std::abs(y.toDouble())>limit || w.toDouble()>limit || h.toDouble()>limit)
                return failure(QStringLiteral("E_OCR_BOUNDS"));
            const QRectF original(x.toDouble(),y.toDouble(),w.toDouble(),h.toDouble());
            double left=limit*3, top=limit*3, right=-limit*3, bottom=-limit*3;
            for (const auto& point : {original.topLeft(),original.topRight(),original.bottomLeft(),original.bottomRight()}) {
                const double dx=point.x()-width/2.0, dy=point.y()-height/2.0;
                const double px=width/2.0+dx*cosine-dy*sine, py=height/2.0+dx*sine+dy*cosine;
                left=std::min(left,px);right=std::max(right,px);top=std::min(top,py);bottom=std::max(bottom,py);
            }
            const QRectF clipped=QRectF(left,top,right-left,bottom-top).intersected(QRectF(0,0,width,height));
            if (clipped.isEmpty()) { ++droppedOutside; continue; }
            word["x"]=clipped.x();word["y"]=clipped.y();word["width"]=clipped.width();word["height"]=clipped.height();
        }
        mappedWords.append(word);
    }
    object["words"]=mappedWords;
    object["coordinate_space"]="client_physical_px";
    object["source_text_angle_degrees"]=rotatedCoordinates ? QJsonValue(angle) : QJsonValue(QJsonValue::Null);
    object["coordinate_transform"]=rotatedCoordinates ? "clockwise_about_image_center" : "legacy_axis_aligned";
    object["outside_frame_tokens_dropped"]=droppedOutside;
    return {true, {}, object};
}

QJsonObject summarizeRecognizedPage(const QJsonObject& observation) {
    // Navigation labels occur on every game page: "交易行" by itself is not a
    // market-page proof. Only collect known, non-personal body anchors here.
    QString body;
    const int height = observation.value("height").toInt();
    for (const auto& value : observation.value("words").toArray()) {
        const auto word = value.toObject();
        const double y = word.value("y").toDouble();
        if (y >= height * 0.15 && y < height * 0.80) body += word.value("text").toString();
    }
    QJsonArray anchors;
    for (const auto& text : {QStringLiteral("技术中心"), QStringLiteral("工作台"), QStringLiteral("指挥中心"),
                            QStringLiteral("训练中心"), QStringLiteral("靶场"), QStringLiteral("净水中心"), QStringLiteral("收藏室")})
        if (body.contains(text)) anchors.append(text);
    return {{"page_hint", anchors.size() >= 3 ? "base" : "unknown"}, {"body_anchors", anchors},
        {"word_count", observation.value("words").toArray().size()}, {"business_fields_validated", false},
        {"confidence_available", false}};
}

WindowsOcrRecognizer::WindowsOcrRecognizer(QString helperPath, QString language, int timeoutMs)
    : m_helperPath(std::move(helperPath)), m_language(std::move(language)), m_timeoutMs(timeoutMs) {}

OcrReply WindowsOcrRecognizer::recognizeRegion(const runtime::observation::FrameEnvelope& frame, const QRect& region,int scale,bool invert) {
    if (!summarizeBgra(frame.pixels,frame.width,frame.height,frame.strideBytes).valid
        || frame.pixelFormat!="BGRA8" || frame.validBytes!=frame.pixels.size()
        || region.isEmpty() || !QRect(0,0,frame.width,frame.height).contains(region)
        || frame.strideBytes != frame.width*4 || scale<1 || scale>3
        || qint64(region.width())*scale>4096 || qint64(region.height())*scale>4096)
        return failure(QStringLiteral("E_OCR_REGION"));
    auto cropped=frame;
    cropped.width=region.width()*scale;cropped.height=region.height()*scale;cropped.strideBytes=cropped.width*4;
    cropped.validBytes=qint64(cropped.strideBytes)*cropped.height;cropped.pixels.resize(cropped.validBytes);
    int minimum=255,maximum=0;
    for(int y=0;y<region.height();++y){
        const auto* row=reinterpret_cast<const unsigned char*>(frame.pixels.constData())+qsizetype(y+region.y())*frame.strideBytes+region.x()*4;
        for(int x=0;x<region.width();++x){const int gray=(row[x*4]+2*row[x*4+1]+row[x*4+2])/4;minimum=std::min(minimum,gray);maximum=std::max(maximum,gray);}
    }
    for(int y=0;y<region.height();++y){
        char* row=cropped.pixels.data()+qsizetype(y*scale)*cropped.strideBytes;
        const char* original=frame.pixels.constData()+qsizetype(y+region.y())*frame.strideBytes+region.x()*4;
        for(int x=0;x<region.width();++x){
            const auto* pixel=reinterpret_cast<const unsigned char*>(original+x*4);
            const int gray=(pixel[0]+2*pixel[1]+pixel[2])/4;
            const int contrasted=maximum-minimum>=8 ? (gray-minimum)*255/(maximum-minimum) : gray;
            const unsigned char value=invert ? 255-contrasted : contrasted;
            for(int i=0;i<scale;++i){auto* out=reinterpret_cast<unsigned char*>(row+(x*scale+i)*4);out[0]=out[1]=out[2]=value;out[3]=255;}
        }
        for(int i=1;i<scale;++i)std::memcpy(row+i*cropped.strideBytes,row,cropped.strideBytes);
    }
    auto result=recognizeFrame(cropped);
    if(!result.ok)return result;
    auto words=result.observation.value("words").toArray();
    for(int i=0;i<words.size();++i){auto word=words[i].toObject();
        word["x"]=word.value("x").toDouble()/scale+region.x();word["y"]=word.value("y").toDouble()/scale+region.y();
        word["width"]=word.value("width").toDouble()/scale;word["height"]=word.value("height").toDouble()/scale;words[i]=word;}
    result.observation["words"]=words;result.observation["width"]=frame.width;result.observation["height"]=frame.height;
    result.observation["coverage"]="roi";
    result.observation["roi_scale"]=scale;
    result.observation["roi_preprocess"]=invert?"grayscale_minmax_contrast_inverted":"grayscale_minmax_contrast";
    return result;
}

QString WindowsOcrRecognizer::recognize(const runtime::observation::FrameEnvelope& frame, QString* errorCode) {
    const auto reply = recognizeFrame(frame);
    if (errorCode) *errorCode = reply.error;
    return reply.ok ? QString::fromUtf8(QJsonDocument(reply.observation).toJson(QJsonDocument::Compact)) : QString();
}

OcrReply WindowsOcrRecognizer::recognizeFrame(const runtime::observation::FrameEnvelope& frame) {
    if (m_timeoutMs < 100 || m_timeoutMs > 15000 || (m_language != "zh-Hans-CN" && m_language != "en-US"))
        return failure(QStringLiteral("E_OCR_OPTIONS"));
    const auto summary = summarizeBgra(frame.pixels, frame.width, frame.height, frame.strideBytes);
    if (!summary.valid || frame.pixelFormat != "BGRA8" || frame.validBytes != frame.pixels.size()
        || frame.strideBytes != frame.width * 4) return failure(QStringLiteral("E_OCR_FRAME"));
    if (summary.nearBlack) return failure(QStringLiteral("E_FRAME_BLACK_OR_INVALID"));
    const QFileInfo helper(m_helperPath);
    if (!helper.isAbsolute() || !helper.isFile() || !helper.isReadable() || helper.isSymLink())
        return failure(QStringLiteral("E_OCR_HELPER"));
#ifdef Q_OS_WIN
    wchar_t systemDirectory[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) return failure(QStringLiteral("E_OCR_HOST"));
    const QString program = QString::fromWCharArray(systemDirectory) + QStringLiteral("/WindowsPowerShell/v1.0/powershell.exe");
    if (!QFileInfo::exists(program)) return failure(QStringLiteral("E_OCR_HOST"));
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    SharedFrameMemory mapping;
    QString error;
    if (!mapping.create(id, 0, frame.validBytes, &error) || !mapping.write(frame.pixels, 0, &error)) return failure(error);
    const QJsonObject input{{"protocol", "windows-ocr-once-v1"}, {"request_id", id},
        {"mapping_name", mapping.poolEntry().value("mapping_name")}, {"width", frame.width}, {"height", frame.height},
        {"bytes", frame.validBytes}, {"language", m_language}};
    QProcess process;
    process.setProgram(program);
    process.setArguments({QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
        QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"), helper.absoluteFilePath()});
    process.setWorkingDirectory(helper.absolutePath());
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
        a->flags |= CREATE_NO_WINDOW; a->startupInfo->dwFlags |= STARTF_USESHOWWINDOW; a->startupInfo->wShowWindow = SW_HIDE;
    });
    QElapsedTimer clock;
    clock.start();
    process.start();
    if (!process.waitForStarted(std::min(2000, m_timeoutMs))) return failure(QStringLiteral("E_OCR_START"));
    HelperUiState helperUi{static_cast<DWORD>(process.processId()), 0, false, false};
    inspectHelperUi(helperUi);
    const QByteArray request = QJsonDocument(input).toJson(QJsonDocument::Compact) + '\n';
    if (process.write(request) != request.size()) error = QStringLiteral("E_OCR_WRITE");
    process.closeWriteChannel();
    QByteArray output;
    qint64 stderrBytes = 0;
    auto drain = [&]() {
        inspectHelperUi(helperUi);
        output += process.readAllStandardOutput();
        stderrBytes += process.readAllStandardError().size();
        if (output.size() > 1048576 || stderrBytes > 65536) error = QStringLiteral("E_OCR_OUTPUT_LIMIT");
        if (helperUi.visible || helperUi.foreground) error = QStringLiteral("E_OCR_HELPER_VISIBLE");
    };
    while (error.isEmpty() && process.state() != QProcess::NotRunning && clock.elapsed() < m_timeoutMs) {
        process.waitForFinished(20); drain();
    }
    if (process.state() != QProcess::NotRunning) {
        if (error.isEmpty()) error = QStringLiteral("E_OCR_TIMEOUT");
        process.kill();
        if (!process.waitForFinished(1500)) return failure(QStringLiteral("E_OCR_EXIT_UNCONFIRMED"));
    }
    drain();
    // The reader is confirmed exited before the owner releases the mapping.
    mapping.close();
    if (!error.isEmpty()) return failure(error);
    if (process.exitStatus() != QProcess::NormalExit) return failure(QStringLiteral("E_OCR_CRASH"));
    auto reply = validateOcrReply(output, id, frame.width, frame.height);
    if (process.exitCode() != 0 && reply.ok) return failure(QStringLiteral("E_OCR_EXIT"));
    if (reply.ok && reply.observation.value("language") != m_language) return failure(QStringLiteral("E_OCR_LANGUAGE"));
    reply.helperUiChecks = helperUi.checks;
    reply.helperVisibleWindowObserved = helperUi.visible;
    reply.helperForegroundObserved = helperUi.foreground;
    return reply;
#else
    return failure(QStringLiteral("E_PLATFORM_UNSUPPORTED"));
#endif
}
} // namespace relink::vision
