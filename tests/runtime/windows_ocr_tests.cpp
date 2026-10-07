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
    check(foregroundWindow() == foreground, "hidden_ocr_test_preserves_foreground");
    check(QDir(temp.path()).entryList({"*.png", "*.jpg", "*.bmp"}, QDir::Files).isEmpty(), "no_test_image_files");
    std::cout << "WINDOWS_OCR_TESTS=" << (failures ? "FAIL" : "PASS") << "; assertions=" << assertions
        << "; failures=" << failures << "; image_file_writes=0; real_ocr=true; synthetic_input=true\n";
    return failures ? 1 : 0;
}
