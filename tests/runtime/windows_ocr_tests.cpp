#include "application/vision/windows_ocr.h"
#include "application/vision/target_window.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QTemporaryDir>
#include <iostream>
#include <cmath>

using namespace relink::vision;
using namespace relink::runtime::observation;
namespace {
int assertions = 0, failures = 0;
void check(bool value, const char* label) {
    ++assertions; if (!value) ++failures;
    std::cout << (value ? "PASS " : "FAIL ") << label << '\n';
}
QByteArray json(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
QJsonObject word(const QString& text, double y = 40) {
    return {{"text", text}, {"x", 10}, {"y", y}, {"width", 30}, {"height", 20}};
}
}
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    const auto foreground = foregroundWindow();
    const QString helper = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    QJsonObject valid{{"protocol", "windows-ocr-once-v1"}, {"request_id", "test"}, {"ok", true},
        {"width", 100}, {"height", 100}, {"language", "en-US"}, {"words", QJsonArray{word("MARKET")}}};
    check(validateOcrReply(json(valid), "test", 100, 100).ok, "valid_result_accepted");
    auto rotated = valid;
    rotated["coordinate_space"]="windows_ocr_rotated";
    rotated["words"]=QJsonArray{QJsonObject{{"text","MARKET"},{"x",60},{"y",40},{"width",20},{"height",10}}};
    for (double angle : {0.0,90.0,-90.0,180.0}) {
        rotated["text_angle"]=angle;
        const auto reply=validateOcrReply(json(rotated),"test",100,100);
        check(reply.ok && reply.observation.value("coordinate_space")=="client_physical_px", "native_angle_maps_to_client_coordinates");
        const auto box=reply.observation.value("words").toArray().first().toObject();
        double ex=60,ey=40,ew=20,eh=10;
        if(angle==90){ex=50;ey=60;ew=10;eh=20;}
        if(angle==-90){ex=40;ey=20;ew=10;eh=20;}
        if(angle==180){ex=20;ey=50;ew=20;eh=10;}
        check(std::abs(box["x"].toDouble()-ex)<.00001 && std::abs(box["y"].toDouble()-ey)<.00001
            && std::abs(box["width"].toDouble()-ew)<.00001 && std::abs(box["height"].toDouble()-eh)<.00001,
            "rotation_uses_clockwise_four_corners_about_image_center");
    }
    rotated["text_angle"]=QJsonValue(QJsonValue::Null);
    check(validateOcrReply(json(rotated),"test",100,100).ok,"null_native_angle_means_identity");
    for(const auto& angle : {QJsonValue("90"),QJsonValue(181),QJsonValue(-181)}) {
        rotated["text_angle"]=angle;
        check(validateOcrReply(json(rotated),"test",100,100).error=="E_OCR_ANGLE","malformed_native_angle_rejected");
    }
    rotated.remove("text_angle");
    check(validateOcrReply(json(rotated),"test",100,100).error=="E_OCR_COORDINATES","rotation_space_requires_angle_metadata");
    rotated=valid;rotated["text_angle"]=10;
    check(validateOcrReply(json(rotated),"test",100,100).error=="E_OCR_COORDINATES","unlabelled_angle_never_silently_ignored");
    rotated=valid;rotated["coordinate_space"]="windows_ocr_rotated";rotated["text_angle"]=0;
    rotated["words"]=QJsonArray{QJsonObject{{"text","edge"},{"x",95},{"y",20},{"width",10},{"height",10}}};
    auto clipped=validateOcrReply(json(rotated),"test",100,100);
    check(clipped.ok && clipped.observation["words"].toArray().first().toObject()["width"]==5,"rotated_edge_box_clips_to_original_frame");
    rotated["words"]=QJsonArray{QJsonObject{{"text","outside"},{"x",110},{"y",20},{"width",10},{"height",10}}};
    auto outside=validateOcrReply(json(rotated),"test",100,100);
    check(outside.ok && outside.observation["words"].toArray().isEmpty() && outside.observation["outside_frame_tokens_dropped"]==1,"fully_outside_box_counted_and_not_used");
    check(!validateOcrReply({}, "test", 100, 100).ok, "empty_result_rejected");
    check(!validateOcrReply(QByteArray(1048577, ' '), "test", 100, 100).ok, "oversized_result_rejected");
    check(!validateOcrReply("not-json", "test", 100, 100).ok, "malformed_json_rejected");
    check(!validateOcrReply("[]", "test", 100, 100).ok, "non_object_rejected");
    check(!validateOcrReply(json(valid), "other", 100, 100).ok, "wrong_request_rejected");
    check(!validateOcrReply(json(valid), "test", 101, 100).ok, "wrong_width_rejected");
    check(!validateOcrReply(json(valid), "test", 100, 101).ok, "wrong_height_rejected");
    check(!validateOcrReply(json(valid), {}, 100, 100).ok, "empty_request_id_rejected");
    auto bad = valid; bad["protocol"] = "wrong";
    check(!validateOcrReply(json(bad), "test", 100, 100).ok, "wrong_protocol_rejected");
    bad = valid; bad["ok"] = "true";
    check(!validateOcrReply(json(bad), "test", 100, 100).ok, "string_boolean_rejected");
    bad = valid; bad["words"] = QJsonObject{};
    check(!validateOcrReply(json(bad), "test", 100, 100).ok, "non_array_words_rejected");
    bad = valid; bad["words"] = QJsonArray{QStringLiteral("not-word")};
    check(!validateOcrReply(json(bad), "test", 100, 100).ok, "non_object_word_rejected");
    QJsonArray many;
    for (int i = 0; i < 2001; ++i) many.append(word("x"));
    bad = valid; bad["words"] = many;
    check(!validateOcrReply(json(bad), "test", 100, 100).ok, "word_budget_enforced");
    for (const QString& field : {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"), QStringLiteral("height")}) {
        for (const QJsonValue& value : {QJsonValue(-1), QJsonValue(200), QJsonValue("1"), QJsonValue(QJsonValue::Null)}) {
            auto w = word("test"); w[field] = value;
            bad = valid; bad["words"] = QJsonArray{w};
            check(!validateOcrReply(json(bad), "test", 100, 100).ok, "invalid_token_bounds_rejected");
        }
    }
    for (const QString& text : {QString(), QString(1025, QLatin1Char('x'))}) {
        bad = valid; bad["words"] = QJsonArray{word(text)};
        check(!validateOcrReply(json(bad), "test", 100, 100).ok, "invalid_token_text_rejected");
    }
    bad = valid; bad["ok"] = false; bad["error"] = "E_OCR_LANGUAGE";
    check(validateOcrReply(json(bad), "test", 100, 100).error == "E_OCR_LANGUAGE", "missing_language_preserved");
    bad["error"] = "unexpected-private-exception";
    check(validateOcrReply(json(bad), "test", 100, 100).error == "E_OCR_ENGINE", "arbitrary_worker_exception_not_echoed");
    bad = valid; bad["words"] = QJsonArray{word(QStringLiteral("交易行"), 2)};
    check(summarizeRecognizedPage(bad).value("page_hint") == "unknown", "navigation_text_is_not_market_page_proof");
    bad["words"] = QJsonArray{word(QStringLiteral("技术中心")), word(QStringLiteral("工作台")), word(QStringLiteral("指挥中心"))};
    check(summarizeRecognizedPage(bad).value("page_hint") == "base", "three_body_anchors_identify_base_hint");
    check(!summarizeRecognizedPage(bad).value("business_fields_validated").toBool(), "page_hint_not_business_verification");
    bad["words"] = QJsonArray{word(QStringLiteral("技术中心"), 2), word(QStringLiteral("工作台"), 2), word(QStringLiteral("指挥中心"), 2)};
    check(summarizeRecognizedPage(bad).value("page_hint") == "unknown", "header_anchors_excluded");
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/segoeui.ttf");
    QImage image(720, 140, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image); QFont font("Segoe UI"); font.setPixelSize(54);
    painter.setFont(font); painter.setPen(Qt::black); painter.drawText(30, 88, "MARKET 123456"); painter.end();
    FrameEnvelope frame;
    frame.width = image.width(); frame.height = image.height(); frame.strideBytes = image.bytesPerLine();
    frame.pixels = QByteArray(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes());
    frame.validBytes = frame.pixels.size();
    WindowsOcrRecognizer recognizer(helper, "en-US");
    auto result = recognizer.recognizeFrame(frame);
    if (!result.ok) std::cout << "OCR_NATIVE_ERROR=" << result.error.toStdString() << '\n';
    check(result.ok, "actual_windows_ocr_on_synthetic_memory_frame");
    QString text;
    for (const auto& value : result.observation.value("words").toArray()) text += value.toObject().value("text").toString() + ' ';
    check(text.contains("MARKET") && text.contains("123456"), "synthetic_ocr_text_verified_not_hash_token");
    check(result.observation.value("language") == "en-US", "exact_recognizer_language");
    check(result.helperUiChecks > 0 && !result.helperVisibleWindowObserved && !result.helperForegroundObserved,
          "actual_helper_has_no_observed_visible_window_or_foreground_ownership");
    const auto originalPixels=frame.pixels;
    const auto regionReply=recognizer.recognizeRegion(frame,QRect(20,20,650,100));
    check(regionReply.ok && regionReply.observation.value("coverage")=="roi"
        && regionReply.observation.value("width")==frame.width,"region_ocr_returns_client_coordinates_but_partial_coverage");
    QString regionText; bool regionBounds=true;
    for(const auto& v:regionReply.observation.value("words").toArray()) {const auto w=v.toObject();regionText+=w.value("text").toString();
        regionBounds &= w.value("x").toDouble()>=20 && w.value("y").toDouble()>=20;}
    check(regionText.contains("MARKET") && regionBounds,"actual_region_text_translated_to_parent_frame");
    check(recognizer.recognizeRegion(frame,QRect(-1,0,10,10)).error=="E_OCR_REGION","out_of_frame_roi_rejected");
    check(frame.pixels==originalPixels && regionReply.observation["roi_scale"]==2
        && regionReply.observation["roi_preprocess"]=="grayscale_minmax_contrast","contrast_upscale_keeps_original_frame_unchanged");
    const auto inverted=recognizer.recognizeRegion(frame,QRect(20,20,650,100),3,true);
    QString invertedText;for(const auto& word:inverted.observation["words"].toArray())invertedText+=word.toObject()["text"].toString();
    check(inverted.ok && invertedText.contains("MARKET") && inverted.observation["roi_scale"]==3,
        "actual_inverted_three_times_roi_preserves_text_and_client_scale");
    check(recognizer.recognizeRegion(frame,QRect(20,20,650,100),0).error=="E_OCR_REGION","zero_roi_scale_rejected");
    check(recognizer.recognizeRegion(frame,QRect(20,20,650,100),4).error=="E_OCR_REGION","excessive_roi_scale_rejected");
    auto invalidFrame = frame; invalidFrame.pixelFormat = "RGB8";
    check(recognizer.recognizeFrame(invalidFrame).error == "E_OCR_FRAME", "pixel_format_rejected");
    invalidFrame = frame; ++invalidFrame.validBytes;
    check(recognizer.recognizeFrame(invalidFrame).error == "E_OCR_FRAME", "invalid_bytes_rejected");
    invalidFrame = frame; invalidFrame.pixels.fill('\0');
    check(recognizer.recognizeFrame(invalidFrame).error == "E_FRAME_BLACK_OR_INVALID", "black_frame_not_ocr_success");
    check(WindowsOcrRecognizer("relative.ps1", "en-US").recognizeFrame(frame).error == "E_OCR_HELPER", "relative_helper_rejected");
    check(WindowsOcrRecognizer(helper, "xx", 8000).recognizeFrame(frame).error == "E_OCR_OPTIONS", "unknown_language_not_silently_replaced");
    check(WindowsOcrRecognizer(helper, "en-US", 0).recognizeFrame(frame).error == "E_OCR_OPTIONS", "zero_timeout_rejected");
    check(WindowsOcrRecognizer(helper, "en-US", 15001).recognizeFrame(frame).error == "E_OCR_OPTIONS", "excessive_timeout_rejected");
    QTemporaryDir temp;
    const auto fixturePath = temp.filePath("slow.ps1");
    QFile fixture(fixturePath); fixture.open(QIODevice::WriteOnly); fixture.write("Start-Sleep -Seconds 20\n"); fixture.close();
    QElapsedTimer timer; timer.start();
    result = WindowsOcrRecognizer(fixturePath, "en-US", 300).recognizeFrame(frame);
    check(result.error == "E_OCR_TIMEOUT", "slow_helper_deadline_enforced");
    check(timer.elapsed() < 3000, "slow_helper_terminated_and_reaped");
    // The user may switch apps during a background test. Attribute focus only
    // to the actual helper PID (sampled above), not any global HWND change.
    if (foregroundWindow() != foreground)
        std::cout << "EXTERNAL_FOREGROUND_CHANGE_OBSERVED=true; not_attributed_to_ocr=true\n";
    check(QDir(temp.path()).entryList({"*.png", "*.jpg", "*.bmp"}, QDir::Files).isEmpty(), "no_test_image_files");
    std::cout << "WINDOWS_OCR_TESTS=" << (failures ? "FAIL" : "PASS") << "; assertions=" << assertions
        << "; failures=" << failures << "; image_file_writes=0; real_ocr=true; synthetic_input=true\n";
    return failures ? 1 : 0;
}
