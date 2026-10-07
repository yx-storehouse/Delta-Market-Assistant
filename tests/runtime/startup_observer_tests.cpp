#include "application/runtime/startup_observer.h"
#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <functional>
#include <iostream>
#include <limits>

using namespace relink::runtime;
using namespace relink::vision;
namespace {
int assertions = 0, failures = 0;
QHash<QString, QJsonObject> fixtures;
void check(bool passed, const QString& label) {
    ++assertions; if (!passed) ++failures;
    std::cout << (passed ? "PASS " : "FAIL ") << label.toStdString() << '\n';
}
RuntimeContext context() { return {"test-run", "test-session", "fixture-clock", "startup", 0, 1}; }
StartupPageObservation observation(const QString& key, int index) {
    return {QStringLiteral("frame-%1").arg(index), context(), index * 100 - 1, index * 100, fixtures.value(key)};
}
bool feed(StartupObserver& observer, const QString& key, int index) {
    return observer.observe(observation(key, index), index * 100);
}
QJsonObject scaled(QJsonObject page, double scale) {
    page["width"] = page.value("width").toDouble() * scale;
    page["height"] = page.value("height").toDouble() * scale;
    auto words = page.value("words").toArray();
    for (qsizetype i = 0; i < words.size(); ++i) {
        auto word = words[i].toObject();
        for (const auto& key : {"x", "y", "width", "height"}) word[key] = word.value(key).toDouble() * scale;
        words[i] = word;
    }
    page["words"] = words; return page;
}
QJsonObject token(const QString& text, double x, double y, double width, double height = 20) {
    return {{"text", text}, {"x", x}, {"y", y}, {"width", width}, {"height", height}};
}
void readOnly(const StartupObserver& observer) {
    const auto json = observer.snapshot().toJson();
    check(!json.value("actions_enabled").toBool() && !json.value("purchase_authorized").toBool()
        && !json.value("capture_requested").toBool(), "no_action_or_capture_side_effect");
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QFile file(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString());
    check(file.open(QIODevice::ReadOnly), "fixture_file_open");
    const auto root = QJsonDocument::fromJson(file.readAll()).object();
    const auto cases = root.value("cases").toArray();
    check(cases.size() == 27 && !root.value("pixels_included").toBool() && !root.value("live_game_test").toBool(), "historical_projection_not_live_accuracy_test");
    for (const auto& value : cases) {
        const auto item = value.toObject();
        const QString id = item.value("id").toString();
        const auto page = item.value("observation").toObject();
        fixtures.insert(id, page);
        const auto result = classifySkinPage(page);
        check(result.validInput && toString(result.page) == item.value("expected_page").toString()
            && toString(result.overlay) == item.value("expected_modal").toString(), "original_projection_" + id + ':' + result.reason + ':' + toString(result.page));
        for (double factor : {.5, 4.0/3.0, 1.5, 2.0}) {
            const auto transformed = classifySkinPage(scaled(page, factor));
            check(transformed.validInput && transformed.page == result.page && transformed.overlay == result.overlay, "scale_invariant_" + id);
        }
        auto noScores = page;
        auto words = noScores.value("words").toArray();
        for (qsizetype i = 0; i < words.size(); ++i) { auto w = words[i].toObject(); w.remove("score"); words[i] = w; }
        noScores["words"] = words;
        const auto scoreless = classifySkinPage(noScores);
        check(scoreless.validInput && scoreless.page == result.page && !scoreless.providerScoresAvailable, "scoreless_provider_does_not_fabricate_confidence_" + id);
    }
    auto home = fixtures["p1_skin_home"];
    auto empty = fixtures["p1_empty_watchlist_1"];
    QJsonObject nav{{"width", 1920}, {"height", 1080}, {"coverage", "full_client"},
        {"words", QJsonArray{token(QStringLiteral("交易行"), 700, 30, 80), token(QStringLiteral("典藏外观"), 300, 40, 100)}}};
    nav["legacy_page_label"] = QStringLiteral("皮肤列表");
    check(classifySkinPage(nav).page == SkinPage::Unknown, "navigation_and_legacy_label_never_supply_page_proof");
    check(classifySkinPage(fixtures["p4_settings_2880"]).page == SkinPage::GameSettings, "legacy_warfare_label_is_actually_settings");
    check(classifySkinPage(fixtures["p6_settings_mislabeled"]).page == SkinPage::GameSettings, "legacy_apply_appearance_label_is_actually_settings");
    auto low = home; auto words = low["words"].toArray();
    for (qsizetype i = 0; i < words.size(); ++i) { auto w = words[i].toObject(); w["score"] = .69; words[i] = w; }
    low["words"] = words;
    auto result = classifySkinPage(low);
    check(result.validInput && result.page == SkinPage::Unknown && result.lowScoreTokens == words.size(), "low_score_labels_do_not_prove_page");
    auto conflicting = home;
    auto combined = home["words"].toArray();
    for (const auto& word : fixtures["p6_settings_mislabeled"]["words"].toArray()) combined.append(word);
    conflicting["words"] = combined;
    check(classifySkinPage(conflicting).reason == "E_PAGE_AMBIGUOUS", "conflicting_pages_fail_closed");
    auto fragment = empty;
    words = empty["words"].toArray();
    for (qsizetype i = 0; i < words.size(); ++i) {
        const auto w = words[i].toObject();
        if (w["text"].toString().contains(QStringLiteral("暂未添加"))) {
            auto first = w, second = w;
            first["text"] = QStringLiteral("暂未添加任何"); second["text"] = QStringLiteral("关注的皮肤");
            first["width"] = w["width"].toDouble()/2 - 1;
            second["x"] = w["x"].toDouble() + w["width"].toDouble()/2 + 1;
            second["width"] = w["width"].toDouble()/2 - 1;
            words[i] = first; words.append(second); break;
        }
    }
    fragment["words"] = words;
    check(classifySkinPage(fragment).page == SkinPage::EmptyWatchlist, "adjacent_same_line_split_chinese_label");
    auto far = words.last().toObject(); far["x"] = far["x"].toDouble() + 150; words[words.size()-1] = far;
    fragment["words"] = words;
    check(classifySkinPage(fragment).page == SkinPage::Unknown, "far_apart_text_not_concatenated_into_page_anchor");
    auto laterPage = fixtures["p3_list_filter_open"];
    words = laterPage["words"].toArray();
    for (qsizetype i = 0; i < words.size(); ++i) {
        auto word = words[i].toObject(); if (word["text"] == QStringLiteral("第1页")) { word["text"] = QStringLiteral("第10页"); words[i] = word; }
    }
    laterPage["words"] = words;
    check(classifySkinPage(laterPage).page == SkinPage::SkinListings, "page_number_not_hardcoded_to_first_two_pages");
    auto covered=fixtures["p3_list_filter_open"];QJsonArray visible;
    for(const auto& v:covered["words"].toArray()){
        const auto t=v.toObject()["text"].toString();
        if(!t.contains(QStringLiteral("第")) && !t.contains(QStringLiteral("相似皮肤")))visible.append(v);
    }
    covered["words"]=visible;
    check(classifySkinPage(covered).page==SkinPage::SkinListings && classifySkinPage(covered).overlay==PageOverlay::ListingFilter,
        "filter_panel_proves_covered_layout_with_sale_sort_condition_context");
    for(const auto& required:QStringList{QStringLiteral("在售"),QStringLiteral("默认排序"),QStringLiteral("价格区间"),QStringLiteral("所有成色"),QStringLiteral("确定")}){
        auto missing=covered;QJsonArray list;
        for(const auto& v:visible)if(v.toObject()["text"].toString()!=required)list.append(v);
        missing["words"]=list;check(classifySkinPage(missing).page==SkinPage::Unknown,"filter_panel_missing_independent_anchor_rejected_"+required);
    }
    auto toastPage=home;QJsonArray toastWords=home["words"].toArray();
    const QString receipt=QStringLiteral("成功添加至我的关注");
    const double tw=home["width"].toDouble(),th=home["height"].toDouble();
    for(int i=0;i<receipt.size();++i)toastWords.append(token(receipt.mid(i,1),tw*.42+i*18,th*.15,16,20));
    toastPage["words"]=toastWords;
    check(classifySkinPage(toastPage).anchorChecks["collection.added"].toBool(),"nine_split_receipt_tokens_in_toast");
    auto gapWords=toastWords;auto gap=gapWords.last().toObject();gap["x"]=gap["x"].toDouble()+80;gapWords[gapWords.size()-1]=gap;
    toastPage["words"]=gapWords;
    check(!classifySkinPage(toastPage).anchorChecks["collection.added"].toBool(),"receipt_gap_rejected");
    toastWords=home["words"].toArray();toastWords.append(token(receipt,tw*.42,th*.60,180,20));toastPage["words"]=toastWords;
    check(!classifySkinPage(toastPage).anchorChecks["collection.added"].toBool(),"receipt_outside_toast_rejected");
    const std::function<void(QJsonObject&)> malformed[] = {
        [](auto& p){p["width"]=0;}, [](auto& p){p["width"]=8193;}, [](auto& p){p["width"]=1920.5;},
        [](auto& p){p["height"]="1080";}, [](auto& p){p.remove("coverage");}, [](auto& p){p["coverage"]="roi";},
        [](auto& p){p["words"]=QJsonObject{};}, [](auto& p){p["words"]=QJsonArray{QStringLiteral("not-a-token")};}
    };
    for (const auto& mutate : malformed) { auto bad = home; mutate(bad); check(!classifySkinPage(bad).validInput, "invalid_observation_rejected"); }
    for (const auto& field : {"x", "y", "width", "height", "score", "text"}) {
        for (const auto& invalid : {QJsonValue(QJsonValue::Null), QJsonValue(-1), QJsonValue(QJsonObject{})}) {
            auto bad = home; auto list = bad["words"].toArray(); auto word = list[0].toObject(); word[field] = invalid; list[0] = word; bad["words"] = list;
            check(!classifySkinPage(bad).validInput, "invalid_word_field_rejected");
        }
    }
    auto many = home; words = {};
    for (int i=0;i<2001;++i) words.append(token("x",1,1,10));
    many["words"]=words; check(!classifySkinPage(many).validInput,"token_budget_enforced");
    many=home;words={};for(int i=0;i<70;++i)words.append(token(QString(1000,QLatin1Char('x')),1,1,10));
    many["words"]=words;check(!classifySkinPage(many).validInput,"text_byte_budget_enforced");

    StartupObserver route;
    check(route.start(context(), 0), "start_original_route");
    check(feed(route, "p1_skin_home", 1) && route.snapshot().checkpoint == "watchlist_precheck", "home_requires_original_watchlist_precheck");
    check(feed(route, "p1_empty_watchlist_1", 2) && !route.snapshot().emptyWatchlistVerified, "first_empty_observation_not_enough");
    check(!route.observe(observation("p1_empty_watchlist_1",2),201) && !route.snapshot().emptyWatchlistVerified, "same_frame_cannot_confirm_empty_twice");
    auto cached = observation("p1_empty_watchlist_1",2);cached.frameId="different-id-same-presentation";
    check(!route.observe(cached,202) && !route.snapshot().emptyWatchlistVerified, "renamed_cached_presentation_rejected");
    check(feed(route,"p1_empty_watchlist_2",3) && route.snapshot().phase==StartupPhase::AwaitHomeAfterEmpty,"second_fresh_empty_confirms");
    check(feed(route,"p1_catalog_filter",4) && route.snapshot().phase!=StartupPhase::FilterObserved,"cannot_skip_return_home_after_empty");
    check(feed(route,"p1_home_after_empty",5) && route.snapshot().phase==StartupPhase::AwaitCatalogFilter,"original_return_home_before_filter");
    check(feed(route,"p1_catalog_filter",6) && route.snapshot().phase==StartupPhase::FilterObserved,"original_precheck_route_complete");
    readOnly(route);
    const int accepted = route.snapshot().acceptedCount;
    check(!feed(route,"p1_skin_list",7) && route.snapshot().acceptedCount==accepted,"completed_observation_requires_new_context");
    check(!route.start(context(),800),"same_context_cannot_restart_after_completion");
    auto freshContext=context();++freshContext.cancelEpoch;
    check(route.start(freshContext,800),"new_epoch_allows_new_observation");
    check(!feed(route,"p1_skin_list",9),"old_epoch_cannot_drive_restarted_observer");
    route.stop();readOnly(route);

    for (const auto& key : {"p1_skin_list", "p1_watch_list"}) {
        StartupObserver existing;existing.start(context(),0);
        check(feed(existing,key,1) && existing.snapshot().phase==StartupPhase::ExistingListObserved,"existing_list_does_not_restart_collection");
        check(existing.snapshot().sourceSteps==QStringList{"S32","S33"},"existing_list_preserves_original_steps");readOnly(existing);
    }
    StartupObserver noPrecheck;noPrecheck.start(context(),0);
    check(feed(noPrecheck,"p1_catalog_filter",1) && noPrecheck.snapshot().reason=="E_STARTUP_PRECHECK_REQUIRED","filter_alone_does_not_prove_startup_route");
    StartupObserver overlay;overlay.start(context(),0);
    check(feed(overlay,"p3_list_filter_open",1) && overlay.snapshot().checkpoint=="recheck_after_overlay" && overlay.active(),"open_overlay_blocks_list_readiness");
    check(feed(overlay,"p1_skin_list",2) && overlay.snapshot().phase==StartupPhase::ExistingListObserved,"fresh_unobscured_list_resumes_original_branch");
    StartupObserver invalid;invalid.start(context(),0);
    const std::function<void(StartupPageObservation&)> contextChanges[] = {
        [](auto& o){o.context.runId+="x";},[](auto& o){o.context.sessionId+="x";},[](auto& o){o.context.clockDomainId+="x";},
        [](auto& o){o.context.stepId+="x";},[](auto& o){++o.context.cancelEpoch;},[](auto& o){++o.context.viewportGeneration;}
    };
    for(const auto& change:contextChanges){auto o=observation("p1_skin_list",1);change(o);check(!invalid.observe(o,100)&&invalid.snapshot().phase==StartupPhase::Locating,"all_context_fields_correlated");}
    auto stale=observation("p1_skin_list",1);stale.captureLowerBoundMonoMs=-1;
    check(!invalid.observe(stale,100),"negative_capture_stamp_rejected");
    stale=observation("p1_skin_list",1);stale.captureEndMonoMs=101;
    check(!invalid.observe(stale,100),"future_capture_rejected");
    stale=observation("p1_skin_list",1);
    check(!invalid.observe(stale,1200),"stale_observation_rejected");
    check(!invalid.tick(1199)&&invalid.snapshot().phase==StartupPhase::Paused,"clock_regression_pauses");readOnly(invalid);
    StartupObserver timeout;timeout.start(context(),0,300);
    check(feed(timeout,"p1_empty_watchlist_1",1),"timeout_fixture_first_empty");
    check(!timeout.tick(300)&&timeout.snapshot().phase==StartupPhase::Paused&&!timeout.snapshot().emptyWatchlistVerified,"deadline_at_equality_expires_without_capture");
    StartupObserver budget;budget.start(context(),0,10000,2);
    check(feed(budget,"p1_transition",1)&&feed(budget,"p1_transition",2)&&budget.snapshot().phase==StartupPhase::Paused,"unknown_page_observation_budget_bounded");
    check(budget.retainedFrameIds()<=2,"frame_history_bounded");
    StartupObserver interrupted;interrupted.start(context(),0);
    feed(interrupted,"p1_empty_watchlist_1",1);interrupted.pause();
    check(!feed(interrupted,"p1_empty_watchlist_2",2)&&!interrupted.snapshot().emptyWatchlistVerified,"pause_invalidates_pending_precheck");
    interrupted.stop();check(!feed(interrupted,"p1_skin_list",3),"stopped_observer_ignores_late_page");readOnly(interrupted);
    for (int value : {0,129}) {StartupObserver x;check(!x.start(context(),0,10000,value),"invalid_frame_budget");}
    for (qint64 value : {qint64(0),qint64(60001)}) {StartupObserver x;check(!x.start(context(),0,value),"invalid_timeout");}
    StartupObserver bounds;
    check(!bounds.start(context(),9007199254740991LL),"deadline_integer_overflow_rejected");
    check(!bounds.start(context(),0,10000,1,2),"confirmation_count_cannot_exceed_budget");
    auto badContext=context();badContext.stepId.clear();check(!bounds.start(badContext,0),"empty_context_id_rejected");
    bounds.start(context(),0);check(!bounds.start(freshContext,0)&&bounds.active(),"active_start_does_not_replace_context");
    std::cout<<"STARTUP_OBSERVER_TESTS="<<(failures?"FAIL":"PASS")<<"; assertions="<<assertions<<"; failures="<<failures
        <<"; historical_cases="<<cases.size()<<"; game_connected=false; system_input_sent=false; image_file_writes=0\n";
    return failures?1:0;
}
