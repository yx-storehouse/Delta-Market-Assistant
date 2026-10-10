#include "application/vision/windows_ocr.h"
#include "application/vision/numeric_roi.h"
#include "application/vision/target_window.h"
#include "application/vision/shared_frame_memory.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <iostream>
#include <cmath>
#include <thread>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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
bool mappingGone(const QString& name) {
#ifdef Q_OS_WIN
    const auto wide = name.toStdWString();
    HANDLE handle = OpenFileMappingW(FILE_MAP_READ, FALSE, wide.c_str());
    if (handle) { CloseHandle(handle); return false; }
    return GetLastError() == ERROR_FILE_NOT_FOUND;
#else
    Q_UNUSED(name); return true;
#endif
}
bool processGone(qint64 pid) {
#ifdef Q_OS_WIN
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!handle) return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool gone = WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
    CloseHandle(handle);
    return gone;
#else
    Q_UNUSED(pid); return true;
#endif
}
void configureHelper(QProcess& process, const QString& helper, const QStringList& extra = {}) {
    process.setProgram(qEnvironmentVariable("WINDIR", "C:/Windows") + "/System32/WindowsPowerShell/v1.0/powershell.exe");
    QStringList arguments{"-NoLogo","-NoProfile","-NonInteractive","-ExecutionPolicy","Bypass","-File",helper};
    arguments.append(extra);
    if (extra.contains(QStringLiteral("-Session"))) arguments.append(QStringLiteral("-OverlappedStdin"));
    process.setArguments(arguments);
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
        a->flags |= CREATE_NO_WINDOW;
        a->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        a->startupInfo->wShowWindow = SW_HIDE;
    });
#endif
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
    frame.frameId = QStringLiteral("synthetic:ocr-session-stable-frame");
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
    check(recognizer.sessionMetrics()["helper_start_count"]==1 && recognizer.sessionMetrics()["request_count"]==3
          && recognizer.sessionMetrics()["engine_create_count"]==1, "three_actual_ocr_calls_share_one_hidden_helper_and_engine");
    check(result.timing["helper_reused"]==false && regionReply.timing["helper_reused"]==true
          && inverted.timing["engine_cache_hit"]==true, "cold_then_warm_calls_have_observed_reuse_metrics");
    check(result.timing["helper_pid"]==regionReply.timing["helper_pid"]
          && result.timing["helper_pid"]==inverted.timing["helper_pid"], "roi_helper_pid_is_stable");
    check(result.observation["frame_id"]==frame.frameId && regionReply.observation["frame_id"]==frame.frameId
          && inverted.observation["frame_id"]==frame.frameId, "full_and_roi_results_remain_bound_to_same_source_frame");
    check(result.timing["request_id"]!=regionReply.timing["request_id"]
          && regionReply.timing["request_id"]!=inverted.timing["request_id"], "shared_source_frame_still_has_unique_request_correlations");
    for (const auto& call : {result,regionReply,inverted}) {
        check(call.timing["frame_released"]==true && mappingGone(call.timing["mapping_name"].toString()),
              "each_completed_call_releases_mapping_while_worker_stays_running");
        check(call.timing["frame_total_ms"].toDouble()>=0 && call.timing["worker_recognize_ms"].toDouble()>=0,
              "real_call_and_engine_timings_recorded");
        for(const auto& value:call.observation["words"].toArray())
            check(!value.toObject().contains("score"), "windows_ocr_does_not_invent_confidence");
    }
    {
        WindowsOcrRecognizer chinese(helper,"zh-Hans-CN");
        const auto translated=chinese.recognizeRegion(frame,QRect(20,20,650,100));
        check(translated.ok && translated.observation["language"]=="zh-Hans-CN", "second_real_language_engine_created");
        check(translated.timing["helper_pid"]==result.timing["helper_pid"]
              && chinese.sessionMetrics()["helper_start_count"]==1, "temporary_language_recognizer_shares_session_owner");
    }
    {
        WindowsOcrRecognizer chineseAgain(helper,"zh-Hans-CN");
        const auto translated=chineseAgain.recognizeFrame(frame);
        check(translated.ok && translated.timing["engine_cache_hit"]==true
              && translated.timing["helper_pid"]==result.timing["helper_pid"], "language_engine_survives_temporary_recognizer_destruction");
    }
    check(recognizer.sessionMetrics()["helper_start_count"]==1 && recognizer.sessionMetrics()["request_count"]==5
          && recognizer.sessionMetrics()["engine_create_count"]==2, "five_real_reads_use_one_helper_two_language_engines");
    {
        // Standby (2026-10-08): a helper that exited on its own idle limit was
        // written to and poisoned its session. Each case runs on a fresh thread,
        // i.e. a fresh session with its own hidden helper.
        auto idleCase = [&](int helperIdleMs, int restartAfterMs, int pauseMs) {
            setWindowsOcrHelperIdlePolicy(helperIdleMs, restartAfterMs);
            OcrReply first, second;
            QJsonObject metrics;
            std::thread worker([&] {
                WindowsOcrRecognizer idle(helper, "en-US");
                first = idle.recognizeFrame(frame);
                QThread::msleep(pauseMs);
                second = idle.recognizeFrame(frame);
                metrics = idle.sessionMetrics();
            });
            worker.join();
            setWindowsOcrHelperIdlePolicy(600000, 570000);
            QString secondText;
            for (const auto& value : second.observation.value("words").toArray()) secondText += value.toObject().value("text").toString();
            return first.ok && second.ok && secondText.contains("MARKET") && metrics["session_poisoned"] == false
                && metrics["helper_start_count"] == 2 && first.timing["helper_pid"] != second.timing["helper_pid"]
                && second.timing["helper_reused"] == false;
        };
        check(idleCase(800, 3600000, 2500), "helper_that_exited_on_its_own_idle_limit_is_restarted_not_poisoned");
        check(idleCase(60000, 400, 900), "helper_near_its_idle_limit_is_retired_gracefully_and_restarted");
    }
    {
        // Review 2026-10-09: a pool worker whose helper fails for good must
        // retire, and its jobs must be re-read on the caller. A helper copy
        // whose replay cap is 2 (instead of 4096) breaks right after warm-up.
        QTemporaryDir cappedDirectory;
        const QString capped = cappedDirectory.filePath("capped.ps1");
        QFile original(helper);
        QByteArray text = original.open(QIODevice::ReadOnly) ? original.readAll() : QByteArray();
        check(text.contains("$seenRequests.Count -gt 4096"), "helper_replay_cap_is_where_the_request_limit_expects_it");
        text.replace("$seenRequests.Count -gt 4096", "$seenRequests.Count -gt 2");
        QFile copy(capped);
        check(copy.open(QIODevice::WriteOnly) && copy.write(text) == text.size(), "capped_helper_written");
        copy.close();
        const auto marketRead = [](const OcrReply& reply) {
            QString words;
            for (const auto& value : reply.observation.value("words").toArray()) words += value.toObject().value("text").toString();
            return reply.ok && words.contains("MARKET");
        };
        {
            WindowsOcrPool pool(capped, 2);
            check(pool.waitWarm(20000), "capped_pool_warms_up");
            WindowsOcrRecognizer callerChinese(helper, "zh-Hans-CN"), callerLatin(helper, "en-US");
            QList<OcrRegionJob> jobs;
            for (int i = 0; i < 8; ++i) jobs.append({"en-US", QRect(20, 20, 650, 100), 2, false, false});
            bool allRead = true;
            for (int batch = 0; batch < 2; ++batch)
                for (const auto& reply : pool.run(frame, jobs, callerChinese, callerLatin)) allRead &= marketRead(reply);
            const auto metrics = pool.metrics();
            std::cout << "OCR_POOL_CONTAINMENT=" << json(metrics).toStdString() << '\n';
            check(allRead, "jobs_failed_by_a_broken_pool_worker_are_reread_on_the_caller");
            check(metrics["caller_reruns"].toInt() >= 1 && !metrics["retired_after_session_failure"].toArray().isEmpty(),
                  "broken_pool_worker_retires_instead_of_failing_later_jobs");
        }
        setWindowsOcrHelperRequestLimit(2);
        bool limitedOk = true;
        QJsonObject limitedMetrics;
        std::thread limitedThread([&] {
            WindowsOcrRecognizer limited(capped, "en-US");
            for (int i = 0; i < 6; ++i) limitedOk &= marketRead(limited.recognizeFrame(frame));
            limitedMetrics = limited.sessionMetrics();
        });
        limitedThread.join();
        setWindowsOcrHelperRequestLimit(4000);
        check(limitedOk && limitedMetrics["session_poisoned"] == false && limitedMetrics["helper_start_count"].toInt() >= 3,
              "session_restarts_its_helper_before_the_helper_request_cap");
    }
    {
        // Pool workers own separate hidden helpers on their own threads; one
        // frame's ROIs keep job order and equal the serial readings.
        WindowsOcrPool pool(helper, 2);
        check(pool.waitWarm(20000),"pool_helpers_warm_up_in_background");
        WindowsOcrRecognizer callerChinese(helper,"zh-Hans-CN");
        const QList<OcrRegionJob> jobs{{"en-US",QRect(20,20,650,100),2,false,false},{"en-US",QRect(20,20,650,100),3,true,false},
            {"zh-Hans-CN",QRect(20,20,650,100),2,false,false},{"en-US",QRect(20,20,650,100),2,false,false}};
        const auto untouched=frame.pixels;
        QList<OcrReply> replies;
        for(int round=0;round<3;++round)replies=pool.run(frame,jobs,callerChinese,recognizer);
        bool ordered=replies.size()==jobs.size(),sameText=true;QSet<int> executors;
        for(int i=0;ordered &&i<replies.size();++i) {
            ordered=replies[i].ok &&replies[i].observation["frame_id"]==frame.frameId
                &&replies[i].observation["roi_scale"]==jobs[i].scale &&replies[i].observation["language"]==jobs[i].language;
            executors.insert(replies[i].timing["ocr_pool_executor"].toInt());
            QString text;for(const auto& word:replies[i].observation["words"].toArray())text+=word.toObject()["text"].toString();
            if(jobs[i].language=="en-US")sameText=sameText &&text.contains("MARKET");
        }
        check(ordered &&sameText,"pool_results_keep_job_order_language_scale_and_actual_text");
        check(frame.pixels==untouched,"pool_reads_do_not_modify_the_shared_frame");
        const auto metrics=pool.metrics();
        check(metrics["warmed"]==2 &&metrics["retired"]==0 &&metrics["batches"]==3
            &&metrics["worker_jobs"].toInt()+metrics["caller_jobs"].toInt()==12,"pool_workers_warm_and_share_every_batch");
        check(executors.size()>=2,"pool_batch_runs_on_more_than_one_helper");
        check(pool.run(frame,{},callerChinese,recognizer).isEmpty(),"empty_pool_batch_returns_without_work");
        // Informational wall-clock comparison on one larger synthetic frame.
        QImage wide(2560,1440,QImage::Format_RGB32);wide.fill(Qt::white);
        QPainter widePainter(&wide);QFont wideFont("Segoe UI");wideFont.setPixelSize(30);widePainter.setFont(wideFont);widePainter.setPen(Qt::black);
        for(int row=0;row<16;++row)widePainter.drawText(720,400+row*34,QStringLiteral("LISTING %1 PRICE 230 WEAR 0.0%2").arg(row).arg(row*7));
        widePainter.drawText(190,120,"SALE");widePainter.drawText(140,270,"DEFAULT SORT PERIOD");widePainter.drawText(1930,620,"DETAIL ANCHOR");widePainter.end();
        FrameEnvelope wideFrame=frame;wideFrame.frameId=QStringLiteral("synthetic:pool-wide");wideFrame.width=wide.width();wideFrame.height=wide.height();
        wideFrame.strideBytes=wide.bytesPerLine();wideFrame.pixels=QByteArray(reinterpret_cast<const char*>(wide.constBits()),wide.sizeInBytes());
        wideFrame.validBytes=wideFrame.pixels.size();
        const QList<OcrRegionJob> listing{{"zh-Hans-CN",QRect(700,370,1160,570),1,true,false},{"zh-Hans-CN",QRect(110,230,1760,70),2,true,false},
            {"zh-Hans-CN",QRect(1905,560,540,190),2,true,false},{"zh-Hans-CN",QRect(170,64,300,96),2,true,false}};
        QElapsedTimer serialClock;serialClock.start();
        for(const auto& job:listing)runOcrRegionJob(wideFrame,job,callerChinese,recognizer);
        const auto serialMs=serialClock.elapsed();
        QElapsedTimer poolClock;poolClock.start();
        const auto pooled=pool.run(wideFrame,listing,callerChinese,recognizer);
        const auto poolMs=poolClock.elapsed();
        bool pooledOk=true;for(const auto& reply:pooled)pooledOk=pooledOk &&reply.ok;
        check(pooledOk,"pool_reads_four_listing_shaped_rois_of_one_frame");
        std::cout<<"OCR_POOL_TIMING serial_ms="<<serialMs<<" pool_ms="<<poolMs<<'\n';
        check(pool.run(frame,{{"de-DE",QRect(20,20,10,10),2,true,false}},callerChinese,recognizer).first().error=="E_OCR_OPTIONS",
            "pool_rejects_unsupported_language");
    }
    // Actual Windows OCR on generated numeric UI fields. These are synthetic
    // pixels, not recorded game samples, and no image is written to disk.
    for(const QString& amount:{QStringLiteral("7"),QStringLiteral("30"),QStringLiteral("300"),
            QStringLiteral("900"),QStringLiteral("1,200"),QStringLiteral("1234567"),QStringLiteral("130"),
            QStringLiteral("230"),QStringLiteral("380"),QStringLiteral("12500"),QStringLiteral("7000000"),QStringLiteral("90000000")}){
        QImage numericImage(260,80,QImage::Format_RGB32);
        for(int y=0;y<numericImage.height();++y)for(int x=0;x<numericImage.width();++x){
            const int gray=70+y/3+x/30;numericImage.setPixelColor(x,y,QColor(gray,gray,gray));}
        const QRect numericRoi(55,17,149,42);
        QPainter numericPainter(&numericImage);numericPainter.setRenderHint(QPainter::TextAntialiasing,true);
        QFont digitsFont("Bahnschrift");digitsFont.setPixelSize(20);numericPainter.setFont(digitsFont);numericPainter.setPen(QColor(240,240,240));
        const QFontMetrics digitsMetrics(digitsFont);
        numericPainter.drawText(numericRoi.right()-7-digitsMetrics.horizontalAdvance(amount),numericRoi.y()+30,amount);
        numericPainter.drawEllipse(QRect(numericRoi.x()+16,numericRoi.y()+13,15,15));
        numericPainter.fillRect(numericRoi.x(),numericRoi.bottom()-1,numericRoi.width(),2,QColor(249,249,249));
        numericPainter.fillRect(numericRoi.right()-1,numericRoi.y(),2,numericRoi.height(),QColor(249,249,249));numericPainter.end();
        FrameEnvelope numericFrame=frame;numericFrame.frameId=QStringLiteral("synthetic:numeric-field");
        numericFrame.width=numericImage.width();numericFrame.height=numericImage.height();numericFrame.strideBytes=numericImage.bytesPerLine();
        numericFrame.pixels=QByteArray(reinterpret_cast<const char*>(numericImage.constBits()),numericImage.sizeInBytes());numericFrame.validBytes=numericFrame.pixels.size();
        const auto untouched=numericFrame.pixels;
        const auto prepared=prepareNumericRegion(numericFrame,numericRoi);
        check(prepared.ok,"numeric_ink_is_measured_without_recognizing_a_value");
        if(!prepared.ok){std::cout<<"NUMERIC_PREP_ERROR="<<amount.toStdString()<<":"<<prepared.error.toStdString()<<'\n';continue;}
        check(prepared.metadata["edge_line_pixels_removed"].toInt()>100,"observed_frame_lines_are_removed_before_number_ocr");
        check(prepared.inkBounds.left()>numericRoi.x()+40,"separate_currency_component_is_not_in_digit_crop");
        check(prepared.frame.frameId==numericFrame.frameId &&prepared.paddingPixels==15,"prepared_numeric_pixels_keep_source_identity_and_explicit_padding");
        bool whitePadding=true;for(int i=0;i<prepared.frame.width;++i){const auto* pixel=reinterpret_cast<const unsigned char*>(prepared.frame.pixels.constData())+i*4;whitePadding&=pixel[0]==255&&pixel[1]==255&&pixel[2]==255;}
        check(whitePadding,"prepared_bitmap_has_plain_white_padding");
        if(qEnvironmentVariableIsSet("RELINK_NUMERIC_MATRIX")){
            for(int sampleScale:{2,3})for(int samplePadding:{3,5,8,12,16}){
                const auto sample=recognizer.recognizeNumericRegion(numericFrame,numericRoi,sampleScale,samplePadding);
                QString sampleText;for(const auto& value:sample.observation["words"].toArray())sampleText+=value.toObject()["text"].toString();
                std::cout<<"NUMERIC_MATRIX expected="<<amount.toStdString()<<" scale="<<sampleScale<<" padding="<<samplePadding
                    <<" actual="<<sampleText.toStdString()<<" error="<<sample.error.toStdString()<<'\n';
            }
        }
        if(qEnvironmentVariableIsSet("RELINK_NUMERIC_MATRIX")){
            WindowsOcrRecognizer numericChinese(helper,"zh-Hans-CN");
            const auto sample=numericChinese.recognizeNumericRegion(numericFrame,numericRoi,3,5);
            QString sampleText;for(const auto& value:sample.observation["words"].toArray())sampleText+=value.toObject()["text"].toString();
            std::cout<<"NUMERIC_CHINESE expected="<<amount.toStdString()<<" actual="<<sampleText.toStdString()<<" error="<<sample.error.toStdString()<<'\n';
        }
        const auto number=recognizer.recognizeNumericRegion(numericFrame,numericRoi);
        QString read;for(const auto& value:number.observation["words"].toArray())read+=value.toObject()["text"].toString();
        std::cout<<"NUMERIC_SYNTHETIC="<<amount.toStdString()<<"; actual="<<read.toStdString()<<"; error="<<number.error.toStdString()<<"; metadata="<<json(prepared.metadata).toStdString()<<'\n';
        QString actualDigits=read.normalized(QString::NormalizationForm_KC);actualDigits.remove(',');
        QString expectedDigits=amount;expectedDigits.remove(',');
        if(amount=="7")check((number.ok &&actualDigits==expectedDigits) ||(!number.ok &&number.error=="E_OCR_NUMERIC_EMPTY" &&read.isEmpty()),
            "single_character_engine_abstention_does_not_become_invented_numeric_success");
        else check(number.ok &&actualDigits==expectedDigits,"actual_windows_ocr_reads_all_synthetic_price_digits");
        check(number.observation["frame_id"]==numericFrame.frameId &&number.observation["width"]==numericFrame.width
            &&number.observation["height"]==numericFrame.height,"numeric_ocr_returns_original_client_geometry");
        check(numericFrame.pixels==untouched,"numeric_preprocessing_does_not_change_original_frame");
        const QRect ink=prepared.inkBounds;
        QJsonObject preparedObservation{{"coordinate_space","client_physical_px"},{"frame_id",numericFrame.frameId},
            {"width",prepared.frame.width},{"height",prepared.frame.height},
            {"words",QJsonArray{QJsonObject{{"text",amount},
                {"x",(ink.x()-prepared.cropBounds.x())*prepared.scale+prepared.paddingPixels},
                {"y",(ink.y()-prepared.cropBounds.y())*prepared.scale+prepared.paddingPixels},
                {"width",ink.width()*prepared.scale},{"height",ink.height()*prepared.scale}}}}};
        QString mappingError;const auto mapped=mapNumericRegionObservation(prepared,preparedObservation,&mappingError);
        const auto mappedWord=mapped["words"].toArray().first().toObject();
        check(mappingError.isEmpty() &&mappedWord["x"]==ink.x() &&mappedWord["y"]==ink.y()
            &&mappedWord["width"]==ink.width() &&mappedWord["height"]==ink.height(),"numeric_padding_crop_inverse_mapping_is_exact");
        if(amount=="300"){
            auto shortObservation=preparedObservation;auto shortWord=shortObservation["words"].toArray().first().toObject();
            shortWord["text"]="30";shortWord["width"]=shortWord["width"].toInt()-12*prepared.scale;shortObservation["words"]=QJsonArray{shortWord};
            const auto incomplete=mapNumericRegionObservation(prepared,shortObservation,&mappingError);
            check(mappingError=="E_OCR_NUMERIC_INCOMPLETE" &&incomplete["words"].toArray().first().toObject()["text"]=="30",
                "missing_final_zero_is_rejected_by_pixel_coverage_not_completed_from_expected_price");
        }
        if(amount=="300" ||amount=="1234567"){
            auto dropped=preparedObservation;auto droppedWord=dropped["words"].toArray().first().toObject();
            droppedWord["text"]=amount=="300"?"30":"124567";dropped["words"]=QJsonArray{droppedWord};
            const auto rejected=mapNumericRegionObservation(prepared,dropped,&mappingError);
            check(mappingError=="E_OCR_NUMERIC_DIGITS_MISSING" &&rejected["words"].toArray().first().toObject()["text"]==droppedWord["text"],
                "full_width_bbox_does_not_hide_a_missing_middle_or_final_digit");
        }
        if(amount=="300"){
            FrameEnvelope joined=numericFrame;
            for(int x=ink.left();x<=ink.right();++x){auto* pixel=reinterpret_cast<unsigned char*>(joined.pixels.data())+qint64(ink.center().y())*joined.strideBytes+x*4;pixel[0]=pixel[1]=pixel[2]=240;}
            const auto joinedPrepared=prepareNumericRegion(joined,numericRoi);
            check(joinedPrepared.ok &&joinedPrepared.metadata["reliable_digit_span_minimum"].toInt()<=1,"joined_multi_digit_components_do_not_create_an_equality_requirement");
            auto relaxed=prepared;relaxed.metadata["reliable_digit_span_minimum"]=joinedPrepared.metadata["reliable_digit_span_minimum"];
            mapNumericRegionObservation(relaxed,preparedObservation,&mappingError);
            check(mappingError.isEmpty(),"more_actual_digits_than_the_reliable_minimum_are_not_rejected");
            FrameEnvelope fragments=numericFrame;
            for(int y=numericRoi.top();y<=numericRoi.bottom();++y)for(int x=numericRoi.left();x<=numericRoi.right();++x){
                auto* pixel=reinterpret_cast<unsigned char*>(fragments.pixels.data())+qint64(y)*fragments.strideBytes+x*4;pixel[0]=pixel[1]=pixel[2]=80;}
            for(const int start:{185,193})for(int y=33;y<47;++y)for(int x=start;x<start+2;++x){
                auto* pixel=reinterpret_cast<unsigned char*>(fragments.pixels.data())+qint64(y)*fragments.strideBytes+x*4;pixel[0]=pixel[1]=pixel[2]=240;}
            const auto split=prepareNumericRegion(fragments,numericRoi);
            check(split.ok &&split.metadata["glyph_span_count"].toInt()==2 &&split.metadata["reliable_digit_span_minimum"].toInt()==0,
                "two_tall_fragments_of_one_glyph_are_not_asserted_as_two_digits");
        }
        preparedObservation["frame_id"]="another-frame";mapNumericRegionObservation(prepared,preparedObservation,&mappingError);
        check(mappingError=="E_OCR_NUMERIC_MAPPING","numeric_mapping_does_not_accept_another_frame");
        check(!prepareNumericRegion(numericFrame,QRect(-1,0,149,42)).ok,"numeric_outside_region_rejected");
    }
    std::cout << "OCR_SESSION_METRICS=" << json(recognizer.sessionMetrics()).toStdString() << '\n';
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
    check(result.timing["helper_exit_confirmed"]==true && result.timing["frame_released"]==true
          && mappingGone(result.timing["mapping_name"].toString()), "timeout_confirms_exit_before_mapping_release");
    // Fault helpers hold the real read-only mapping while producing an invalid
    // response. The coordinator must kill/reap them before releasing its view.
    const QByteArray faultScript = R"PS($ErrorActionPreference='Stop'
$e=[Console]::ReadLine()|ConvertFrom-Json
$r=$e.request
$map=[IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting([string]$r.mapping_name,[IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
$view=$map.CreateViewStream(0,[long]$r.bytes,[IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
MODE_PLACEHOLDER
Start-Sleep -Seconds 20
)PS";
    const QList<QPair<QByteArray,QString>> faultModes{
        {"[Console]::WriteLine('{broken')", "E_OCR_RESPONSE"},
        {"[Console]::WriteLine('{\"protocol\":\"wrong\"}')", "E_OCR_CORRELATION"},
        {R"PS($response=@{protocol='windows-ocr-session-v1';session_id=$e.session_id;request_id=$r.request_id;frame_id=$e.frame_id;mapping_name=$r.mapping_name;frame_released=$false;result=@{};timing=@{}}
[Console]::WriteLine(($response|ConvertTo-Json -Depth 5 -Compress)))PS", "E_OCR_RELEASE"},
        {R"PS($response=@{protocol='windows-ocr-session-v1';session_id=$e.session_id;request_id=$r.request_id;frame_id='wrong-frame';mapping_name=$r.mapping_name;frame_released=$true;result=@{};timing=@{}}
[Console]::WriteLine(($response|ConvertTo-Json -Depth 5 -Compress)))PS", "E_OCR_CORRELATION"}};
    int faultIndex=0;
    for(const auto& mode:faultModes){
        const QString path=temp.filePath(QStringLiteral("fault-%1.ps1").arg(faultIndex++));
        QFile script(path); check(script.open(QIODevice::WriteOnly),"fault_helper_file_open");
        QByteArray contents=faultScript;contents.replace("MODE_PLACEHOLDER",mode.first);script.write(contents);script.close();
        WindowsOcrRecognizer faulty(path,"en-US",2500);
        const auto failed=faulty.recognizeFrame(frame);
        check(failed.error==mode.second,"malformed_or_uncorrelated_session_response_rejected");
        check(failed.timing["helper_exit_confirmed"]==true && failed.timing["frame_released"]==true
              && mappingGone(failed.timing["mapping_name"].toString()),"fault_reader_exits_before_owner_mapping_is_released");
        check(faulty.recognizeFrame(frame).error=="E_OCR_SESSION_POISONED"
              && faulty.sessionMetrics()["helper_start_count"]==1,"faulted_session_never_reuses_or_silently_restarts_reader");
    }
    qint64 ownedPid=0;
    const QString copiedHelper=temp.filePath("owned-session.ps1");
    check(QFile::copy(helper,copiedHelper),"isolated_owner_helper_copy");
    {
        WindowsOcrRecognizer owner(copiedHelper,"en-US");
        const auto actual=owner.recognizeFrame(frame);
        check(actual.ok,"isolated_session_owner_performs_actual_ocr");
        ownedPid=actual.timing["helper_pid"].toString().toLongLong();
        check(ownedPid>0 && !processGone(ownedPid),"session_is_alive_between_requests");
    }
    check(ownedPid>0 && processGone(ownedPid),"last_recognizer_owner_confirms_hidden_worker_exit");
    // Direct helper tests preserve the legacy protocol and verify an explicit
    // session's idle exit without giving the helper an image path.
    for(const bool sessionMode:{false,true}){
        const QString id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        SharedFrameMemory memory;
        QString memoryError;
        check(memory.create(id,0,frame.validBytes,&memoryError) && memory.write(frame.pixels,0,&memoryError),"direct_helper_shared_memory_created");
        const QJsonObject input{{"protocol","windows-ocr-once-v1"},{"request_id",id},
            {"mapping_name",memory.poolEntry()["mapping_name"]},{"width",frame.width},{"height",frame.height},
            {"bytes",frame.validBytes},{"language","en-US"}};
        const QJsonObject request=sessionMode ? QJsonObject{{"protocol","windows-ocr-session-v1"},{"session_id",id},
            {"frame_id","synthetic-idle-test"},{"request",input}} : input;
        QProcess direct;
        configureHelper(direct,helper,sessionMode?QStringList{"-Session","-IdleTimeoutMs","1000"}:QStringList{});
        direct.start();check(direct.waitForStarted(2000),"direct_hidden_helper_started");
        direct.write(json(request)+'\n');
        if(!sessionMode)direct.closeWriteChannel();
        QByteArray output;QElapsedTimer deadline;deadline.start();
        while(!output.contains('\n') && deadline.elapsed()<6000){direct.waitForReadyRead(50);output+=direct.readAllStandardOutput();}
        const auto returned=QJsonDocument::fromJson(output).object();
        const auto body=sessionMode?returned["result"].toObject():returned;
        check(validateOcrReply(json(body),id,frame.width,frame.height).ok,
              sessionMode?"explicit_session_direct_actual_ocr":"legacy_one_shot_protocol_actual_ocr_compatible");
        if(sessionMode)check(returned["frame_released"]==true,"session_reply_follows_reader_mapping_release");
        check(direct.waitForFinished(sessionMode?2500:1000) || direct.state()==QProcess::NotRunning,
              sessionMode?"idle_session_exits_without_another_request":"legacy_one_shot_exits_after_reply");
        if(direct.state()!=QProcess::NotRunning){direct.kill();direct.waitForFinished(1500);}
        check(direct.exitCode()==0 && direct.exitStatus()==QProcess::NormalExit,"direct_helper_normal_exit");
        memory.close();
    }
    // The user may switch apps during a background test. Attribute focus only
    // to the actual helper PID (sampled above), not any global HWND change.
    if (foregroundWindow() != foreground)
        std::cout << "EXTERNAL_FOREGROUND_CHANGE_OBSERVED=true; not_attributed_to_ocr=true\n";
    check(QDir(temp.path()).entryList({"*.png", "*.jpg", "*.bmp"}, QDir::Files).isEmpty(), "no_test_image_files");
    std::cout << "WINDOWS_OCR_TESTS=" << (failures ? "FAIL" : "PASS") << "; assertions=" << assertions
        << "; failures=" << failures << "; image_file_writes=0; real_ocr=true; synthetic_input=true\n";
    return failures ? 1 : 0;
}
