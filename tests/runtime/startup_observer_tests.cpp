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
    QJsonObject missingHome{{"width",2560},{"height",1440},{"coverage","full_client"},
        {"words",QJsonArray{token(QStringLiteral("典藏外观"),390,91,100),
                           token(QStringLiteral("我的关注"),2288,264,100)}}};
    check(classifySkinPage(missingHome).page==SkinPage::Unknown
        &&homeCirculationRefinementRegion(missingHome)==QRect(625,310,800,95),
        "repaired_navigation_requests_real_circulation_without_assuming_home");
    auto missingNav=missingHome;auto repairedWords=missingHome["words"].toArray();
    auto onlyOne=repairedWords;onlyOne.removeLast();missingNav["words"]=onlyOne;
    check(homeCirculationRefinementRegion(missingNav).isEmpty(),"one_navigation_anchor_is_not_enough_to_request_home_refinement");
    repairedWords.append(token(QStringLiteral("当前市场流通量：471个"),625,320,300));
    auto repairedHome=missingHome;repairedHome["words"]=repairedWords;
    check(classifySkinPage(repairedHome).page==SkinPage::SkinHome
        &&homeCirculationRefinementRegion(repairedHome).isEmpty(),
        "actual_circulation_word_supplies_original_independent_home_anchor");
    check(homeCirculationRefinementRegion(scaled(missingHome,.75)).isEmpty(),"home_refinement_does_not_guess_unmeasured_resolution");
    auto conflictingHome=missingHome;auto conflictingWords=missingHome["words"].toArray();
    conflictingWords.append(token(QStringLiteral("分辨率"),500,500,100));
    conflictingWords.append(token(QStringLiteral("显示模式"),500,550,100));
    conflictingWords.append(token(QStringLiteral("局内帧数上限"),500,600,160));
    conflictingHome["words"]=conflictingWords;
    check(homeCirculationRefinementRegion(conflictingHome).isEmpty(),"known_other_page_never_gets_home_refinement");
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
    {
        // Live 2026-10-09: the watchlist count may be read without its colon,
        // split from the number, or as separate characters.
        const auto watch = fixtures["p1_watch_list"];
        bool allWatch = true;
        for (const auto& variant : QList<QStringList>{{QStringLiteral("我的关注60/150")}, {QStringLiteral("我的关注"), QStringLiteral("：60/150")},
                                                     {QStringLiteral("我的"), QStringLiteral("关注"), QStringLiteral("60/150")}}) {
            auto page = watch;
            auto words = page["words"].toArray();
            for (qsizetype i = 0; i < words.size(); ++i) {
                const auto w = words[i].toObject();
                if (!w["text"].toString().startsWith(QStringLiteral("我的关注"))) continue;
                words.removeAt(i);
                double x = w["x"].toDouble();
                for (const auto& part : variant) {
                    const double width = 20.0 * part.size();
                    words.append(token(part, x, w["y"].toDouble(), width, w["height"].toDouble()));
                    x += width + 2;
                }
                break;
            }
            page["words"] = words;
            allWatch &= classifySkinPage(page).page == SkinPage::WatchlistListings;
        }
        check(allWatch, "watchlist_count_without_colon_or_split_still_watchlist");
        auto toast = fixtures["p1_skin_list"];
        auto words = toast["words"].toArray();
        words.append(token(QStringLiteral("成功添加至我的关注"), 800, 160, 320, 28));
        toast["words"] = words;
        check(classifySkinPage(toast).page == SkinPage::SkinListings, "collection_toast_does_not_make_a_listing_a_watchlist");
    }
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
    // A synthetic full-client observation reproduces run_06's exact missing
    // anchor pattern; label boxes below are the recorded same-layout label
    // positions. These tests do not claim an OCR re-read of unsaved live pixels.
    auto filterMissing=scaled(fixtures["p3_list_filter_open"],4.0/3.0);
    filterMissing["frame_id"]="fixture:filter-refinement";
    filterMissing["frame_sha256"]=QString(64,QLatin1Char('a'));
    QJsonArray missingLabelWords;
    for(const auto& value:filterMissing["words"].toArray())
        if(value.toObject()["text"]!=QStringLiteral("所有成色"))missingLabelWords.append(value);
    filterMissing["words"]=missingLabelWords;
    const QRect labelRoi(1886,444,120,42);
    const auto beforeRefinement=classifySkinPage(filterMissing);
    check(beforeRefinement.page==SkinPage::SkinListings &&beforeRefinement.overlay==PageOverlay::None
        &&beforeRefinement.anchorChecks["listing.filter_price"].toBool()
        &&beforeRefinement.anchorChecks["listing.filter_confirm"].toBool()
        &&!beforeRefinement.anchorChecks["listing.filter_all"].toBool(),"missing_all_label_not_inferred_from_other_two_modal_anchors");
    check(listingFilterAllLabelRefinementRegion(filterMissing)==labelRoi,"listing_filter_label_roi_excludes_checked_box");
    QJsonObject labelRead{{"width",2560},{"height",1440},{"coverage","roi"},{"same_frame",true},
        {"frame_id",filterMissing["frame_id"]},{"frame_sha256",filterMissing["frame_sha256"]},
        {"words",QJsonArray{token(QStringLiteral("所"),1894,456,19,18),token(QStringLiteral("有"),1915,455,19,19),
                           token(QStringLiteral("成"),1936,455,19,19),token(QStringLiteral("色"),1957,455,19,19)}}};
    const auto originalFilter=filterMissing,originalRead=labelRead;
    QJsonObject labelEvidence;
    const auto filterRefined=refineListingFilterAllLabel(filterMissing,labelRead,labelRoi,&labelEvidence);
    const auto refinedPage=classifySkinPage(filterRefined);
    check(labelEvidence["replacement_applied"].toBool() &&labelEvidence["same_frame"].toBool()
        &&!labelEvidence["actions_enabled"].toBool() &&refinedPage.overlay==PageOverlay::ListingFilter
        &&refinedPage.anchorChecks["listing.filter_all"].toBool(),"same_frame_actual_label_tokens_restore_original_three_anchor_modal_rule");
    check(filterMissing==originalFilter &&labelRead==originalRead,"filter_refinement_inputs_unchanged");
    for(const auto& removed:QStringList{QStringLiteral("在售"),QStringLiteral("默认排序"),QStringLiteral("价格区间"),QStringLiteral("确定")}){
        auto invalidContext=filterMissing;QJsonArray remaining;
        for(const auto& value:invalidContext["words"].toArray())
            if(value.toObject()["text"]!=removed)remaining.append(value);
        invalidContext["words"]=remaining;
        check(listingFilterAllLabelRefinementRegion(invalidContext).isEmpty(),"label_refinement_requires_independent_context_"+removed);
    }
    check(listingFilterAllLabelRefinementRegion(filterRefined).isEmpty(),"complete_modal_not_repeatedly_refined");
    auto wrongSize=scaled(filterMissing,.5);
    check(listingFilterAllLabelRefinementRegion(wrongSize).isEmpty(),"fixed_label_roi_not_applied_to_unmeasured_resolution");
    const std::function<void(QJsonObject&)> corruptLabel[] = {
        [](auto& p){p["frame_id"]="old-frame";},[](auto& p){p["frame_sha256"]=QString(64,QLatin1Char('b'));},
        [](auto& p){p["same_frame"]=false;},[](auto& p){p["coverage"]="full_client";},
        [](auto& p){p["width"]=1280;},[](auto& p){p["words"]=QJsonArray{};},
        [](auto& p){auto w=p["words"].toArray();auto a=w[0].toObject();a["x"]=1885;w[0]=a;p["words"]=w;},
        [](auto& p){auto w=p["words"].toArray();auto a=w[1].toObject();a["text"]=QStringLiteral("右");w[1]=a;p["words"]=w;},
        [](auto& p){auto w=p["words"].toArray();w.removeAt(1);p["words"]=w;},
        [](auto& p){auto w=p["words"].toArray();auto a=w[0].toObject();a["score"]=.69;w[0]=a;p["words"]=w;},
        [](auto& p){QJsonArray w;for(int i=0;i<17;++i)w.append(token(QStringLiteral("所"),1894,456,19,18));p["words"]=w;}
    };
    for(const auto& corrupt:corruptLabel){
        auto bad=labelRead;corrupt(bad);QJsonObject rejected;
        const auto unchanged=refineListingFilterAllLabel(filterMissing,bad,labelRoi,&rejected);
        check(!rejected["replacement_applied"].toBool() &&unchanged==filterMissing,
            "invalid_missing_shifted_or_cross_frame_label_never_invents_modal_evidence");
    }
    QJsonObject wrongRoiEvidence;
    check(refineListingFilterAllLabel(filterMissing,labelRead,labelRoi.translated(1,0),&wrongRoiEvidence)==filterMissing
        &&!wrongRoiEvidence["replacement_applied"].toBool(),"caller_cannot_expand_or_shift_label_refinement_roi");
    // Synthetic replies at the recorded run03 label positions. These verify
    // merge contracts, not OCR accuracy of the original unsaved game pixels.
    QJsonObject unknownEmpty{{"width",2560},{"height",1440},{"coverage","full_client"},
        {"frame_id","fixture:empty-refinement"},{"frame_sha256",QString(64,QLatin1Char('d'))},
        {"words",QJsonArray{token(QStringLiteral("暂未添任何关注"),1165,879,230,20)}}};
    auto saleRead=unknownEmpty; saleRead["coverage"]="roi";saleRead["same_frame"]=true;
    saleRead["words"]=QJsonArray{token(QStringLiteral("在售"),235,93,41,20)};
    auto emptyRead=saleRead;
    QJsonArray messageWords;
    const QString message=QStringLiteral("暂未添加任何关注的皮肤");
    for(int i=0;i<message.size();++i)messageWords.append(token(message.mid(i,1),1165+i*21,879,20,20));
    emptyRead["words"]=messageWords;
    QJsonObject emptyEvidence;
    const auto fixedEmpty=refineEmptyWatchlistPage(unknownEmpty,saleRead,emptyRead,&emptyEvidence);
    check(classifySkinPage(unknownEmpty).page==SkinPage::Unknown
        &&classifySkinPage(fixedEmpty).page==SkinPage::EmptyWatchlist
        &&emptyEvidence["replacement_applied"].toBool() &&!emptyEvidence["actions_enabled"].toBool(),
        "new_same_frame_full_exact_empty_message_and_sale_required");
    const std::function<void(QJsonObject&)> corruptEmpty[] = {
        [](auto& p){p["frame_id"]="old-frame";},[](auto& p){p["frame_sha256"]=QString(64,QLatin1Char('a'));},
        [](auto& p){p["same_frame"]=false;},[](auto& p){p["coverage"]="full_client";},
        [](auto& p){p["height"]=1080;},[](auto& p){p["words"]=QJsonArray{};},
        [](auto& p){auto w=p["words"].toArray();w.removeAt(3);p["words"]=w;},
        [](auto& p){auto w=p["words"].toArray();auto x=w[3].toObject();x["text"]=QStringLiteral("减");w[3]=x;p["words"]=w;},
        [](auto& p){auto w=p["words"].toArray();auto x=w[0].toObject();x["score"]=.69;w[0]=x;p["words"]=w;},
        [](auto& p){auto w=p["words"].toArray();auto x=w[0].toObject();x["x"]=500;w[0]=x;p["words"]=w;}
    };
    for(const auto& corrupt:corruptEmpty){
        auto bad=emptyRead;corrupt(bad);QJsonObject evidence;
        check(refineEmptyWatchlistPage(unknownEmpty,saleRead,bad,&evidence)==unknownEmpty
            &&!evidence["replacement_applied"].toBool(),"partial_cross_frame_wrong_or_outside_message_rejected");
    }
    auto wrongSale=saleRead;wrongSale["words"]=QJsonArray{token(QStringLiteral("停售"),235,93,41,20)};
    check(refineEmptyWatchlistPage(unknownEmpty,wrongSale,emptyRead)==unknownEmpty,"message_alone_does_not_prove_empty_page");
    check(refineEmptyWatchlistPage(fixedEmpty,saleRead,emptyRead)==fixedEmpty,"known_page_not_refined_again");
    auto wrongEmptySize=scaled(unknownEmpty,.5);
    check(refineEmptyWatchlistPage(wrongEmptySize,saleRead,emptyRead)==wrongEmptySize,"unmeasured_resolution_not_refined");
    {
        // peek01: the watchlist its refresh emptied. Full OCR kept only the
        // filter/sort labels; three new same-frame reads prove the page.
        QJsonObject emptied{{"width",2560},{"height",1440},{"coverage","full_client"},
            {"frame_id","fixture:emptied"},{"frame_sha256",QString(64,QLatin1Char('e'))},
            {"words",QJsonArray{token(QStringLiteral("不限公示期"),527,254,105,22),
                                token(QStringLiteral("按稀有度升序"),1153,253,126,22)}}};
        auto sale=emptied;sale["coverage"]="roi";sale["same_frame"]=true;
        sale["words"]=QJsonArray{token(QStringLiteral("在售"),235,93,41,20)};
        auto content=sale;content["words"]=QJsonArray{token(QStringLiteral("暂无信息内容"),928,861,125,22)};
        auto count=sale;count["words"]=QJsonArray{token(QStringLiteral("我的关注:0/150"),120,1250,173,22)};
        QJsonObject evidence;
        const auto fixed=refineEmptiedWatchlistPage(emptied,sale,content,count,&evidence);
        check(classifySkinPage(emptied).page==SkinPage::Unknown &&classifySkinPage(fixed).page==SkinPage::EmptyWatchlist
              &&evidence["replacement_applied"].toBool(),"refresh_emptied_watchlist_needs_message_and_count");
        auto noCount=count;noCount["words"]=QJsonArray{token(QStringLiteral("第1页"),940,1250,60,22)};
        check(refineEmptiedWatchlistPage(emptied,sale,content,noCount)==emptied,"no_content_message_alone_is_not_the_watchlist");
        auto outside=content;outside["words"]=QJsonArray{token(QStringLiteral("暂无信息内容"),1300,861,125,22)};
        check(refineEmptiedWatchlistPage(emptied,sale,outside,count)==emptied,"emptied_message_outside_its_region_rejected");
        auto stale=count;stale["frame_id"]="old-frame";
        check(refineEmptiedWatchlistPage(emptied,sale,content,stale)==emptied,"emptied_reads_must_share_the_frame");
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
