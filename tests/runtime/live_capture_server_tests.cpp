#include "diagnostics/live_capture_protocol.h"
#include "diagnostics/collection_label_refinement.h"
#include <QCoreApplication>
#include <QProcess>
#include <iostream>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace relink::diagnostics;
namespace {
QByteArray request(const QJsonValue& id, const QJsonArray& arguments) {
    return QJsonDocument(QJsonObject{{"id", id}, {"arguments", arguments}}).toJson(QJsonDocument::Compact);
}
QJsonArray base() { return {"--live-capture-check", "--focus-policy", "caller-owned"}; }
struct ProcessReply { bool completed = false; int status = -1; QByteArray output; QByteArray error; };
ProcessReply run(const QString& exe, const QByteArray& input,
    const QStringList& arguments = {QStringLiteral("--live-capture-server")}) {
    QProcess process;
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
        args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        args->startupInfo->wShowWindow = SW_HIDE;
    });
#endif
    process.start(exe, arguments);
    ProcessReply reply;
    if (!process.waitForStarted(3000)) return reply;
    if (process.write(input) != input.size()) return reply;
    process.closeWriteChannel();
    reply.completed = process.waitForFinished(10000) && process.exitStatus() == QProcess::NormalExit;
    if (!reply.completed) { process.kill(); process.waitForFinished(3000); }
    reply.status = process.exitCode();
    reply.output = process.readAllStandardOutput();
    reply.error = process.readAllStandardError();
    return reply;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int assertions = 0, failures = 0;
    const auto check = [&](bool success, const char* label) {
        ++assertions; failures += !success;
        std::cout << (success ? "PASS " : "FAIL ") << label << '\n';
    };
    const auto reject = [&](QByteArray data, const char* code, const char* label) {
        check(parseCaptureServiceRequest(data).error == QLatin1String(code), label);
    };
    const auto accepted = parseCaptureServiceRequest(request("first", base()));
    check(accepted.error.isEmpty() && accepted.id == "first" && accepted.arguments.size() == 4,
        "caller_owned_read_only_arguments_accepted_with_synthetic_argv0");
    auto args = base(); args.append("--frames=1"); args.append("--ocr");
    args.append("--collection-local-price"); args.append("--collection-price-image");
    check(parseCaptureServiceRequest(request(0, args)).error.isEmpty(), "safe_integer_id_and_local_price_option_accepted");
    args.append("--collection-local-text");args.append("--collection-title-image");args.append("--collection-catalog-image");
    check(parseCaptureServiceRequest(request(1, args)).error.isEmpty(), "fixed_local_text_provider_option_is_read_only");
    args.append("--reuse-capture-resources");
    check(parseCaptureServiceRequest(request(2, args)).error.isEmpty(), "gpu_resource_reuse_option_is_read_only");
    args.append("--collection-selection-preflight");
    check(parseCaptureServiceRequest(request(3,args)).error.isEmpty(),"selection_preflight_is_read_only");
    args.append("--collection-ready-stream");
    check(parseCaptureServiceRequest(request(4,args)).error.isEmpty(),"latest_frame_stream_is_explicit_read_only_protocol_option");
    args.append("--collection-scroll-readiness");
    check(parseCaptureServiceRequest(request(5,args)).error.isEmpty(),"scroll_readiness_is_explicit_read_only_protocol_option");
    args.append("--collection-selection-point");args.append("[1429,851]");
    check(parseCaptureServiceRequest(request(6,args)).error.isEmpty(),"dispatched_selection_point_is_read_only_hint");
    {
        const auto card=[](int x,bool selected){return QJsonObject{{"selected",selected},{"bounds_basis","observed"},
            {"bounds",QJsonArray{x,741,866,264}},{"fields_bounds",QJsonArray{x+1,962,864,42}},
            {"edges",QJsonObject{{"top",true},{"bottom",true},{"left",true},{"right",true}}}};};
        QJsonObject layout{{"complete",true},{"cards",QJsonArray{card(119,true),card(997,false)}}};
        auto gate=collectionSelectionTargetGate(layout,{1429,851});
        check(gate["ocr_deferred"]==true&&gate["selected_count"]==1&&gate["selected_target_match_count"]==0,
            "recorded_old_left_selection_does_not_finish_right_target_watch");
        layout["cards"]=QJsonArray{card(119,false),card(997,true)};
        gate=collectionSelectionTargetGate(layout,{1429,851});
        check(gate["ready_for_full_ocr"]==true&&gate["identity_proven"]==false&&gate["actions_enabled"]==false,
            "target_selected_permits_ocr_not_collection");
        check(collectionSelectionTargetGate(layout,{998,742})["ocr_deferred"]==true,"point_on_edge_is_not_target_body_proof");
        check(collectionSelectionTargetGate(layout,{})["ocr_deferred"]==true,"missing_target_is_not_ready_in_target_mode");
        check(!collectionSelectionPointValid({1.5,5})&&!collectionSelectionPointValid({true,5})
            &&!collectionSelectionPointValid({8192,1})&&!collectionSelectionPointValid({0,1})
            &&!collectionSelectionPointValid({1,2,3}),"selection_hint_rejects_noninteger_type_bounds_or_shape");
        layout["cards"]=QJsonArray{card(119,true),card(997,true)};
        check(collectionSelectionTargetGate(layout,{1429,851})["ocr_deferred"]==true,"hint_cannot_override_ambiguous_selection");
    }
    {
        // Live 2026-10-09 (purchase_readonly/live01): the persistent capture
        // service rejected the watchlist read with E_CAPTURE_SERVER_OPTION.
        auto purchaseArgs=base();purchaseArgs.append("--ocr");purchaseArgs.append("--purchase-observation");
        check(parseCaptureServiceRequest(request(8,purchaseArgs)).error.isEmpty(),"purchase_observation_is_a_read_only_protocol_option");
        auto twice=purchaseArgs;twice.append("--purchase-observation");
        check(parseCaptureServiceRequest(request(9,twice)).error==QStringLiteral("E_CAPTURE_SERVER_OPTION"),"purchase_observation_not_repeatable");
        auto watchArgs=purchaseArgs;watchArgs.append("--purchase-countdown-watch");watchArgs.append("3000");
        check(parseCaptureServiceRequest(request(10,watchArgs)).error.isEmpty(),"countdown_watch_is_a_read_only_protocol_option");
        auto dialogArea=watchArgs;dialogArea.append("--purchase-countdown-area");dialogArea.append("dialog");
        check(parseCaptureServiceRequest(request(13,dialogArea)).error.isEmpty(),"countdown_area_is_a_read_only_protocol_option");
        auto green=watchArgs;green.append("--purchase-countdown-stop-on-green");
        check(parseCaptureServiceRequest(request(14,green)).error.isEmpty(),"stop_on_green_is_a_read_only_protocol_option");
        auto stop=watchArgs;stop.append("--purchase-countdown-stop-on-button");
        check(parseCaptureServiceRequest(request(12,stop)).error.isEmpty(),"stop_on_button_is_a_read_only_protocol_option");
        auto bare=base();bare.append("--purchase-countdown-watch");
        check(parseCaptureServiceRequest(request(11,bare)).error==QStringLiteral("E_CAPTURE_SERVER_OPTION"),"countdown_watch_needs_a_duration");
    }
    {
        auto pixelArgs=base();pixelArgs.append("--collection-pixel-receipt");
        pixelArgs.append("--collection-receipt-reference");pixelArgs.append("{\"card_id\":\"a\"}");
        check(parseCaptureServiceRequest(request(7,pixelArgs)).error.isEmpty(),"pixel_receipt_is_explicit_read_only_protocol_option");
        const auto card=[](int x,bool selected){return QJsonObject{{"id",QStringLiteral("edge:%1").arg(x)},{"selected",selected},
            {"bounds_basis","observed"},{"bounds",QJsonArray{x,579,865,261}},{"fields_bounds",QJsonArray{x+1,798,863,42}},
            {"edges",QJsonObject{{"top",true},{"bottom",true},{"left",true},{"right",true}}}};};
        const QJsonObject reference{{"card_id","edge:996"},{"bounds",QJsonArray{996,579,865,261}}};
        QJsonObject layout{{"complete",true},{"frame_id","f"},{"cards",QJsonArray{card(119,false),card(996,true)}}};
        // 36x34 star ROI: 1224 pixels. Recorded gold: warm>=.05, bright<=.02.
        auto gate=collectionPixelReceiptGate(layout,reference,300,10,1224);
        check(gate["ready"]==true &&gate["acceptable"]==true &&gate["actions_enabled"]==false
            &&gate["ocr_performed"]==false,"gold_star_on_reference_selected_card_ends_pixel_watch_only");
        check(collectionPixelReceiptGate(layout,reference,0,160,1224)["acceptable"]==false,"white_star_is_not_a_receipt");
        check(collectionPixelReceiptGate(layout,reference,300,40,1224)["acceptable"]==false,"bright_white_mix_is_not_gold");
        layout["cards"]=QJsonArray{card(119,true),card(996,false)};
        check(collectionPixelReceiptGate(layout,reference,300,10,1224)["acceptable"]==false,
            "gold_star_with_other_card_selected_is_not_this_receipt");
        layout["cards"]=QJsonArray{card(119,true),card(996,true)};
        check(collectionPixelReceiptGate(layout,reference,300,10,1224)["acceptable"]==false,"ambiguous_selection_is_not_a_receipt");
        layout["cards"]=QJsonArray{card(119,false),card(1010,true)};
        check(collectionPixelReceiptGate(layout,reference,300,10,1224)["acceptable"]==false,"moved_selected_card_is_not_reference");
        QJsonObject partial{{"complete",false},{"selected_card_proof",QJsonObject{{"proven",true},{"card",card(996,true)}}}};
        gate=collectionPixelReceiptGate(partial,reference,300,10,1224);
        check(gate["acceptable"]==true &&gate["ready"]==false,"receipt_only_proof_is_acceptable_but_not_complete_layout");
        check(collectionPixelReceiptGate(layout,reference,300,10,0)["star_gold"]==false,"missing_star_pixels_fail_closed");
    }
    {
        const auto word=[](const char* text,double x,double y){return QJsonObject{{"text",text},{"x",x},{"y",y},{"width",40},{"height",20}};};
        const QJsonArray top{word("确认",900,400),word("关注数量已达上限",800,650)};
        const QJsonArray bottom{word("关注数量已达上限",801,652),word("取消",1100,800),word("关注",800,700)};
        const auto merged=mergeSplitRegionWords(top,bottom);
        check(merged.size()==4 &&merged[1].toObject()["x"]==800 &&merged[2].toObject()["text"]=="取消"
            &&merged[3].toObject()["text"]=="关注","split_halves_keep_overlap_word_once_and_all_others");
        check(mergeSplitRegionWords({},bottom).size()==3 &&mergeSplitRegionWords(top,{}).size()==2,"split_half_without_words_keeps_other_half");
    }
    {
        const auto miss=[](QJsonArray missing,bool blocker=false){
            QJsonObject semantic{{"matched",false},{"reason","E_LISTING_REGIONS_REQUIRED_ANCHOR"},{"missing_anchors",missing}};
            if(blocker)semantic["blocker"]=QJsonObject{{"label","确认"}};
            return semantic;};
        check(collectionListingStaticAnchorMiss(miss({"listing.default_sort"}))
            &&collectionListingStaticAnchorMiss(miss({"listing.sale","listing.default_sort"})),"recorded_header_or_sort_miss_reads_next_frame");
        check(!collectionListingStaticAnchorMiss(miss({"listing.similar"})) &&!collectionListingStaticAnchorMiss(miss({"listing.sale","listing.condition"}))
            &&!collectionListingStaticAnchorMiss(miss({"listing.sale"},true)) &&!collectionListingStaticAnchorMiss(miss({}))
            &&!collectionListingStaticAnchorMiss(QJsonObject{{"matched",true}})
            &&!collectionListingStaticAnchorMiss(QJsonObject{{"matched",false},{"reason","E_LISTING_REGIONS_BLOCKING_LABEL"}}),
            "detail_anchor_blocker_or_other_failures_keep_full_fallback");
    }
    args = {"--live-capture-check", "--focus-policy=caller-owned"};
    check(parseCaptureServiceRequest(request(9007199254740991.0, args)).error.isEmpty(), "safe_integer_max_and_equals_policy_accepted");
    reject("{", "E_CAPTURE_SERVER_JSON", "invalid_json_rejected");
    reject("[]", "E_CAPTURE_SERVER_JSON", "array_envelope_rejected");
    reject(QByteArray(CaptureRequestMaximumBytes + 1, 'x'), "E_CAPTURE_SERVER_LINE_LIMIT", "oversized_line_rejected_before_json_parse");
    for (const auto& id : {QJsonValue(), QJsonValue(true), QJsonValue(""), QJsonValue(1.5), QJsonValue(9007199254740992.0)})
        reject(request(id, base()), "E_CAPTURE_SERVER_ID", "uncorrelatable_id_rejected");
    reject(request(QString(129, 'x'), base()), "E_CAPTURE_SERVER_ID", "oversized_id_rejected");
    reject("{\"id\":\"x\",\"arguments\":[],\"command\":\"input\"}", "E_CAPTURE_SERVER_ENVELOPE", "unknown_envelope_keys_rejected");
    reject("{\"id\":\"x\",\"arguments\":\"shell\"}", "E_CAPTURE_SERVER_ENVELOPE", "arguments_must_be_array");
    reject(request("x", {}), "E_CAPTURE_SERVER_ARGUMENT_LIMIT", "empty_arguments_rejected");
    QJsonArray tooMany;
    for (int i = 0; i <= CaptureRequestMaximumArguments; ++i) tooMany.append("--ocr");
    reject(request("x", tooMany), "E_CAPTURE_SERVER_ARGUMENT_LIMIT", "argument_count_bounded");
    args = base(); args.append(true);
    reject(request("x", args), "E_CAPTURE_SERVER_ARGUMENT", "non_string_argument_rejected");
    args = base(); args.append(QString(4097, 'x'));
    reject(request("x", args), "E_CAPTURE_SERVER_ARGUMENT", "argument_bytes_bounded");
    args = base(); args.append(QStringLiteral("--ocr") + QChar(0));
    reject(request("x", args), "E_CAPTURE_SERVER_ARGUMENT", "embedded_nul_rejected");
    for (const auto& extra : {"--self-test", "--import-collection-tasks=a", "--live-capture-server", "--help", "--unknown", "input.exe", "--ocr=true"}) {
        args = base(); args.append(extra);
        reject(request("x", args), "E_CAPTURE_SERVER_OPTION", "non_capture_command_or_value_rejected");
    }
    args = base(); args.append("--focus-policy=caller-owned");
    reject(request("x", args), "E_CAPTURE_SERVER_OPTION", "duplicate_option_rejected");
    args = base(); args.append("--frames"); args.append("--ocr");
    reject(request("x", args), "E_CAPTURE_SERVER_OPTION", "option_cannot_fill_required_value");
    args = base(); args.append("--frames=");
    reject(request("x", args), "E_CAPTURE_SERVER_OPTION", "empty_value_rejected");
    reject(request("x", {"--live-capture-check"}), "E_CAPTURE_SERVER_FOCUS_POLICY", "implicit_standalone_forbidden");
    reject(request("x", {"--live-capture-check", "--focus-policy=standalone"}), "E_CAPTURE_SERVER_FOCUS_POLICY", "explicit_standalone_forbidden");
    reject(request("x", {"--focus-policy=caller-owned"}), "E_CAPTURE_SERVER_FOCUS_POLICY", "capture_opt_in_required");

    const QJsonObject before{{"request_count", 3}, {"helper_start_count", 1}, {"success_count", 3},
        {"engine_create_count", 2}, {"frame_mappings_released", 3}, {"helper_ui_checks", 20}};
    const QJsonObject after{{"scope", "current_diagnostic_process"}, {"request_count", 5},
        {"helper_start_count", 1}, {"success_count", 5}, {"engine_create_count", 2},
        {"frame_mappings_released", 5}, {"helper_ui_checks", 26},
        {"calls", QJsonArray{QJsonObject{{"request_id", "old"}}, QJsonObject{{"request_id", "new1"}}, QJsonObject{{"request_id", "new2"}}}}};
    const auto metrics = captureRequestOcrMetrics(before, after);
    check(metrics["scope"] == "capture_request" && metrics["request_count"] == 2
        && metrics["helper_start_count"] == 0 && metrics["engine_create_count"] == 0
        && metrics["success_count"] == 2 && metrics["frame_mappings_released"] == 2
        && metrics["helper_ui_checks"] == 6, "request_metrics_are_additive_not_repeated_cumulative_totals");
    check(metrics["calls"].toArray().size() == 2 && metrics["calls"].toArray()[0].toObject()["request_id"] == "new1"
        && metrics["calls_truncated"] == false, "prior_call_timing_history_removed_from_new_capture");
    check(metrics["cumulative"].toObject()["request_count"] == 5 && !metrics["cumulative"].toObject().contains("calls"),
        "lifetime_counts_preserved_without_duplicate_timing_array");
    check(captureRequestOcrMetrics(after, after)["calls"].toArray().isEmpty(), "zero_new_ocr_requests_emit_no_old_calls");
    check(captureRequestOcrMetrics({}, after)["calls_truncated"] == true, "bounded_call_history_truncation_explicit");

    const bool parserOnly = app.arguments().size() == 2 && app.arguments()[1] == "--parser-only";
    const QJsonObject readyCard{{"selected",true},{"bounds_basis","observed"},
        {"fields_bounds",QJsonArray{100,500,860,42}},
        {"edges",QJsonObject{{"top",true},{"bottom",true},{"left",true},{"right",true}}}};
    for(int mode=0;mode<5;++mode){
        auto card=readyCard;
        if(mode==1)card["selected"]=false;
        if(mode==2)card["fields_bounds"]=QJsonValue();
        QJsonArray cards{card};if(mode==3)cards.append(card);
        const auto gate=collectionSelectionGate({{"complete",mode!=4},{"cards",cards},
            {"frame_id","current-frame"},{"frame_sha256",QString(64,'a')}});
        check(gate["ready_for_full_ocr"].toBool()==(mode==0)
            &&gate["ocr_deferred"].toBool()==(mode!=0),"preflight_defers_ocr_for_no_selection_partial_ambiguous_incomplete");
        check(gate["actions_enabled"]==false &&gate["page_classified"]==false
            &&gate["identity_proven"]==false &&gate["requires_original_full_validation"]==true,
            "even_ready_preflight_cannot_authorize_input_or_replace_page_identity_checks");
    }
    const QRect labelBox(1130,238,135,48);
    const QJsonArray nonempty{QJsonObject{{"text","unrecognized-label"},{"x",1153},{"y",253},{"width",80},{"height",20}}};
    for (int nonemptyAt = 0; nonemptyAt <= 3; ++nonemptyAt) {
        int calls = 0;
        const auto label = readCollectionLabel("fresh-frame",labelBox,[&](int scale,bool invert) {
            ++calls;
            check(scale == (calls==1?2:calls==2?1:3) &&invert == (calls!=2),"label_preprocessing_sequence_is_fixed");
            relink::vision::OcrReply reply;reply.ok=true;
            reply.observation={{"frame_id","fresh-frame"},{"coverage","roi"},
                {"words", calls==nonemptyAt ? nonempty:QJsonArray{}}};
            return reply;
        });
        check(calls == (nonemptyAt?nonemptyAt:3) && label.attempts.size()==calls,
            "empty_label_retry_bounded_and_all_attempts_preserved");
        check(label.reply.ok &&label.reply.observation["words"].toArray()==(nonemptyAt?nonempty:QJsonArray{}),
            "first_nonempty_even_wrong_label_is_not_retried_for_expected_text");
    }
    for (int errorCase = 0; errorCase < 4; ++errorCase) {
        int calls=0;
        const auto label=readCollectionLabel("fresh-frame",labelBox,[&](int,bool){
            ++calls;relink::vision::OcrReply reply;
            reply.ok=errorCase!=0;reply.error=errorCase==0?"E_PROVIDER":"";
            reply.observation={{"frame_id",errorCase==1?"old-frame":"fresh-frame"},
                {"coverage",errorCase==2?"full_client":"roi"},{"words",nonempty}};
            if(errorCase==3)reply.observation.remove("words");
            return reply;
        });
        check(calls==1 &&!label.reply.ok &&!label.reply.error.isEmpty(),
            "provider_error_stale_frame_wrong_coverage_malformed_words_stop_without_retry");
    }
    if (app.arguments().size() == 2 && !parserOnly) {
        const auto exe = app.arguments()[1];
        auto reply = run(exe, {});
        check(reply.completed && reply.status == 0 && reply.output.isEmpty(), "empty_eof_exits_without_ui_or_unsolicited_handshake");
        const QByteArray batch = "{\n" + request("a", base()) + "\n" + request("b", base()) + "\r\n";
        reply = run(exe, batch);
        const auto lines = reply.output.trimmed().split('\n');
        check(reply.completed && reply.status == 0 && lines.size() == 3, "bad_request_does_not_terminate_bounded_sequential_service");
        if (lines.size() == 3) {
            const auto malformed = QJsonDocument::fromJson(lines[0]).object();
            const auto first = QJsonDocument::fromJson(lines[1]).object();
            const auto second = QJsonDocument::fromJson(lines[2]).object();
            check(malformed["id"].isNull() && malformed["exit_status"] == 2
                && malformed["result"].toObject()["error"] == "E_CAPTURE_SERVER_JSON", "malformed_response_is_structured_json_only");
            check(first["id"] == "a" && second["id"] == "b" && first["exit_status"] == 2 && second["exit_status"] == 2,
                "request_ids_and_original_exit_status_preserved");
            const auto r1 = first["result"].toObject(); const auto r2 = second["result"].toObject();
            const auto s1 = r1["capture_server"].toObject(); const auto s2 = r2["capture_server"].toObject();
            check(r1["error"] == "E_DIAGNOSTIC_ARGUMENTS" && r2["error"] == "E_DIAGNOSTIC_ARGUMENTS"
                && r1["game_input_sent"] == false && r2["game_input_sent"] == false
                && r1["image_file_writes"] == 0 && !r1["recognition_performed"].toBool(), "invalid_target_arguments_stop_before_capture_or_helper");
            check(s1["pid"] == s2["pid"] && s1["request_sequence"] == 1 && s2["request_sequence"] == 2
                && s1["ocr_owner_reused"] == false && s2["ocr_owner_reused"] == true,
                "one_process_owns_two_request_lifetimes");
        }
        // Hotkey runs (F2 inside the game) send no return window: a caller-owned
        // probe never restores one. A given return window still must differ.
        const QJsonArray noReturn{"--live-capture-check", "--focus-policy", "caller-owned",
            "--target-hwnd", "1", "--target-pid", "1", "--frames", "1"};
        reply = run(exe, request("no-return", noReturn) + "\n");
        const auto unbound = QJsonDocument::fromJson(reply.output).object()["result"].toObject();
        check(reply.completed && !unbound["error"].toString().isEmpty() && unbound["error"] != "E_DIAGNOSTIC_ARGUMENTS"
            && unbound["game_input_sent"] == false && unbound["image_file_writes"] == 0,
            "caller_owned_probe_needs_no_return_window");
        // Countdown watch: bounded duration, watchlist page, latest-frame
        // stream and full purchase OCR of the final frame are all required.
        const auto requestError=[&](const QJsonArray& arguments) {
            const auto r = run(exe, request("watch", arguments) + "\n");
            return QJsonDocument::fromJson(r.output).object()["result"].toObject()["error"].toString();
        };
        QJsonArray watch = noReturn;
        watch << "--ocr" << "--purchase-observation" << "--reuse-capture-resources" << "--collection-ready-stream";
        const auto withWatch=[](QJsonArray arguments, const char* page, const char* ms) {
            arguments << "--expected-page" << page << "--purchase-countdown-watch" << ms;
            return arguments;
        };
        QJsonArray noStream = noReturn;
        noStream << "--ocr" << "--purchase-observation";
        QJsonArray noPurchase = noReturn;
        noPurchase << "--ocr" << "--reuse-capture-resources" << "--collection-ready-stream";
        for (const auto& arguments : {withWatch(watch, "watchlist_listings", "400"), withWatch(watch, "watchlist_listings", "10001"),
                withWatch(watch, "watchlist_listings", "3s"), withWatch(watch, "skin_listings", "3000"),
                withWatch(noStream, "watchlist_listings", "3000"), withWatch(noPurchase, "watchlist_listings", "3000")})
            check(requestError(arguments) == "E_PURCHASE_COUNTDOWN_WATCH_ARGUMENTS", "countdown_watch_bounds_page_stream_and_ocr_enforced");
        const auto bound = requestError(withWatch(watch, "watchlist_listings", "3000"));
        check(!bound.isEmpty() && bound != "E_PURCHASE_COUNTDOWN_WATCH_ARGUMENTS" && bound != "E_DIAGNOSTIC_ARGUMENTS",
            "valid_countdown_watch_reaches_window_binding");
        QJsonArray badArea = watch;
        badArea << "--purchase-countdown-watch" << "3000" << "--purchase-countdown-area" << "anywhere";
        check(requestError(badArea) == "E_PURCHASE_COUNTDOWN_WATCH_ARGUMENTS", "countdown_area_is_footer_or_dialog");
        QJsonArray dialogWatch = watch;
        dialogWatch << "--purchase-countdown-watch" << "3000" << "--purchase-countdown-area" << "dialog";
        const auto dialogError = requestError(dialogWatch);
        check(!dialogError.isEmpty() && dialogError != "E_PURCHASE_COUNTDOWN_WATCH_ARGUMENTS" && dialogError != "E_DIAGNOSTIC_ARGUMENTS",
            "dialog_countdown_watch_reaches_window_binding");
        QJsonArray lean = noReturn;
        lean << "--reuse-capture-resources" << "--collection-ready-stream" << "--purchase-countdown-watch" << "3000"
             << "--purchase-countdown-area" << "dialog" << "--purchase-countdown-lean";
        const auto leanError = requestError(lean);
        check(!leanError.isEmpty() && leanError != "E_PURCHASE_COUNTDOWN_WATCH_ARGUMENTS" && leanError != "E_DIAGNOSTIC_ARGUMENTS",
            "lean_countdown_watch_needs_no_ocr");
        QJsonArray leanWithOcr = lean;
        leanWithOcr << "--ocr";
        check(requestError(leanWithOcr) == "E_PURCHASE_COUNTDOWN_WATCH_ARGUMENTS", "lean_countdown_watch_has_no_full_ocr");
        QJsonArray stopAlone = noReturn;
        stopAlone << "--ocr" << "--purchase-observation" << "--purchase-countdown-stop-on-button";
        check(requestError(stopAlone) == "E_PURCHASE_COUNTDOWN_WATCH_ARGUMENTS", "stop_on_button_needs_a_countdown_watch");
        QJsonArray pageless = watch;
        pageless << "--purchase-countdown-watch" << "3000";
        const auto pagelessError = requestError(pageless);
        check(!pagelessError.isEmpty() && pagelessError != "E_PURCHASE_COUNTDOWN_WATCH_ARGUMENTS" && pagelessError != "E_DIAGNOSTIC_ARGUMENTS",
            "countdown_watch_page_is_checked_by_the_caller");
        QJsonArray sameReturn = noReturn;
        sameReturn << "--return-hwnd" << "1" << "--return-pid" << "1";
        reply = run(exe, request("same-return", sameReturn) + "\n");
        check(QJsonDocument::fromJson(reply.output).object()["result"].toObject()["error"] == "E_DIAGNOSTIC_ARGUMENTS",
            "a_given_return_window_must_still_differ_from_the_game");
        reply = run(exe, request("partial", base()));
        check(reply.completed && reply.status == 0 && QJsonDocument::fromJson(reply.output).object()["id"] == "partial",
            "bounded_final_line_processed_at_eof");
        reply = run(exe, QByteArray(CaptureRequestMaximumBytes + 1, 'x') + '\n');
        check(reply.completed && reply.status == 2 && QJsonDocument::fromJson(reply.output).object()["result"].toObject()["error"] == "E_CAPTURE_SERVER_LINE_LIMIT",
            "oversized_stream_closes_without_unbounded_drain");
        reply = run(exe, {}, {"--live-capture-server", "--ocr"});
        check(reply.completed && reply.status == 2 && QJsonDocument::fromJson(reply.output).object()["error"] == "E_CAPTURE_SERVER_STARTUP_ARGUMENTS",
            "server_startup_accepts_no_capture_or_ui_arguments");
        reply = run(exe, {}, {"--live-capture-server", "--live-capture-check"});
        check(reply.completed && reply.status == 2 && reply.error.contains("E_DIAGNOSTIC_MODE_CONFLICT"), "diagnostic_modes_are_mutually_exclusive");
    } else if (!parserOnly) check(false, "test_requires_application_path");
    std::cout << "LIVE_CAPTURE_SERVER_TESTS=" << (failures ? "FAIL" : "PASS")
        << "; assertions=" << assertions << "; failures=" << failures << "; live_captures=0; input_dispatches=0\n";
    return failures ? 1 : 0;
}
