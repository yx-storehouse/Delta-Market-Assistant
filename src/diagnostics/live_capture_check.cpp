#include "live_capture_check.h"
#include "foreground_policy.h"
#include "live_capture_protocol.h"
#include "collection_label_refinement.h"
#include "collection_scroll_readiness.h"
#include "application/vision/dxgi_observation_source.h"
#include "application/vision/windows_ocr.h"
#include "application/vision/skin_page_classifier.h"
#include "application/vision/catalog_filter_reader.h"
#include "application/vision/collection_layout.h"
#include "application/vision/collection_ocr_fields.h"
#include "application/vision/numeric_roi.h"
#include "application/vision/frame_sha256.h"
#include "application/vision/collection_page_context.h"
#include "application/vision/collection_listing_regions.h"
#include "application/vision/latest_frame_stream.h"
#include "application/vision/purchase_observation.h"
#include "application/vision/purchase_countdown.h"

#include <QBuffer>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QImage>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QUuid>
#include <cmath>
#include <cstdio>
#include <limits>
#include <QHash>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

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
struct CaptureResult { QJsonObject packet; int status = 0; };
struct FavoriteStarCounts { int warm = 0, bright = 0, total = 0; };
// The fixed detail-panel favorite star, identical to the selected-card packet.
FavoriteStarCounts favoriteStarCounts(const FrameEnvelope& frame) {
    FavoriteStarCounts counts;
    if(frame.width!=2560 ||frame.height!=1440 ||frame.strideBytes!=2560*4
        ||frame.pixels.size()!=qint64(frame.strideBytes)*frame.height)return counts;
    for(int y=303;y<337;++y)for(int x=2320;x<2356;++x){
        const auto* p=reinterpret_cast<const unsigned char*>(frame.pixels.constData()+qint64(y)*frame.strideBytes+x*4);
        ++counts.total;
        counts.warm+=int(p[2])>110 && int(p[2])-int(p[1])>15 && int(p[1])-int(p[0])>15;
        counts.bright+=p[0]>140 && p[1]>140 && p[2]>140;
    }
    return counts;
}
// Two same-frame halves of one ROI (already in client coordinates) as one
// region read: words seen in both overlap halves are kept once.
OcrReply mergeSplitRegion(const OcrReply& top,const OcrReply& bottom,const FrameEnvelope& frame,const QRect& region) {
    if(!top.ok)return top;
    if(!bottom.ok)return bottom;
    OcrReply merged=top;
    merged.observation["words"]=mergeSplitRegionWords(top.observation["words"].toArray(),bottom.observation["words"].toArray());
    merged.observation["frame_id"]=frame.frameId;
    merged.observation["split_region"]=QJsonArray{region.x(),region.y(),region.width(),region.height()};
    merged.timing=QJsonObject{{"split_halves",QJsonArray{top.timing,bottom.timing}},{"split_overlap_px",60},
        {"frame_total_ms",std::max(top.timing["frame_total_ms"].toDouble(),bottom.timing["frame_total_ms"].toDouble())},
        {"frame_released",top.timing["frame_released"].toBool() &&bottom.timing["frame_released"].toBool()}};
    return merged;
}
QString ocrJobKey(const OcrRegionJob& job) {
    return QStringLiteral("%1|%2,%3,%4,%5|%6|%7|%8").arg(job.language).arg(job.region.x()).arg(job.region.y())
        .arg(job.region.width()).arg(job.region.height()).arg(job.scale).arg(job.invert).arg(job.numeric);
}
CaptureResult capture(const QStringList& arguments, WindowsOcrRecognizer& recognizer,
        const std::shared_ptr<DxgiCaptureResources>& sharedCapture = {}, CollectionPageContext* pageContext = nullptr,
        DxgiFrameStream* frameStream = nullptr, WindowsOcrPool* ocrPool = nullptr) {
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"live-capture-check", "Explicit read-only capture; standalone returns IDE once, caller-owned batch never switches focus."});
    parser.addOption({"focus-policy", "standalone (default) or caller-owned for an enclosing foreground batch.", "policy", "standalone"});
    parser.addOption({"target-hwnd", "Exact target window handle (decimal).", "hwnd"});
    parser.addOption({"target-pid", "Exact target process ID (decimal).", "pid"});
    parser.addOption({"return-hwnd", "Exact IDE window to restore (decimal).", "hwnd"});
    parser.addOption({"return-pid", "Exact IDE process ID (decimal).", "pid"});
    parser.addOption({"frames", "Bounded sample count, 1 to 5; default 1.", "count", "1"});
    parser.addOption({"reuse-capture-resources", "Server only: reuse validated GPU resources, never pixels; each request requires a fresh presentation."});
    parser.addOption({"collection-ready-stream", "Server only: latest-only memory capture during this foreground session; bounded native readiness watch, no OCR on unready frames."});
    parser.addOption({"collection-scroll-readiness", "Server only: after wheel input, observe stable current layout before full OCR; never changes scroll coverage or selection association."});
    parser.addOption({"collection-pixel-receipt", "Server only: after a dispatched favorite, watch new frames until the referenced selected card shows the gold detail star. Pixel-only receipt observation, no OCR, never authorizes input."});
    parser.addOption({"preview-stdout", "Explicit final-frame PNG preview over stdout only; no file output."});
    parser.addOption({"ocr", "Run Windows OCR on the final in-memory frame without intermediate foreground switches."});
    parser.addOption({"purchase-observation", "With full OCR: same-frame watchlist/countdown/dialog text evidence only; no input or purchase."});
    parser.addOption({"purchase-countdown-watch", "Server only, with ready stream and purchase observation: watch the countdown line on every new frame for N ms (500..10000); read only that line when its pixels change; full OCR of the last frame. Timing evidence only, no input.", "ms"});
    parser.addOption({"purchase-countdown-area", "With the countdown watch: footer (watchlist detail, default) or dialog (外观购买).", "area", "footer"});
    parser.addOption({"purchase-countdown-lean", "With the countdown watch: no full OCR of the last frame, so the watch returns right after its end (the purchase dialog's countdown before the click). Timing evidence only, no input."});
    parser.addOption({"purchase-countdown-stop-on-green", "With the countdown watch: also end it two frames after the buy button shows the green price fill, even on the first frame. Timing evidence only, no input."});
    parser.addOption({"purchase-countdown-stop-on-button", "With the countdown watch: end it two frames after the buy button's pixels change (公示中 to price). Timing evidence only, no input."});
    parser.addOption({"purchase-countdown-expect-zero-ms", "With --purchase-countdown-stop-on-jump: the caller's followed display zero (QPC ms); the watch's first readable line far from it also ends the watch.", "ms"});
    parser.addOption({"purchase-countdown-stop-on-jump", "With the countdown watch: end it two frames after the line reads a value that does not follow the previous one (another listing in the panel). Timing evidence only, no input."});
    parser.addOption({"purchase-countdown-stop-on-zero", "With the countdown watch: end it two frames after the line first reads 0 seconds after a positive value (the dialog's zero), so the zero lies inside one watch. Timing evidence only, no input."});
    parser.addOption({"ocr-lobby-anchors", "With --ocr: emit text boxes from three fixed lobby button regions only, not full OCR."});
    parser.addOption({"ocr-market-anchors", "With --ocr: emit bounded allowlisted market labels, not full OCR or images."});
    parser.addOption({"ocr-ui-regions", "With --ocr: reread three UI-label regions at 2x from the same memory frame; no extra capture."});
    parser.addOption({"catalog-filter-state", "With --ocr: read season and checkbox states from the same frame; no input or image files."});
    parser.addOption({"collection-observation", "With --ocr: bounded catalogue names/title/season labels from this frame only."});
    parser.addOption({"collection-layout-profile", "With --ocr: same-frame numerical row/column features; no image file or actionable layout."});
    parser.addOption({"collection-layout", "With collection observation: detect visible card and scrollbar edges in this same frame."});
    parser.addOption({"collection-selection-preflight", "Server only: defer OCR while a post-selection frame lacks one fully observed selected card. Never authorizes an action."});
    parser.addOption({"collection-selection-point", "Internal dispatched selection point: old selected peer cannot end the readiness watch; original geometry rebind still required.","json"});
    parser.addOption({"collection-local-hotpath", "Server only: continue calibrated listing context with exact current pixels or current listing ROIs; a miss uses full OCR of this same frame."});
    parser.addOption({"collection-receipt-reference", "Prior selected rectangle as a search hint for an already dispatched receipt; never authorizes a new action.","json"});
    parser.addOption({"collection-numeric-price", "Prepare numeric ink with borders removed before same-frame Windows OCR."});
    parser.addOption({"collection-local-price", "With numeric-price and price-image: emit measured ink metadata for a fixed local price provider, without Windows price OCR."});
    parser.addOption({"collection-price-image", "Explicit raw selected price ROI PNG over stdout only for local in-memory OCR comparison."});
    parser.addOption({"collection-title-image", "With collection observation: raw product-title ROI PNG over stdout only, independent of card geometry."});
    parser.addOption({"collection-local-text", "Fixed local text provider: emit same-frame title/catalogue crops without duplicate native text recognition."});
    parser.addOption({"collection-catalog-image", "With collection observation: raw catalog-names ROI PNG over stdout only on the classified skin home page."});
    parser.addOption({"collection-card-id", "With dynamic layout: identify an observed card without overriding actual selection.","id"});
    parser.addOption({"collection-visible-index", "With dynamic layout: reference a measured visible card, not a legacy fixed grid.","index"});
    parser.addOption({"collection-card-index", "With collection observation: visible card 0..5, row-major.","index","0"});
    parser.addOption({"expected-page", "With --ocr: require this page and no blocking overlay; mismatch exits 1.", "page"});
    if (!parser.parse(arguments) || !parser.positionalArguments().isEmpty())
        return {captureServiceError(QStringLiteral("E_DIAGNOSTIC_ARGUMENTS")), 2};
    if (parser.isSet("help"))
        return {QJsonObject{{"diagnostic", "live_capture"}, {"help", parser.helpText()}}, 0};
    const quintptr targetHwnd = positive(parser.value("target-hwnd"), std::numeric_limits<quintptr>::max());
    const quint32 targetPid = positive(parser.value("target-pid"), std::numeric_limits<quint32>::max());
    const quintptr returnHwnd = positive(parser.value("return-hwnd"), std::numeric_limits<quintptr>::max());
    const quint32 returnPid = positive(parser.value("return-pid"), std::numeric_limits<quint32>::max());
    const int frameCount = int(positive(parser.value("frames"), 5));
    bool cardIndexOk=false;const int cardIndex=parser.value("collection-card-index").toInt(&cardIndexOk);
    const bool dynamicLayout=parser.isSet("collection-layout");
    const bool pixelReceipt=parser.isSet("collection-pixel-receipt");
    const auto receiptDocument=QJsonDocument::fromJson(parser.value("collection-receipt-reference").toUtf8());
    const auto receiptReference=receiptDocument.object();
    const auto selectionPoint=QJsonDocument::fromJson(parser.value("collection-selection-point").toUtf8()).array();
    bool visibleIndexOk=true;const int visibleIndex=parser.isSet("collection-visible-index")?parser.value("collection-visible-index").toInt(&visibleIndexOk):-1;
    const int cardDx=(cardIndex%2)*877,cardDy=(cardIndex/2)*275;
    QJsonObject result{{"diagnostic", "live_capture"}, {"capture_passed", false}, {"frames", QJsonArray{}},
        {"recognition_performed", false}, {"image_file_writes", 0}, {"game_input_sent", false},
        {"ide_foreground_restored", false}, {"game_in_background", false}};
    CountdownArea countdownAreaRects;
    const bool countdownAreaOk=countdownArea(parser.value("purchase-countdown-area"),&countdownAreaRects);
    bool countdownMsOk=true;
    const int countdownWatchMs=parser.isSet("purchase-countdown-watch")?parser.value("purchase-countdown-watch").toInt(&countdownMsOk):0;
    bool expectZeroOk=true;
    const double expectZeroMs=parser.isSet("purchase-countdown-expect-zero-ms")
        ?parser.value("purchase-countdown-expect-zero-ms").toDouble(&expectZeroOk):0.0;
    expectZeroOk=expectZeroOk &&std::isfinite(expectZeroMs) &&(!parser.isSet("purchase-countdown-expect-zero-ms")
        ||parser.isSet("purchase-countdown-stop-on-jump"));
    const auto focusPolicy = parser.value("focus-policy");
    if (focusPolicy != "standalone" && focusPolicy != "caller-owned") {
        result["error"] = "E_DIAGNOSTIC_FOCUS_POLICY"; return {result, 2};
    }
    const bool callerOwned = focusPolicy == "caller-owned";
    // A caller-owned probe never switches windows, so the window the batch
    // returns to is the caller's business. The hotkey runner (started inside
    // the game or from the program) sends none; agent batches still bind the IDE.
    const bool returnWindowBound = !callerOwned || returnHwnd || returnPid;
    result["focus_policy"] = focusPolicy;
    result["foreground_cleanup_owner"] = callerOwned ? "batch_caller" : "diagnostic";
    const QString expectedPage = parser.value("expected-page");
    const QStringList pageNames{"lobby", "warehouse", "mandel", "skin_home", "empty_watchlist", "catalog_filter",
        "skin_listings", "watchlist_listings", "game_settings", "base"};
    if (parser.isSet("expected-page") && (!parser.isSet("ocr") || !pageNames.contains(expectedPage))) {
        result["error"] = "E_DIAGNOSTIC_EXPECTED_PAGE"; return {result, 2};
    }
    if (!expectedPage.isEmpty()) { result["expected_page"] = expectedPage; result["page_match_passed"] = false; }
    if(parser.isSet("purchase-observation") &&(!parser.isSet("ocr")
        ||parser.isSet("collection-local-hotpath") ||parser.isSet("collection-selection-preflight")
        ||parser.isSet("collection-pixel-receipt") ||parser.isSet("collection-scroll-readiness"))){
        result["error"]="E_PURCHASE_FULL_OCR_REQUIRED";return {result,2};
    }
    // The countdown watch reads the latest-frame stream of the watchlist
    // page; the final frame's full OCR binds the ticks to the selected offer.
    // The expected page is optional: live clock06 lost the run 8 s before the
    // unlock when one frame's OCR missed the small "我的关注 1/150" corner and
    // the unchanged page read as skin_listings. The caller checks the page.
    // A lean watch may be short: the last moments before a timed click.
    if((parser.isSet("purchase-countdown-watch") &&(!countdownMsOk
        ||countdownWatchMs<(parser.isSet("purchase-countdown-lean")?100:500) ||countdownWatchMs>10000
        ||(!parser.isSet("purchase-observation") &&!parser.isSet("purchase-countdown-lean"))
        ||(parser.isSet("purchase-countdown-lean") &&(parser.isSet("ocr") ||parser.isSet("purchase-observation")))
        ||!parser.isSet("collection-ready-stream")
        ||(!expectedPage.isEmpty() &&expectedPage!="watchlist_listings") ||parser.isSet("preview-stdout")))
        ||((parser.isSet("purchase-countdown-stop-on-button") ||parser.isSet("purchase-countdown-area")
            ||parser.isSet("purchase-countdown-stop-on-jump") ||parser.isSet("purchase-countdown-stop-on-zero")
            ||parser.isSet("purchase-countdown-stop-on-green") ||parser.isSet("purchase-countdown-lean"))
            &&!parser.isSet("purchase-countdown-watch")) ||!countdownAreaOk ||!expectZeroOk){
        result["error"]="E_PURCHASE_COUNTDOWN_WATCH_ARGUMENTS";return {result,2};
    }
    if (!visibleIndexOk || (parser.isSet("collection-visible-index") && visibleIndex<0)
        ||(parser.isSet("collection-selection-point")&&(!parser.isSet("collection-selection-preflight")
            ||!collectionSelectionPointValid(selectionPoint)))
        || (parser.isSet("collection-scroll-readiness") &&(!parser.isSet("collection-ready-stream")
            ||!dynamicLayout||expectedPage!="skin_listings"||parser.isSet("collection-selection-preflight")
            ||parser.isSet("collection-receipt-reference")))
        || (parser.isSet("collection-ready-stream") && (!frameStream ||!sharedCapture ||!callerOwned
            ||!parser.isSet("reuse-capture-resources") ||frameCount!=1))
        || (parser.isSet("collection-local-hotpath") && (!pageContext || !callerOwned || !dynamicLayout
            || expectedPage!="skin_listings" || frameCount!=1
            || (!parser.isSet("collection-selection-preflight") && !parser.isSet("collection-receipt-reference")
                && !parser.isSet("collection-scroll-readiness"))))
        || (pixelReceipt && (!parser.isSet("collection-ready-stream") || !frameStream || !sharedCapture || !callerOwned
            || !parser.isSet("reuse-capture-resources") || frameCount!=1 || !parser.isSet("collection-receipt-reference")
            || parser.isSet("ocr") || dynamicLayout || parser.isSet("collection-observation")
            || parser.isSet("collection-selection-preflight") || parser.isSet("collection-scroll-readiness")
            || parser.isSet("collection-local-hotpath") || parser.isSet("preview-stdout")))
        || (parser.isSet("collection-selection-preflight") && (!sharedCapture ||!callerOwned ||!dynamicLayout
            ||expectedPage!="skin_listings" ||parser.isSet("collection-receipt-reference")))
        || (parser.isSet("reuse-capture-resources") && (!sharedCapture || !callerOwned))
        || (parser.isSet("collection-local-text") &&(!parser.isSet("collection-observation")
            ||!parser.isSet("collection-title-image") ||!parser.isSet("collection-catalog-image")))
        || (parser.isSet("collection-local-price") && (!parser.isSet("collection-price-image")
            || !parser.isSet("collection-numeric-price") || !dynamicLayout))
        || (parser.isSet("collection-title-image") &&!parser.isSet("collection-observation"))
        || (parser.isSet("collection-catalog-image") &&!parser.isSet("collection-observation"))
        || (((parser.isSet("collection-receipt-reference") &&!pixelReceipt) ||parser.isSet("collection-numeric-price") ||parser.isSet("collection-price-image")) &&!dynamicLayout)
        || (parser.isSet("collection-receipt-reference") &&(receiptReference.isEmpty() ||parser.value("collection-receipt-reference").size()>4096))
        || (dynamicLayout && (!parser.isSet("collection-observation") ||parser.isSet("collection-card-index")))
        || ((parser.isSet("collection-card-id") ||parser.isSet("collection-visible-index")) &&!dynamicLayout)
        || (parser.isSet("collection-card-id") &&parser.isSet("collection-visible-index"))
        || !cardIndexOk || cardIndex<0 || cardIndex>5 || (parser.isSet("collection-card-index") && !parser.isSet("collection-observation"))
        || !frameCount || !targetHwnd
        || (returnWindowBound && (!returnHwnd || targetHwnd == returnHwnd || targetPid == returnPid))
        || ((parser.isSet("ocr-lobby-anchors") || parser.isSet("ocr-market-anchors") || parser.isSet("ocr-ui-regions") || parser.isSet("catalog-filter-state") || parser.isSet("collection-observation") || parser.isSet("collection-layout-profile")) && !parser.isSet("ocr"))) {
        result["error"] = "E_DIAGNOSTIC_ARGUMENTS"; return {result, 2};
    }
    // DXGI permits a single duplication for a given output in this process.
    // A cold request must release the previous explicit cache before creating
    // its request-local duplication; mode toggling must not create both.
    const bool streaming=parser.isSet("collection-ready-stream");
    const bool selectionWatch=streaming &&parser.isSet("collection-selection-preflight");
    const bool scrollWatch=streaming &&parser.isSet("collection-scroll-readiness");
    const bool pixelWatch=streaming &&pixelReceipt;
    const bool readyWatch=selectionWatch||scrollWatch||pixelWatch;
    const bool countdownWatch=streaming &&parser.isSet("purchase-countdown-watch");
    CollectionScrollReadiness scrollReadiness;
    if(frameStream &&!streaming)frameStream->stop();
    if (sharedCapture && (!parser.isSet("reuse-capture-resources") ||streaming) && !sharedCapture->discard()) {
        result["error"]="E_CAPTURE_RESOURCE_BUSY";return {result,1};
    }
    QString error;
    TargetWindow ide;
    if (returnWindowBound) {
        ide = bindTargetWindow(returnHwnd, returnPid, &error);
        if (!error.isEmpty()) { result["error"] = error; return {result, 2}; }
    }
    auto target = bindTargetWindow(targetHwnd, targetPid, &error);
    if (!error.isEmpty()) { result["error"] = error; return {result, 2}; }
    // Diagnostic allowlist prevents accidentally capturing an unrelated browser
    // or arbitrary desktop. The reusable backend itself binds caller identities.
    if (target.executableName.compare(QStringLiteral("DeltaForceClient-Win64-Shipping.exe"), Qt::CaseInsensitive) != 0
        || (returnWindowBound && ide.executableName.compare(QStringLiteral("Mirasim.exe"), Qt::CaseInsensitive) != 0)) {
        result["error"] = "E_DIAGNOSTIC_TARGET"; return {result, 2};
    }
    QString restoreError;
    ForegroundPolicy focus(callerOwned, targetHwnd, returnHwnd, foregroundWindow,
        [&]() { return activateTargetWindow(target, &error); },
        [&]() { return activateTargetWindow(ide, &restoreError); });
    QJsonArray frames;
    QByteArray finalPixels;
    FrameEnvelope ocrFrame;
    QString ocrFrameSha;
    int finalWidth = 0, finalHeight = 0, finalStride = 0;
    QJsonObject selectionLayout, selectionGate;
    QJsonArray readinessSamples;
    // Countdown watch: the reference ink is the last line that was read; a
    // change is timed between the previous examined frame and this one.
    CountdownInk countdownReference;
    QJsonArray countdownEvents;
    QJsonObject countdownStartClock;
    qint64 countdownPreviousSource=-1,countdownFirstSource=-1,countdownMaxGap=0,countdownWatchEnd=0,countdownLastRequested=0;
    int countdownFrames=0,countdownReads=0,countdownSegments=1,countdownQuietMax=0;
    // The buy button: its reference is the last changed button; a change
    // may end the watch (stop-on-button) after it has settled.
    CountdownInk buttonReference;
    QJsonArray buttonEvents;
    int buttonStopIn=-1;
    bool stoppedOnButton=false;
    const bool stopOnButton=parser.isSet("purchase-countdown-stop-on-button");
    // Another listing in the panel (selection changed, head withdrawn): the
    // line jumps; end the watch so the caller re-reads the panel at once.
    const bool stopOnJump=parser.isSet("purchase-countdown-stop-on-jump");
    int lineSeconds=CountdownLineUnread,jumpStopIn=-1;
    qint64 lineSecondsMs=0;
    bool stoppedOnJump=false;
    QJsonValue jumpSeen;
    // The dialog's 0分0秒: one watch spans it instead of a gap between two
    // (review wf_b0b8309e-39d: a zero between watches was only inferred).
    const bool stopOnZero=parser.isSet("purchase-countdown-stop-on-zero");
    int zeroStopIn=-1;
    bool stoppedOnZero=false;
    QJsonValue zeroSeen;
    // The price may turn green between two watches: then the next watch's
    // first frame is already green and no change is seen.
    const bool stopOnGreen=parser.isSet("purchase-countdown-stop-on-green");
    QJsonValue greenSeen;
    FrameEnvelope receiptFrame;
    QString receiptFrameSha;
    int acceptableReceiptFrames=0;
    const auto describeFrame=[&](const FrameEnvelope& f,qint64 requestedMonoMs,qint64 notBeforeMonoMs) {
        const auto pixels = summarizeBgra(f.pixels, f.width, f.height, f.strideBytes);
        const auto digest=frameSha256(f.pixels);
        const QString frameSha=QString::fromLatin1(digest.bytes.toHex());
        return qMakePair(QJsonObject{{"width", f.width}, {"height", f.height}, {"dpi", f.dpiX},
            {"bytes", f.validBytes}, {"source", f.sourceKind}, {"freshness", toString(f.freshnessBasis)},
            {"capture_ms", f.captureEndMonoMs - f.captureStartMonoMs},
            {"capture_start_mono_ms",f.captureStartMonoMs},{"capture_end_mono_ms",f.captureEndMonoMs},
            {"source_mono_ms",f.sourceMonoMs},{"requested_mono_ms",requestedMonoMs},
            {"not_before_mono_ms",notBeforeMonoMs},
            {"source_age_ms", f.captureEndMonoMs - f.sourceMonoMs},
            {"source_uncertainty_ms", f.sourceUncertaintyMs}, {"near_black", pixels.nearBlack},
            {"pixel_min", pixels.minimum}, {"pixel_max", pixels.maximum},
            {"sha256", frameSha},{"digest_provider",digest.provider},{"digest_ms",digest.elapsedNs/1e6}},frameSha);
    };
    // Current listing ROIs of one frame (same frame, same scales). Used in
    // the readiness loop and in the page-context step below.
    struct ListingRegionsRead { bool valid=false; QJsonObject semantic; QJsonArray regions; QHash<QString,OcrReply> prefetched; };
    const auto readListingRegions=[&](const FrameEnvelope& frame,const QString& frameSha,QJsonObject& layout) {
        ListingRegionsRead out;
        QElapsedTimer timer;timer.start();QJsonArray currentRegions;
        const auto specs=collectionListingRegionSpecs();
        QList<OcrReply> parallelReads;
        if(ocrPool) {
            // Same frame, same ROIs/scales/preprocessing as the serial
            // loop below, recognized concurrently on pool helpers. The
            // fixed precise-wear ROI and the selected card's condition
            // ROI are read speculatively from THIS frame; the field
            // code below consumes them only for exactly those requests.
            QList<OcrRegionJob> jobs;
            for(const auto& spec:specs)
                jobs.append(OcrRegionJob{QStringLiteral("zh-Hans-CN"),spec.second,spec.first=="modal_guard"?1:2,true,false});
            QList<OcrRegionJob> speculative{OcrRegionJob{QStringLiteral("en-US"),QRect(2206,650,164,52),3,true,false}};
            if(dynamicLayout) {
                if(layout.isEmpty())layout=detectCollectionLayout(frame,receiptReference);
                QJsonArray candidates=layout["complete"].toBool()?layout["cards"].toArray():QJsonArray{};
                const auto proof=layout["selected_card_proof"].toObject();
                if(!layout["complete"].toBool() &&proof["proven"].toBool())candidates.append(proof["card"]);
                for(const auto& value:candidates) {
                    const auto card=value.toObject();const auto r=card["condition_bounds"].toArray();
                    if(card["selected"]!=true ||card["fields_bounds"].isNull() ||r.size()!=4)continue;
                    speculative.append(OcrRegionJob{QStringLiteral("zh-Hans-CN"),
                        QRect(r[0].toInt(),r[1].toInt(),r[2].toInt(),r[3].toInt()),2,true,false});
                }
            }
            // The broad modal guard dominates the batch: read it as two
            // halves overlapping by 60 px (taller than any one dialog
            // text line at 1x), then merge into its single region.
            const QRect guard=jobs[3].region;
            const int half=guard.height()/2,overlap=30;
            const OcrRegionJob guardTop{jobs[3].language,QRect(guard.x(),guard.y(),guard.width(),half+overlap),1,true,false};
            const OcrRegionJob guardBottom{jobs[3].language,QRect(guard.x(),guard.y()+half-overlap,guard.width(),
                guard.height()-half+overlap),1,true,false};
            QList<OcrRegionJob> ordered{guardTop,guardBottom,jobs[1],jobs[2],jobs[0]};
            ordered+=speculative;
            WindowsOcrRecognizer callerLatin(QCoreApplication::applicationDirPath()+QStringLiteral("/vision/windows_ocr_worker.ps1"),QStringLiteral("en-US"));
            const auto replies=ocrPool->run(frame,ordered,recognizer,callerLatin);
            parallelReads={replies[4],replies[2],replies[3],mergeSplitRegion(replies[0],replies[1],frame,guard)};
            for(int i=0;i<speculative.size();++i) {
                auto reply=replies[5+i];reply.timing["ocr_pool_speculative_same_frame"]=true;
                out.prefetched.insert(ocrJobKey(speculative[i]),reply);
            }
            result["ocr_pool"]=ocrPool->metrics();
        }
        for(int index=0;index<specs.size();++index) {
            const auto& spec=specs[index];
            // The broad dialog guard keeps the original full-frame
            // physical text size. Upscaling it would nearly recreate
            // the full-screen OCR workload before the field reads.
            const int scale=spec.first=="modal_guard"?1:2;
            const auto read=ocrPool?parallelReads[index]:recognizer.recognizeRegion(frame,spec.second,scale,true);
            const auto words=read.observation["words"].toArray();
            const bool bound=read.observation["frame_id"]==frame.frameId;
            currentRegions.append(QJsonObject{{"kind",spec.first},{"ok",read.ok},
                {"error",read.error},{"same_frame",bound},{"frame_id",read.observation["frame_id"]},
                {"frame_sha256",frameSha},{"truncated",words.size()>256},
                {"bounds",QJsonArray{spec.second.x(),spec.second.y(),spec.second.width(),spec.second.height()}},
                {"words",words},{"timing",read.timing},{"scale",scale},
                {"provider","Windows.Media.Ocr"},{"language","zh-Hans-CN"}});
            if(!read.ok ||!bound)break;
        }
        out.semantic=classifyCollectionListingRegions(frame,frameSha,currentRegions);
        out.semantic["elapsed_ms"]=timer.elapsed();out.semantic["observations"]=currentRegions;
        out.semantic["parallel_ocr"]=ocrPool!=nullptr;
        out.regions=currentRegions;out.valid=true;
        return out;
    };
    // A transient frame may lack only the static header/sort labels while the
    // selection is already visible (S11 pipeline run02: 2/9 frames ~300 ms
    // after a favorite). Read the next fresh frame instead of full-frame OCR
    // plus a coordinator retry. Bounded; any other miss keeps the fallback.
    const bool earlyHotpath=parser.isSet("collection-local-hotpath") &&pageContext &&parser.isSet("ocr")
        &&(selectionWatch ||scrollWatch);
    QJsonObject earlyProof;QString earlyFrameId;ListingRegionsRead earlyRead;int listingRetries=0;
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
            // The scroll gate ends as soon as 3 frames over 100 ms are stable;
            // its bound replaces the former fixed 180 ms pre-wait and absorbs a
            // slower list re-render (S11 pipeline run03: ~600 ms of frames
            // without a complete layout after the sixth wheel).
            demand.deadlineMonoMs = demand.createdMonoMs + (countdownWatch ? countdownWatchMs + 400 : pixelWatch ? 1200 : scrollWatch ? 1600
                : readyWatch ? 900 : 2500 * frameCount);
            countdownWatchEnd=demand.createdMonoMs+countdownWatchMs;
            demand.maxFrameAgeMs = 1000;
            // Frames now arrive every ~20 ms: the time deadline, not a
            // 16-frame count, bounds readiness (run02 exhausted 16 frames
            // 94 ms into the 100 ms scroll stability window).
            // 600 is the adapter's ceiling per demand. The stream delivers
            // ~130-145 frames/s here (hotkey run 20261008-222601), so a
            // countdown watch continues in further demands below.
            demand.frameBudget = countdownWatch ? 600 : readyWatch ? 48 : frameCount;
            demand.mode = readyWatch ||countdownWatch ||frameCount!=1 ? ObservationMode::BoundedWatch : ObservationMode::OneShot;
            demand.minIntervalMs = readyWatch ||countdownWatch ||frameCount==1 ? 0 : 150;
            demand.roiSpecs = {{QStringLiteral("client"), QStringLiteral("client_physical_px"), 0, 0,
                target.clientRect.width(), target.clientRect.height(), 1}};
            const auto resourceOwner = parser.isSet("reuse-capture-resources") ? sharedCapture
                : std::make_shared<DxgiCaptureResources>();
            QJsonObject resourceBefore = resourceOwner->metrics();
            // A countdown watch outlasting one demand's frame budget goes on
            // in a fresh bounded demand on the same stream. The read line and
            // the previous examined frame carry over, so a tick spanning the
            // switch is still timed between the frames actually examined.
            for (int segment = 0; ; ++segment) {
            if (segment) {
                demand.demandId = QStringLiteral("live-check:%1:%2").arg(session).arg(segment);
                demand.createdMonoMs = demand.notBeforeMonoMs = captureClockMs();
                demand.deadlineMonoMs = countdownWatchEnd + 400;
            }
            int segmentFrames = 0;
            std::unique_ptr<IObservationSource> source;
            if(streaming) {
                error=frameStream->ensure(target);
                if(error.isEmpty()) {
                    if(!segment)resourceBefore=frameStream->metrics()["resource_owner"].toObject();
                    source=std::make_unique<LatestFrameObservationSource>(*frameStream->pump(),target,demand);
                }
            } else source=std::make_unique<DxgiObservationSource>(target,demand,resourceOwner);
            ObservationAdapter adapter(source.get());
            if (error.isEmpty() &&adapter.start(demand, captureClockMs(), &error)) {
                if(countdownWatch &&!segment)countdownStartClock=purchaseClockPair();
                for (int i = 0; i < demand.frameBudget && error.isEmpty(); ++i) {
                    if (i &&!readyWatch &&!countdownWatch) QThread::msleep(150);
                    const auto request = adapter.beginAcquire(captureClockMs(), &error);
                    if (!error.isEmpty()) break;
                    const auto raw = source->capture(request);
                    const auto reply = adapter.completeCapture(raw, captureClockMs());
                    if (reply.status != CaptureStatus::Captured) { error = reply.errorCode; break; }
                    const auto accepted = adapter.consumePending(captureClockMs());
                    if (!accepted.accepted) { error = accepted.errorCode; break; }
                    const auto& f = accepted.frame;
                    // A countdown watch examines every frame of the stream:
                    // only its final frame (the one that receives full OCR
                    // below) is hashed and reported, after the loop.
                    QString frameSha;
                    if(countdownWatch)countdownLastRequested=request.requestedMonoMs;
                    else {
                        const auto described=describeFrame(f,request.requestedMonoMs,demand.notBeforeMonoMs);
                        frames.append(described.first);frameSha=described.second;
                    }
                    if (parser.isSet("preview-stdout")) {
                        finalPixels = f.pixels; finalWidth = f.width; finalHeight = f.height; finalStride = f.strideBytes;
                    }
                    // A lean countdown watch keeps its last frame too (reported, not read).
                    if (parser.isSet("ocr") ||countdownWatch) { ocrFrame = f; ocrFrameSha = frameSha; }
                    bool selectionReady=true;
                    if(pixelWatch) {
                        // Pixel-only: the layout and fixed star counts of THIS
                        // frame. Keep the final frame for the receipt packet.
                        QElapsedTimer timer;timer.start();
                        selectionLayout=detectCollectionLayout(f,receiptReference);
                        const auto star=favoriteStarCounts(f);
                        selectionGate=collectionPixelReceiptGate(selectionLayout,receiptReference,star.warm,star.bright,star.total);
                        selectionGate["elapsed_ms"]=timer.elapsed();
                        receiptFrame=f;receiptFrameSha=frameSha;ocrFrame=f;ocrFrameSha=frameSha;
                        acceptableReceiptFrames+=selectionGate["acceptable"].toBool();
                        // Prefer a complete layout; a receipt-only proof is
                        // accepted after three acceptable frames or at deadline.
                        selectionReady=selectionGate["ready"].toBool() ||acceptableReceiptFrames>=3;
                        readinessSamples.append(QJsonObject{{"frame_id",f.frameId},{"frame_sha256",frameSha},
                            {"source_mono_ms",f.sourceMonoMs},{"requested_mono_ms",request.requestedMonoMs},
                            {"source_uncertainty_ms",f.sourceUncertaintyMs},{"gate",selectionGate},
                            {"ocr_performed",false},{"fixed_retry_wait_ms",0}});
                    } else if(countdownWatch) {
                        const auto ink=countdownInk(f,countdownAreaRects.line);
                        if(!ink.valid)error=QStringLiteral("E_PURCHASE_COUNTDOWN_ROI");
                        else {
                            ++countdownFrames;
                            if(countdownFirstSource<0)countdownFirstSource=f.sourceMonoMs;
                            if(countdownPreviousSource>=0)countdownMaxGap=std::max(countdownMaxGap,f.sourceMonoMs-countdownPreviousSource);
                            const int changed=countdownStrongDifference(countdownReference,ink);
                            if(!countdownReference.valid ||changed>=PurchaseCountdownChangePixels) {
                                QJsonObject event{{"index",countdownEvents.size()},
                                    {"kind",countdownReference.valid?"change":"baseline"},
                                    {"frame_id",f.frameId},{"source_mono_ms",double(f.sourceMonoMs)},
                                    {"previous_source_mono_ms",countdownPreviousSource>=0?QJsonValue(double(countdownPreviousSource)):QJsonValue()},
                                    {"changed_px",changed},{"bright_px",ink.bright}};
                                // Bounded: a flickering line must not turn the
                                // watch into continuous OCR.
                                if(countdownReads<32) {
                                    QElapsedTimer timer;timer.start();
                                    // Diagnostic only: the whole line is read, and the
                                    // icon is removed from the words by its position
                                    // in Python (purchase_observation.strip_line_icon).
                                    const auto icon=countdownTextRegion(ink,countdownAreaRects.line);
                                    event["icon_candidate"]=QJsonObject{{"accepted",icon.iconExcluded},
                                        {"run_start",icon.runStart},{"run_end",icon.runEnd},{"next_run",icon.nextRun},
                                        {"run_pixels",icon.runPixels},{"run_height",icon.runHeight}};
                                    const auto& lineRect=countdownAreaRects.line;
                                    event["read_region"]=QJsonArray{lineRect.x(),lineRect.y(),lineRect.width(),lineRect.height()};
                                    const auto read=recognizer.recognizeRegion(f,lineRect,2,true);
                                    ++countdownReads;
                                    QJsonArray words;QString line;
                                    for(const auto& value:read.observation["words"].toArray()){
                                        if(words.size()>=24)break;
                                        const auto w=value.toObject();
                                        words.append(QJsonObject{{"text",w["text"].toString().left(32)},{"x",w["x"]},{"y",w["y"]},
                                            {"width",w["width"]},{"height",w["height"]}});
                                        line+=w["text"].toString().left(32);
                                    }
                                    event["ocr_ok"]=read.ok;event["ocr_error"]=read.error;
                                    event["text"]=line;event["words"]=words;event["read_ms"]=double(timer.elapsed());
                                    const int seconds=read.ok?countdownLineSeconds(countdownLineWithoutIcon(
                                        words,countdownLineLayout(countdownAreaRects.name))):CountdownLineUnread;
                                    event["line_seconds"]=seconds;
                                    const bool offClock=stopOnJump &&lineSeconds<0 &&parser.isSet("purchase-countdown-expect-zero-ms")
                                        &&countdownLineOffClock(expectZeroMs,seconds,f.sourceMonoMs);
                                    if(stopOnJump &&jumpStopIn<0 &&jumpSeen.isNull()
                                        &&(offClock ||countdownLineJumped(lineSeconds,lineSecondsMs,seconds,f.sourceMonoMs))) {
                                        jumpSeen=QJsonObject{{"from",offClock?QJsonValue("expected_zero"):QJsonValue(lineSeconds)},
                                            {"to",seconds},{"frame_id",f.frameId},{"source_mono_ms",double(f.sourceMonoMs)},
                                            {"expected_zero_ms",offClock?QJsonValue(expectZeroMs):QJsonValue()}};
                                        jumpStopIn=PurchaseButtonSettleFrames;
                                    }
                                    if(stopOnZero &&zeroStopIn<0 &&zeroSeen.isNull() &&seconds==0 &&lineSeconds>0) {
                                        zeroSeen=QJsonObject{{"from",lineSeconds},{"frame_id",f.frameId},
                                            {"source_mono_ms",double(f.sourceMonoMs)}};
                                        zeroStopIn=PurchaseButtonSettleFrames;
                                    }
                                    if(seconds>=0){lineSeconds=seconds;lineSecondsMs=f.sourceMonoMs;}
                                } else event["read_skipped"]=true;
                                event["recorded_mono_ms"]=double(captureClockMs());
                                countdownEvents.append(event);
                                countdownReference=ink;
                            } else countdownQuietMax=std::max(countdownQuietMax,changed);
                            const auto button=countdownInk(f,countdownAreaRects.button);
                            const int buttonChanged=countdownLevelDifference(buttonReference,button);
                            if(buttonReference.valid &&buttonChanged>=PurchaseButtonChangePixels &&buttonEvents.size()<16) {
                                buttonEvents.append(QJsonObject{{"index",buttonEvents.size()},{"frame_id",f.frameId},
                                    {"source_mono_ms",double(f.sourceMonoMs)},
                                    {"previous_source_mono_ms",countdownPreviousSource>=0?QJsonValue(double(countdownPreviousSource)):QJsonValue()},
                                    {"changed_px",buttonChanged},{"bright_px",button.bright}});
                                if(stopOnButton &&buttonStopIn<0)buttonStopIn=PurchaseButtonSettleFrames;
                            }
                            if(!buttonReference.valid ||buttonChanged>=PurchaseButtonChangePixels)buttonReference=button;
                            if(stopOnGreen &&greenSeen.isNull()) {
                                const double green=buttonGreenFraction(f,countdownAreaRects.button);
                                if(green>=PurchaseButtonGreenFill) {
                                    greenSeen=QJsonObject{{"frame_id",f.frameId},{"source_mono_ms",double(f.sourceMonoMs)},
                                        {"previous_source_mono_ms",countdownPreviousSource>=0?QJsonValue(double(countdownPreviousSource)):QJsonValue()},
                                        {"green_fraction",green}};
                                    if(buttonStopIn<0)buttonStopIn=PurchaseButtonSettleFrames;
                                }
                            }
                            countdownPreviousSource=f.sourceMonoMs;
                            if(buttonStopIn>=0 &&buttonStopIn--==0)stoppedOnButton=true;
                            if(jumpStopIn>=0 &&jumpStopIn--==0)stoppedOnJump=true;
                            if(zeroStopIn>=0 &&zeroStopIn--==0)stoppedOnZero=true;
                        }
                    } else if(readyWatch) {
                        QElapsedTimer timer;timer.start();
                        selectionLayout=detectCollectionLayout(f);
                        selectionGate=scrollWatch?scrollReadiness.observe(selectionLayout,f.sourceMonoMs)
                            :selectionPoint.isEmpty()?collectionSelectionGate(selectionLayout):collectionSelectionTargetGate(selectionLayout,selectionPoint);
                        selectionGate["elapsed_ms"]=timer.elapsed();
                        selectionReady=selectionGate["ready_for_full_ocr"].toBool();
                        QJsonObject sample{{"frame_id",f.frameId},{"frame_sha256",frameSha},
                            {"source_mono_ms",f.sourceMonoMs},{"requested_mono_ms",request.requestedMonoMs},
                            {"source_uncertainty_ms",f.sourceUncertaintyMs},{"gate",selectionGate},
                            {"ocr_performed",false},{"fixed_retry_wait_ms",0}};
                        if(selectionReady &&earlyHotpath) {
                            earlyRead={};earlyFrameId=f.frameId;
                            earlyProof=pageContext->check(f,target,frameSha);
                            if(earlyProof["used"]!=true &&earlyProof["current_regions_eligible"]==true) {
                                earlyRead=readListingRegions(f,frameSha,selectionLayout);
                                sample["listing_regions_reason"]=earlyRead.semantic["reason"];
                                sample["ocr_performed"]=true;
                                if(collectionListingStaticAnchorMiss(earlyRead.semantic) &&listingRetries<4
                                    &&captureClockMs()+200<demand.deadlineMonoMs) {
                                    ++listingRetries;selectionReady=false;
                                    sample["listing_regions_retry_next_frame"]=true;
                                }
                            }
                        }
                        readinessSamples.append(sample);
                    }
                    if (!adapter.releaseFrame(f.leaseId, f.slotGeneration)) error = QStringLiteral("E_DIAGNOSTIC_RELEASE");
                    ++segmentFrames;
                    if(readyWatch &&(selectionReady ||captureClockMs()+100>=demand.deadlineMonoMs))break;
                    if(countdownWatch &&(stoppedOnButton ||stoppedOnJump ||stoppedOnZero ||captureClockMs()>=countdownWatchEnd))break;
                }
                adapter.close();
                result["resources_drained"] = adapter.snapshot().resourcesDrained;
                if(streaming)result["resources_drained_scope"]="request_delivery_leases_only_producer_owned_by_session";
                result["adapter_state"] = toString(adapter.state());
            }
            if(!countdownWatch ||!error.isEmpty() ||stoppedOnButton ||stoppedOnJump ||stoppedOnZero ||segmentFrames<demand.frameBudget
                ||captureClockMs()+30>=countdownWatchEnd ||segment>=40)break;
            countdownSegments=segment+2;
            }
            if(countdownWatch &&!ocrFrame.frameId.isEmpty()) {
                const auto described=describeFrame(ocrFrame,countdownLastRequested,demand.notBeforeMonoMs);
                frames.append(described.first);ocrFrameSha=described.second;
            }
            auto resourceMetrics=streaming?frameStream->metrics()["resource_owner"].toObject():resourceOwner->metrics();
            resourceMetrics["shared_between_requests"]=parser.isSet("reuse-capture-resources");
            resourceMetrics["initializations_this_request"]=resourceMetrics["initializations"].toInt()-resourceBefore["initializations"].toInt();
            resourceMetrics["reuses_this_request"]=resourceMetrics["reuses"].toInt()-resourceBefore["reuses"].toInt();
            resourceMetrics["staging_creates_this_request"]=resourceMetrics["staging_creates"].toInt()-resourceBefore["staging_creates"].toInt();
            resourceMetrics["source_timestamp_barrier_unchanged"]=true;
            result["capture_resource_cache"]=resourceMetrics;
            if(streaming)result["capture_stream"]=frameStream->metrics();
            if(readyWatch)result["collection_ready_watch"]=QJsonObject{{"schema","collection-ready-watch-v1"},
                {"mode","native_latest_frame_readiness"},{"maximum_frames",demand.frameBudget},{"deadline_budget_ms",900},
                {"listing_region_frame_retries",listingRetries},
                {"samples",readinessSamples},{"fixed_retry_wait_ms",0},{"actions_enabled",false},
                {"kind",pixelWatch?"pixel_receipt":scrollWatch?"scroll":"selection"},{"full_candidate_validation_required",true},
                {"maximum_frames_requested",demand.frameBudget},{"deadline_budget_ms_requested",double(demand.deadlineMonoMs-demand.createdMonoMs)}};
            if(countdownWatch)result["purchase_countdown_watch"]=QJsonObject{{"schema","purchase-countdown-watch-v1"},
                {"area",countdownAreaRects.name},
                {"roi",QJsonArray{countdownAreaRects.line.x(),countdownAreaRects.line.y(),countdownAreaRects.line.width(),countdownAreaRects.line.height()}},
                {"requested_ms",countdownWatchMs},{"frames_examined",countdownFrames},{"line_reads",countdownReads},
                {"first_source_mono_ms",countdownFirstSource>=0?QJsonValue(double(countdownFirstSource)):QJsonValue()},
                {"last_source_mono_ms",countdownPreviousSource>=0?QJsonValue(double(countdownPreviousSource)):QJsonValue()},
                {"max_frame_gap_ms",double(countdownMaxGap)},{"change_threshold_px",PurchaseCountdownChangePixels},
                {"segments",countdownSegments},{"max_quiet_changed_px",countdownQuietMax},
                {"covered_ms",countdownFirstSource>=0?double(countdownPreviousSource-countdownFirstSource):0.0},
                {"ended_by",!error.isEmpty()?"error":stoppedOnButton?"button":stoppedOnJump?"jump":stoppedOnZero?"zero"
                    :captureClockMs()+30>=countdownWatchEnd?"time":"segment_limit"},
                {"stop_on_jump",stopOnJump},{"stopped_on_jump",stoppedOnJump},{"jump_seen",jumpSeen},
                {"stop_on_zero",stopOnZero},{"stopped_on_zero",stoppedOnZero},{"zero_seen",zeroSeen},
                {"button_rect",QJsonArray{countdownAreaRects.button.x(),countdownAreaRects.button.y(),countdownAreaRects.button.width(),countdownAreaRects.button.height()}},
                {"button_change_measure","ink_level_changes"},
                {"button_change_threshold_px",PurchaseButtonChangePixels},{"button_events",buttonEvents},
                {"stop_on_button",stopOnButton},{"stopped_on_button",stoppedOnButton},
                {"stop_on_green",stopOnGreen},{"green_seen",greenSeen},
                {"lean",parser.isSet("purchase-countdown-lean")},
                {"final_button_green_fraction",ocrFrame.frameId.isEmpty()?-1.0:buttonGreenFraction(ocrFrame,countdownAreaRects.button)},
                {"final_button_mean_bgr",ocrFrame.frameId.isEmpty()?QJsonArray{}:rectMeanBgr(ocrFrame,countdownAreaRects.button)},
                // Present times are QPC floored to whole ms: a present at
                // t lies in [t, t+1).
                {"time_rounding","floor_ms"},
                {"ink_levels",QJsonArray{PurchaseCountdownInkDark,PurchaseCountdownInkBright}},
                {"events",countdownEvents},{"clock_pairs",QJsonArray{countdownStartClock,purchaseClockPair()}},
                {"time_basis","dxgi_last_present_qpc_ms"},{"system_time_changed",false},
                {"actions_enabled",false},{"purchase_authorized",false},{"image_file_writes",0}};
            if(pixelWatch &&!receiptFrame.frameId.isEmpty()) {
                const bool acceptable=selectionGate["acceptable"].toBool();
                const bool complete=selectionLayout["complete"].toBool();
                const auto card=selectionGate["card"].toObject();
                result["collection_pixel_receipt"]=QJsonObject{{"schema","collection-pixel-receipt-v1"},
                    {"ready",acceptable},{"layout_complete",complete},{"frame_id",receiptFrame.frameId},
                    {"frame_sha256",receiptFrameSha},{"source_mono_ms",double(receiptFrame.sourceMonoMs)},
                    {"acceptable_frames",acceptableReceiptFrames},{"sample_count",readinessSamples.size()},
                    {"gate",selectionGate},{"reference",receiptReference},{"ocr_performed",false},
                    {"actions_enabled",false},{"identity_basis","pre_dispatch_candidate_geometry_and_journal"}};
                result["collection_layout"]=selectionLayout;
                result["collection_observation"]=QJsonObject{{"frame_id",receiptFrame.frameId},
                    {"frame_sha256",receiptFrameSha},{"same_frame",true},{"regions",QJsonArray{}},
                    {"coverage","pixel_receipt_only"},{"image_file_writes",0}};
                QJsonObject selected{{"selected",card["selected"]==true},{"same_frame",true},
                    {"frame_id",receiptFrame.frameId},{"frame_sha256",receiptFrameSha},
                    {"favorite_warm_fraction",selectionGate["favorite_warm_fraction"]},
                    {"favorite_bright_fraction",selectionGate["favorite_bright_fraction"]},
                    {"actions_enabled",false}};
                if(!card.isEmpty()){selected["card_id"]=card["id"];selected["bounds"]=card["bounds"];selected["fields_bounds"]=card["fields_bounds"];}
                if(!complete){selected["scope"]="receipt_only";selected["unique_selection_proven"]=false;}
                result["collection_selected_card"]=selected;
                result["startup_page"]=QJsonObject{{"page","unobserved"},{"overlay","unobserved"},{"valid_input",false},
                    {"actions_enabled",false},{"reason","PIXEL_RECEIPT_NO_PAGE_OCR"}};
                result["ocr_passed"]=false;
                result["ocr"]=QJsonObject{{"provider","not_invoked"},{"coverage","none"},{"frame_sha256",receiptFrameSha},
                    {"frame_age_at_result_ms",double(captureClockMs()-receiptFrame.sourceMonoMs)}};
            }
            if(scrollWatch) {
                result["collection_scroll_readiness"]=selectionGate;
                if(selectionGate["ready_for_full_ocr"]!=true)error="E_COLLECTION_SCROLL_NOT_STABLE";
            }
        }
    } catch (...) { error = QStringLiteral("E_DIAGNOSTIC_EXCEPTION"); }
    result["return_hwnd"] = QString::number(returnHwnd);
    result["frames"] = frames;
    result["capture_passed"] = error.isEmpty() && (readyWatch ||countdownWatch ? !frames.isEmpty() : frames.size() == frameCount);
    if (!error.isEmpty()) result["error"] = error;
    const int streamProducedBeforeOcr=result["capture_stream"].toObject()["produced"].toInt();
    bool deferSelectionOcr=false;
    if(error.isEmpty() &&parser.isSet("collection-selection-preflight")) {
        if(selectionGate.isEmpty()) {
            QElapsedTimer timer;timer.start();selectionLayout=detectCollectionLayout(ocrFrame);
            selectionGate=selectionPoint.isEmpty()?collectionSelectionGate(selectionLayout):collectionSelectionTargetGate(selectionLayout,selectionPoint);
            selectionGate["elapsed_ms"]=timer.elapsed();
        }
        const auto gate=selectionGate;
        result["collection_selection_preflight"]=gate;
        deferSelectionOcr=gate["ocr_deferred"].toBool();
        if(deferSelectionOcr) {
            result["collection_layout"]=selectionLayout;
            result["collection_observation"]=QJsonObject{{"frame_id",ocrFrame.frameId},
                {"frame_sha256",ocrFrameSha},{"same_frame",true},{"regions",QJsonArray{}}};
            result["startup_page"]=QJsonObject{{"page","unobserved"},{"overlay","unobserved"},
                {"valid_input",false},{"actions_enabled",false},{"reason","SELECTION_NOT_READY_FOR_OCR"}};
            result["ocr_passed"]=false;
            result["ocr"]=QJsonObject{{"provider","not_invoked"},{"coverage","none"},
                {"frame_sha256",ocrFrameSha},{"frame_age_at_result_ms",captureClockMs()-ocrFrame.sourceMonoMs}};
            const auto metrics=recognizer.sessionMetrics();
            result["ocr_session"]=captureRequestOcrMetrics(metrics,metrics);
        }
    }
    if (error.isEmpty() && parser.isSet("ocr") &&!deferSelectionOcr) {
        const auto sessionBefore=recognizer.sessionMetrics();
        bool localHotpath=false;
        QHash<QString,OcrReply> prefetched;
        QJsonObject contextProof,contextPage,contextControls,contextDetail;
        QString contextBasis="exact_current_pixel_page_context";
        if(parser.isSet("collection-local-hotpath")) {
            contextProof=earlyFrameId==ocrFrame.frameId &&!earlyProof.isEmpty()
                ?earlyProof:pageContext->check(ocrFrame,target,ocrFrameSha);
            contextPage=pageContext->page(contextProof);
            localHotpath=contextProof["used"]==true &&!contextPage.isEmpty();
            if(localHotpath)contextControls=pageContext->controls(contextProof);
            else if(contextProof["current_regions_eligible"]==true) {
                // The full calibration is server-owned and still bound to
                // this target/age. Read current pixels, not old control text;
                // subtle game animation must not force whole-screen OCR for
                // every selection and every receipt. A semantic miss falls
                // through to the original full OCR of this SAME frame.
                ListingRegionsRead read;
                if(earlyFrameId==ocrFrame.frameId &&earlyRead.valid)read=earlyRead;
                else read=readListingRegions(ocrFrame,ocrFrameSha,selectionLayout);
                prefetched=read.prefetched;
                const auto currentRegions=read.regions;
                const auto semantic=read.semantic;
                result["collection_current_listing_regions"]=semantic;
                if(semantic["matched"]==true) {
                    contextPage=semantic["startup_page"].toObject();
                    for(const auto& region:currentRegions) {
                        if(region.toObject()["kind"]=="listing_controls")contextControls=region.toObject();
                        if(region.toObject()["kind"]=="detail_anchors")contextDetail=region.toObject();
                    }
                    localHotpath=!contextPage.isEmpty() &&!contextControls.isEmpty();
                    if(localHotpath) {
                        contextBasis="current_same_frame_listing_regions";
                        const QString exactGuardReason=contextProof.value("reason").toString();
                        contextProof["exact_guard_reason"]=exactGuardReason;
                        contextProof["used"]=true;contextProof["reason"]="current_listing_regions_confirmed";
                        contextProof["evidence_basis"]=contextBasis;
                        contextProof["current_control_text_read"]=true;
                    }
                }
            }
            result["collection_page_context"]=contextProof;
        }
        OcrReply recognized;
        if(localHotpath) {
            recognized.ok=true;
            if(result.contains("collection_selection_preflight")) {
                auto gate=result["collection_selection_preflight"].toObject();
                gate["requires_original_full_validation"]=false;
                gate["requires_current_candidate_validation"]=true;
                gate["full_frame_ocr_replaced_by"]=contextBasis;
                result["collection_selection_preflight"]=gate;
            }
            recognized.observation=QJsonObject{{"width",ocrFrame.width},{"height",ocrFrame.height},
                {"frame_id",ocrFrame.frameId},{"words",QJsonArray{}},{"coverage","calibrated_listing_fields"},
                {"coordinate_space","client_physical_px"},{"language","zh-Hans-CN"}};
        } else recognized=recognizer.recognizeFrame(ocrFrame);
        if(!localHotpath &&recognized.ok) {
            // Our own status banner (top 4% band, click-through) is visible in
            // the frame: its words are never page, label or business text.
            const QRect overlay=statusOverlayRect(target);
            if(!overlay.isEmpty()) {
                const QRectF band(overlay.translated(-target.clientRect.topLeft()));
                QJsonArray kept;int dropped=0;
                for(const auto& value:recognized.observation["words"].toArray()) {
                    const auto word=value.toObject();
                    const QPointF centre(word["x"].toDouble()+word["width"].toDouble()/2,
                        word["y"].toDouble()+word["height"].toDouble()/2);
                    if(band.contains(centre))++dropped; else kept.append(value);
                }
                recognized.observation["words"]=kept;
                result["status_overlay"]=QJsonObject{{"present",true},{"full_frame_words_dropped",dropped},
                    {"client_rect",QJsonArray{band.x(),band.y(),band.width(),band.height()}}};
            }
        }
        result["full_frame_ocr_performed"]=!localHotpath;
        if(parser.isSet("collection-layout-profile")) result["collection_layout_profile"]=collectionLayoutProfile(ocrFrame);
        if(!localHotpath &&recognized.ok && parser.isSet("collection-observation") && ocrFrame.width==2560 && ocrFrame.height==1440){
            auto initial=recognized.observation;initial["coverage"]="full_client";
            auto checks=classifySkinPage(initial).anchorChecks;
            if(checks["listing.sale"].toBool() || (checks["listing.condition"].toBool() && checks["listing.similar"].toBool())){
                QJsonArray metadata;
                QList<QRect> labelRegions;
                if(!checks["listing.sale"].toBool())labelRegions.append(QRect(170,64,300,96));
                if(!checks["listing.default_sort"].toBool())labelRegions.append(QRect(1130,238,135,48));
                if(!checks["listing.page_number"].toBool() && !checks["listing.similar"].toBool())labelRegions.append(QRect(895,1226,190,66));
                for(const auto& region:labelRegions){
                    const auto reading=readCollectionLabel(ocrFrame.frameId,region,
                        [&](int scale,bool invert){return recognizer.recognizeRegion(ocrFrame,region,scale,invert);});
                    const auto& refined=reading.reply;
                    const bool replacement=refined.ok &&!refined.observation["words"].toArray().isEmpty();
                    metadata.append(QJsonObject{{"ok",refined.ok},{"error",refined.error},
                        {"bounds",QJsonArray{region.x(),region.y(),region.width(),region.height()}},
                        {"words",refined.observation["words"]},{"timing",refined.timing},
                        {"frame_id",ocrFrame.frameId},{"same_frame",true},{"attempts",reading.attempts},
                        {"replacement_applied",replacement},{"selection_policy","first_nonempty_fixed_preprocess_same_frame"}});
                    if(!replacement)continue;
                    QJsonArray merged;
                    for(const auto& v:recognized.observation["words"].toArray()){
                        const auto w=v.toObject();const QPointF center(w["x"].toDouble()+w["width"].toDouble()/2,w["y"].toDouble()+w["height"].toDouble()/2);
                        if(!QRectF(region).contains(center))merged.append(v);
                    }
                    for(const auto& w:refined.observation["words"].toArray())merged.append(w);
                    recognized.observation["words"]=merged;
                }
                result["collection_page_refinements"]=metadata;
            }
            // A selected white checkbox can hide its adjacent all-conditions
            // text from broad OCR. Re-read only the actual label pixels from
            // this captured frame; never infer an open modal from two anchors.
            auto filterObservation=recognized.observation;filterObservation["coverage"]="full_client";
            const QRect filterLabelRegion=listingFilterAllLabelRefinementRegion(filterObservation);
            if(!filterLabelRegion.isEmpty() &&ocrFrame.dpiX==144 &&ocrFrame.dpiY==144){
                const auto label=recognizer.recognizeRegion(ocrFrame,filterLabelRegion,3,true);
                const QString& sourceHash=ocrFrameSha;
                filterObservation["frame_id"]=ocrFrame.frameId;filterObservation["frame_sha256"]=sourceHash;
                auto local=label.observation;
                local["same_frame"]=label.ok &&local["frame_id"].toString()==ocrFrame.frameId;
                local["frame_sha256"]=sourceHash;
                QJsonObject evidence;
                const auto refined=refineListingFilterAllLabel(filterObservation,local,filterLabelRegion,&evidence);
                evidence["ocr_ok"]=label.ok;evidence["ocr_error"]=label.error;evidence["timing"]=label.timing;
                evidence["scale"]=3;evidence["provider"]="Windows.Media.Ocr";evidence["language"]="zh-Hans-CN";
                result["collection_listing_filter_refinement"]=evidence;
                if(evidence["replacement_applied"].toBool())recognized.observation=refined;
            }
        }
        if(!localHotpath &&recognized.ok && parser.isSet("ocr-ui-regions")) {
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
        // Navigation ROI repair must precede the circulation prerequisite.
        // Empty-watchlist navigation can lose one Chinese glyph or the sale
        // header in broad OCR (S11 run03/04). Try fixed SAME-frame crops only
        // when still Unknown; retain both original exact anchors. This never
        // runs during the calibrated per-item hot path or enables collection.
        if(!localHotpath &&recognized.ok &&parser.isSet("ocr-ui-regions")
            &&ocrFrame.width==2560 &&ocrFrame.height==1440 &&ocrFrame.dpiX==144 &&ocrFrame.dpiY==144){
            auto original=recognized.observation;original["coverage"]="full_client";
            original["frame_id"]=ocrFrame.frameId;original["frame_sha256"]=ocrFrameSha;
            const auto page=classifySkinPage(original);
            if(page.validInput &&page.page==SkinPage::Unknown &&page.candidates.isEmpty() &&page.overlay==PageOverlay::None){
                QJsonArray attempts;
                struct Variant{int scale;bool invert;};
                const Variant variants[]={{2,true},{1,false},{3,false}};
                for(const auto& variant:variants){
                    const auto sale=recognizer.recognizeRegion(ocrFrame,QRect(170,64,300,96),variant.scale,variant.invert);
                    const auto empty=recognizer.recognizeRegion(ocrFrame,QRect(1075,835,435,100),variant.scale,variant.invert);
                    auto a=sale.observation,b=empty.observation;
                    a["frame_sha256"]=ocrFrameSha;b["frame_sha256"]=ocrFrameSha;
                    a["same_frame"]=sale.ok;b["same_frame"]=empty.ok;
                    QJsonObject evidence;
                    const auto refined=refineEmptyWatchlistPage(original,a,b,&evidence);
                    evidence["scale"]=variant.scale;evidence["invert"]=variant.invert;
                    evidence["sale_words"]=a["words"];evidence["empty_words"]=b["words"];
                    evidence["sale_timing"]=sale.timing;evidence["empty_timing"]=empty.timing;
                    attempts.append(evidence);
                    if(!sale.ok ||!empty.ok){recognized.ok=false;recognized.error=!sale.ok?sale.error:empty.error;break;}
                    if(evidence["replacement_applied"].toBool()){recognized.observation=refined;break;}
                }
                result["empty_watchlist_refinement"]=attempts;
                if(recognized.ok &&classifySkinPage(QJsonObject(recognized.observation)).page==SkinPage::Unknown){
                    // The watchlist its refresh emptied: "暂无信息内容" + "我的关注：0/150".
                    QJsonArray emptied;
                    for(const auto& variant:variants){
                        const auto sale=recognizer.recognizeRegion(ocrFrame,QRect(170,64,300,96),variant.scale,variant.invert);
                        const auto content=recognizer.recognizeRegion(ocrFrame,EmptiedWatchlistContentRegion,variant.scale,variant.invert);
                        const auto count=recognizer.recognizeRegion(ocrFrame,WatchlistCountRegion,variant.scale,variant.invert);
                        auto a=sale.observation,b=content.observation,c=count.observation;
                        a["frame_sha256"]=ocrFrameSha;b["frame_sha256"]=ocrFrameSha;c["frame_sha256"]=ocrFrameSha;
                        a["same_frame"]=sale.ok;b["same_frame"]=content.ok;c["same_frame"]=count.ok;
                        QJsonObject evidence;
                        const auto refined=refineEmptiedWatchlistPage(original,a,b,c,&evidence);
                        evidence["scale"]=variant.scale;evidence["invert"]=variant.invert;
                        evidence["sale_words"]=a["words"];evidence["content_words"]=b["words"];evidence["count_words"]=c["words"];
                        emptied.append(evidence);
                        if(!sale.ok ||!content.ok ||!count.ok){
                            recognized.ok=false;recognized.error=!sale.ok?sale.error:!content.ok?content.error:count.error;break;
                        }
                        if(evidence["replacement_applied"].toBool()){recognized.observation=refined;break;}
                    }
                    result["emptied_watchlist_refinement"]=emptied;
                }
            }
        }
        // Navigation ROI repair must precede the circulation prerequisite.
        // run07 had two real repaired navigation anchors but the earlier
        // full-frame angle estimate had hidden circulation. Read these actual
        // pixels only after the repaired words are available to the classifier.
        if(!localHotpath &&recognized.ok &&parser.isSet("collection-observation") &&ocrFrame.dpiX==144 &&ocrFrame.dpiY==144){
            auto observed=recognized.observation;observed["coverage"]="full_client";
            const QRect region=homeCirculationRefinementRegion(observed);
            if(!region.isEmpty()){
                const auto refined=recognizer.recognizeRegion(ocrFrame,region,2,true);
                const bool replacement=refined.ok &&!refined.observation["words"].toArray().isEmpty();
                result["collection_home_refinement"]=QJsonObject{{"ok",refined.ok},{"error",refined.error},
                    {"bounds",QJsonArray{region.x(),region.y(),region.width(),region.height()}},
                    {"same_frame",true},{"replacement_applied",replacement},
                    {"stage","after_ui_anchor_refinement"},{"frame_id",ocrFrame.frameId},
                    {"words",refined.observation["words"]},{"timing",refined.timing}};
                if(replacement){
                    QJsonArray merged;
                    for(const auto& value:recognized.observation["words"].toArray()){
                        const auto word=value.toObject();const QPointF center(word["x"].toDouble()+word["width"].toDouble()/2,
                            word["y"].toDouble()+word["height"].toDouble()/2);
                        if(!QRectF(region).contains(center))merged.append(value);
                    }
                    for(const auto& word:refined.observation["words"].toArray())merged.append(word);
                    recognized.observation["words"]=merged;
                }
            }
        }
        result["recognition_performed"] = true;
        result["ocr_passed"] = recognized.ok;
        if (recognized.ok) {
            auto pageObservation = recognized.observation;
            SkinPageResult classified;
            if(localHotpath) {
                // This is explicitly a continuation, not fabricated full-frame
                // OCR. Candidate/title/price/wear/star/geometry below are new.
                classified.validInput=true;classified.page=SkinPage::SkinListings;
                classified.overlay=PageOverlay::None;
                classified.anchorChecks=contextPage["anchor_checks"].toObject();
                result["startup_page"]=contextPage;
            } else {
                pageObservation["coverage"] = "full_client";
                classified=classifySkinPage(pageObservation);
                result["startup_page"]=classified.toJson();
            }
            if(parser.isSet("purchase-observation")){
                pageObservation["frame_id"]=ocrFrame.frameId;
                pageObservation["frame_sha256"]=ocrFrameSha;
                result["purchase_observation"]=projectPurchaseObservation(pageObservation,result["startup_page"].toObject());
            }
            if(parser.isSet("collection-observation")){
                QJsonArray regions;
                QJsonArray productTitleWords;
                const QString& collectionFrameSha=ocrFrameSha;
                QJsonObject dynamicGeometry;
                if(dynamicLayout &&(classified.page==SkinPage::SkinListings
                    ||(parser.isSet("purchase-observation") &&classified.page==SkinPage::WatchlistListings))
                    &&classified.overlay==PageOverlay::None){
                    dynamicGeometry=selectionLayout.isEmpty()?detectCollectionLayout(ocrFrame,receiptReference):selectionLayout;
                    result["collection_layout"]=dynamicGeometry;
                    if(!dynamicGeometry["complete"].toBool() &&!result.contains("collection_layout_profile")){
                        // Preserve the failed detector's actual in-memory frame
                        // features, not a later recapture that may have healed.
                        // Successful observations omit this large diagnostic.
                        result["collection_layout_profile"]=collectionLayoutProfile(ocrFrame);
                        result["collection_layout_profile_reason"]="native_layout_incomplete_same_frame";
                    }
                }
                QList<QPair<QString,QRect>> requested;
                if(ocrFrame.width==2560 && ocrFrame.height==1440 && ocrFrame.dpiX==144 && ocrFrame.dpiY==144){
                    if(classified.page==SkinPage::CatalogFilter)requested.append({"season_options",QRect(746,474,370,420)});
                    if(classified.page==SkinPage::SkinHome){
                        requested.append({"catalog_names",QRect(110,320,468,1014)});
                        requested.append({"product_title",QRect(625,235,1375,72)});
                        requested.append({"product_title_latin",QRect(675,238,350,64)});
                    }
                    if(classified.page==SkinPage::SkinListings || classified.page==SkinPage::WatchlistListings){
                        if(classified.overlay==PageOverlay::ListingFilter)
                            requested.append({"condition_filter",QRect(1830,100,675,560)});
                        else {
                            requested.append({"listing_controls",QRect(110,230,1760,70)});
                            requested.append({"product_title",QRect(1915,238,530,54)});
                            requested.append({"selected_detail",QRect(1920,640,505,66)});
                            requested.append({"selected_detail_precise",QRect(2206,650,164,52)});
                            if(!dynamicLayout)requested.append({"first_card_fields",QRect(120+cardDx,525+cardDy,865,45)});
                        }
                    }
                }
                for(const auto& request:requested){
                    if(localHotpath &&request.first=="listing_controls") {
                        regions.append(contextControls);
                        continue;
                    }
                    if(localHotpath &&request.first=="selected_detail" &&!contextDetail.isEmpty()) {
                        const auto projected=projectCurrentCollectionRegion(contextDetail,ocrFrame.frameId,
                            collectionFrameSha,request.first,request.second);
                        if(!projected.isEmpty()) {regions.append(projected);continue;}
                    }
                    const bool latinTitle=request.first=="product_title_latin";
                    // The explicit local provider rereads the actual same-frame
                    // pixels independently. Keep a pending region, never native
                    // text that will immediately be discarded. The coordinator
                    // must still supply and validate all required title words.
                    const bool localText=parser.isSet("collection-local-text");
                    if(localText &&latinTitle)continue;
                    const bool localPending=localText &&(request.first=="product_title" ||request.first=="catalog_names");
                    const bool preciseDetail=request.first=="selected_detail_precise";
                    WindowsOcrRecognizer latinRecognizer(QCoreApplication::applicationDirPath()+QStringLiteral("/vision/windows_ocr_worker.ps1"),QStringLiteral("en-US"));
                    QRect requestedBounds=request.second;
                    QJsonArray titleAttempts;QJsonObject titleCropEvidence;int selectedTitleAttempt=0;
                    if(latinTitle){
                        int firstCjkX=requestedBounds.right()+1;QJsonObject firstCjkWord;
                        for(const auto& value:productTitleWords){
                            const auto word=value.toObject();const auto text=word["text"].toString().trimmed();
                            if(text.isEmpty() ||!word["x"].isDouble())continue;
                            const ushort code=text.front().unicode();
                            const bool startsWithCjk=(code>=0x3400 &&code<=0x4dbf)
                                ||(code>=0x4e00 &&code<=0x9fff) ||(code>=0xf900 &&code<=0xfaff);
                            const double measuredX=word["x"].toDouble();
                            if(!startsWithCjk ||!std::isfinite(measuredX)
                                ||measuredX<=requestedBounds.left()+18 ||measuredX>=firstCjkX)continue;
                            // The word must begin with CJK: an OCR box containing
                            // mixed Latin/CJK text does not locate its inner glyphs.
                            // Ignore the unrelated icon left of the prefix ROI.
                            firstCjkX=int(std::floor(measuredX));firstCjkWord=word;
                        }
                        if(!firstCjkWord.isEmpty())requestedBounds.setWidth(firstCjkX-requestedBounds.left()-2);
                        titleCropEvidence=QJsonObject{{"basis",firstCjkWord.isEmpty()?"historical_prefix_no_measured_cjk_edge":"same_frame_cjk_word_left_edge"},
                            {"original_bounds",QJsonArray{request.second.x(),request.second.y(),request.second.width(),request.second.height()}},
                            {"bounds",QJsonArray{requestedBounds.x(),requestedBounds.y(),requestedBounds.width(),requestedBounds.height()}},
                            {"cjk_word",firstCjkWord},{"right_gap_px",firstCjkWord.isEmpty()?0:2},
                            {"same_frame",true},{"frame_id",ocrFrame.frameId},{"frame_sha256",collectionFrameSha}};
                    }
                    OcrReply reply;
                    const auto preciseKey=ocrJobKey(OcrRegionJob{QStringLiteral("en-US"),requestedBounds,3,true,false});
                    if(localPending){reply.error="E_LOCAL_TEXT_PENDING";}
                    else if(preciseDetail &&prefetched.contains(preciseKey))reply=prefetched.take(preciseKey);
                    else reply=(latinTitle || preciseDetail)?latinRecognizer.recognizeRegion(ocrFrame,requestedBounds,preciseDetail?3:2,true)
                        :recognizer.recognizeRegion(ocrFrame,requestedBounds,2,true);
                    if(latinTitle){
                        const auto recordTitleAttempt=[&](int attempt,int scale,const OcrReply& read){
                            titleAttempts.append(QJsonObject{{"attempt",attempt},{"scale",scale},{"language","en-US"},
                                {"engine","Windows.Media.Ocr"},{"bounds",titleCropEvidence["bounds"]},
                                {"same_frame",true},{"frame_id",ocrFrame.frameId},{"frame_sha256",collectionFrameSha},
                                {"ok",read.ok},{"error",read.error},{"words",read.observation["words"]},{"timing",read.timing}});
                        };
                        const auto hasText=[](const OcrReply& read){
                            if(!read.ok)return false;
                            for(const auto& word:read.observation["words"].toArray())
                                if(!word.toObject()["text"].toString().trimmed().isEmpty())return true;
                            return false;
                        };
                        recordTitleAttempt(1,2,reply);selectedTitleAttempt=1;
                        const auto unscaled=latinRecognizer.recognizeRegion(ocrFrame,requestedBounds,1,true);
                        recordTitleAttempt(2,1,unscaled);
                        // Selection is independent of the requested product.
                        // Prefer 2x actual text, otherwise use the 1x actual text;
                        // retain both attempts verbatim, never complete a prefix.
                        if(!hasText(reply) &&unscaled.ok){reply=unscaled;selectedTitleAttempt=2;}
                    }
                    auto words=reply.observation["words"].toArray();
                    bool splitFields=false,splitOk=true;
                    if(request.first=="first_card_fields" && reply.ok && words.isEmpty()){
                        // Widely separated labels on a bright lower-row card
                        // may yield no line in the whole strip. Reread the two
                        // physical fields separately from the very same frame.
                        splitFields=true;
                        for(const auto& region:QList<QRect>{QRect(120+cardDx,525+cardDy,100,45),QRect(850+cardDx,525+cardDy,135,45)}){
                            const auto field=recognizer.recognizeRegion(ocrFrame,region,2,true);
                            splitOk=splitOk && field.ok;
                            for(const auto& word:field.observation["words"].toArray())words.append(word);
                        }
                    }
                    const bool truncated=words.size()>256;
                    while(words.size()>256)words.removeLast();
                    QJsonObject region{{"kind",request.first},{"ok",reply.ok && splitOk},{"error",reply.error},{"truncated",truncated},{"words",words},{"split_fields_refinement",splitFields}};
                    if(latinTitle){
                        region["attempts"]=titleAttempts;region["selected_attempt"]=selectedTitleAttempt;
                        region["selection_policy"]="first_nonempty_successful_reading_prefer_2x";
                        region["crop_evidence"]=titleCropEvidence;
                    }
                    regions.append(region);
                    if(request.first=="product_title"){
                        if(reply.ok &&!truncated)productTitleWords=words;
                        if(parser.isSet("collection-title-image")){
                            const auto box=request.second;
                            const QImage view(reinterpret_cast<const uchar*>(ocrFrame.pixels.constData()),ocrFrame.width,ocrFrame.height,ocrFrame.strideBytes,QImage::Format_ARGB32);
                            QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);
                            if(QRect(0,0,ocrFrame.width,ocrFrame.height).contains(box) &&view.copy(box).save(&buffer,"PNG"))
                                result["collection_title_image"]=QJsonObject{{"schema","collection-title-image-v1"},
                                    {"kind","product_title"},{"bounds",QJsonArray{box.x(),box.y(),box.width(),box.height()}},
                                    {"coordinate_space","client_physical_px"},{"raw_roi",true},
                                    {"frame_id",ocrFrame.frameId},{"frame_sha256",collectionFrameSha},{"same_frame",true},
                                    {"png_sha256",QString::fromLatin1(QCryptographicHash::hash(png,QCryptographicHash::Sha256).toHex())},
                                    {"png_base64",QString::fromLatin1(png.toBase64())},{"image_file_writes",0}};
                            else result["collection_title_image_error"]="E_COLLECTION_TITLE_IMAGE_ENCODING";
                        }
                    }
                    if(request.first=="catalog_names" &&parser.isSet("collection-catalog-image")){
                        // Use this request's actual same-frame crop, independent
                        // of its Windows OCR result; the local reader receives
                        // raw pixels, never repaired or completed label text.
                        const auto box=requestedBounds;
                        const QImage view(reinterpret_cast<const uchar*>(ocrFrame.pixels.constData()),ocrFrame.width,ocrFrame.height,ocrFrame.strideBytes,QImage::Format_ARGB32);
                        QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);
                        if(QRect(0,0,ocrFrame.width,ocrFrame.height).contains(box) &&view.copy(box).save(&buffer,"PNG"))
                            result["collection_catalog_image"]=QJsonObject{{"schema","collection-catalog-image-v1"},
                                {"kind","catalog_names"},{"bounds",QJsonArray{box.x(),box.y(),box.width(),box.height()}},
                                {"coordinate_space","client_physical_px"},{"raw_roi",true},
                                {"frame_id",ocrFrame.frameId},{"frame_sha256",collectionFrameSha},{"same_frame",true},
                                {"png_sha256",QString::fromLatin1(QCryptographicHash::hash(png,QCryptographicHash::Sha256).toHex())},
                                {"png_base64",QString::fromLatin1(png.toBase64())},{"image_file_writes",0}};
                        else result["collection_catalog_image_error"]="E_COLLECTION_CATALOG_IMAGE_ENCODING";
                    }
                }
                if(parser.isSet("collection-title-image") &&!result.contains("collection_title_image")
                    &&!result.contains("collection_title_image_error"))result["collection_title_image_error"]="E_COLLECTION_TITLE_REGION_UNAVAILABLE";
                if(parser.isSet("collection-catalog-image") &&!result.contains("collection_catalog_image")
                    &&!result.contains("collection_catalog_image_error"))result["collection_catalog_image_error"]="E_COLLECTION_CATALOG_REGION_UNAVAILABLE";
                const auto receiptProof=dynamicGeometry["selected_card_proof"].toObject();
                const bool receiptOnly=!dynamicGeometry["complete"].toBool() &&receiptProof["proven"].toBool();
                if(dynamicLayout &&(dynamicGeometry["complete"].toBool() ||receiptOnly)){
                    const auto asRect=[](const QJsonValue& value){const auto a=value.toArray();return QRect(a[0].toInt(),a[1].toInt(),a[2].toInt(),a[3].toInt());};
                    const auto cards=receiptOnly?QJsonArray{receiptProof["card"]}:dynamicGeometry["cards"].toArray();
                    QString requestedId=parser.value("collection-card-id");
                    if(parser.isSet("collection-visible-index")){
                        if(visibleIndex<cards.size())requestedId=cards[visibleIndex].toObject()["id"].toString();
                        else result["collection_requested_card_error"]="E_COLLECTION_VISIBLE_INDEX";
                    }
                    bool requestedFound=requestedId.isEmpty();
                    WindowsOcrRecognizer latinRecognizer(QCoreApplication::applicationDirPath()+QStringLiteral("/vision/windows_ocr_worker.ps1"),QStringLiteral("en-US"));
                    for(const auto& value:cards){
                        const auto card=value.toObject();const auto id=card["id"].toString();
                        if(id==requestedId)requestedFound=true;
                        // Geometry is enumerated for every card. Business
                        // fields are needed only after this actual selection.
                        if(card["fields_bounds"].isNull() ||!card["selected"].toBool())continue;
                        const bool localPrice=parser.isSet("collection-local-price");
                        PreparedNumericRegion preparedPrice;
                        if(localPrice)preparedPrice=prepareNumericRegion(ocrFrame,asRect(card["price_bounds"]),3);
                        if(parser.isSet("collection-price-image")){
                            const auto box=asRect(card["price_bounds"]);
                            const QImage view(reinterpret_cast<const uchar*>(ocrFrame.pixels.constData()),ocrFrame.width,ocrFrame.height,ocrFrame.strideBytes,QImage::Format_ARGB32);
                            QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);
                            if(QRect(0,0,ocrFrame.width,ocrFrame.height).contains(box) &&view.copy(box).save(&buffer,"PNG")){
                                result["collection_price_image"]=QJsonObject{{"schema","collection-price-image-v1"},{"bounds",card["price_bounds"]},
                                    {"kind","price"},{"coordinate_space","client_physical_px"},{"raw_roi",true},
                                    {"card_id",id},{"frame_id",ocrFrame.frameId},{"frame_sha256",dynamicGeometry["frame_sha256"]},
                                    {"same_frame",true},{"png_sha256",QString::fromLatin1(QCryptographicHash::hash(png,QCryptographicHash::Sha256).toHex())},
                                    {"png_base64",QString::fromLatin1(png.toBase64())},{"image_file_writes",0}};
                                if(localPrice){
                                    auto envelope=result["collection_price_image"].toObject();
                                    envelope["numeric_preprocess"]=preparedPrice.metadata;
                                    envelope["numeric_preprocess_ok"]=preparedPrice.ok;
                                    envelope["numeric_preprocess_error"]=preparedPrice.error;
                                    result["collection_price_image"]=envelope;
                                }
                            }else result["collection_price_image_error"]="E_COLLECTION_PRICE_IMAGE_ENCODING";
                        }
                        QJsonArray words,fieldAttempts;bool ok=true;QString regionError;
                        for(const auto& field:QStringList{"condition_bounds","price_bounds"}){
                            const bool price=field=="price_bounds";
                            const bool numericPrice=price &&parser.isSet("collection-numeric-price");
                            if(price &&localPrice){
                                // The provider is fixed before the run. Do not
                                // spend three Windows OCR attempts then choose
                                // a model according to the configured price.
                                const auto pendingError=preparedPrice.ok?QStringLiteral("E_LOCAL_PRICE_PENDING"):preparedPrice.error;
                                fieldAttempts.append(QJsonObject{{"field",field},{"attempt",0},{"scale",3},
                                    {"engine","local_price_pending"},{"bounds",card[field]},{"same_frame",true},
                                    {"frame_id",ocrFrame.frameId},{"ok",false},{"error",pendingError},
                                    {"numeric_preprocess",preparedPrice.metadata}});
                                ok=false;regionError=pendingError;
                                continue;
                            }
                            const auto conditionKey=ocrJobKey(OcrRegionJob{QStringLiteral("zh-Hans-CN"),asRect(card[field]),2,true,false});
                            auto reply=price?(numericPrice?latinRecognizer.recognizeNumericRegion(ocrFrame,asRect(card[field]),3)
                                :latinRecognizer.recognizeRegion(ocrFrame,asRect(card[field]),3,true))
                                :prefetched.contains(conditionKey)?prefetched.take(conditionKey)
                                :recognizer.recognizeRegion(ocrFrame,asRect(card[field]),2,true);
                            QJsonArray conditionRefinements;
                            QRect recognizedBounds=asRect(card[field]);
                            if(!price &&reply.ok) {
                                const auto refinement=collectionConditionRefinementRegion(
                                    reply.observation["words"].toArray(),recognizedBounds);
                                if(!refinement.isEmpty()) {
                                    const auto saveCondition=[&](const OcrReply& read,const QRect& bounds){
                                        conditionRefinements.append(QJsonObject{{"bounds",QJsonArray{bounds.x(),bounds.y(),bounds.width(),bounds.height()}},
                                            {"words",read.observation["words"]},{"ok",read.ok},{"error",read.error},
                                            {"frame_id",ocrFrame.frameId},{"frame_sha256",collectionFrameSha},
                                            {"same_frame",read.observation["frame_id"]==ocrFrame.frameId},
                                            {"timing",read.timing},{"scale",2},{"provider","Windows.Media.Ocr"},
                                            {"literal_condition",collectionConditionWordsAreLiteral(read.observation["words"].toArray())}});
                                    };
                                    saveCondition(reply,recognizedBounds);
                                    reply=recognizer.recognizeRegion(ocrFrame,refinement,2,true);
                                    recognizedBounds=refinement;saveCondition(reply,recognizedBounds);
                                    if(reply.ok &&(reply.observation["frame_id"]!=ocrFrame.frameId
                                        ||!collectionConditionWordsAreLiteral(reply.observation["words"].toArray()))) {
                                        reply.ok=false;reply.error="E_COLLECTION_CONDITION_REFINEMENT";
                                    }
                                }
                            }
                            const auto recordAttempt=[&](int attempt,int scale,const QString& language,const OcrReply& read){
                                QJsonArray tokens;for(const auto& word:read.observation["words"].toArray()){
                                    if(tokens.size()==16)break;
                                    tokens.append(word.toObject()["text"].toString().left(64));
                                }
                                QJsonObject recorded{{"field",field},{"attempt",attempt},{"scale",scale},
                                    {"engine","Windows.Media.Ocr"},{"language",language},{"tokens",tokens},
                                    {"bounds",card[field]},{"same_frame",true},{"frame_id",ocrFrame.frameId},
                                    {"ok",read.ok},{"error",read.error},{"word_count",read.observation["words"].toArray().size()},
                                    {"numeric_preprocess",read.observation["numeric_preprocess"]},{"timing",read.timing}};
                                if(!conditionRefinements.isEmpty()) {
                                    recorded["condition_refinement_attempts"]=conditionRefinements;
                                    recorded["recognized_bounds"]=QJsonArray{recognizedBounds.x(),recognizedBounds.y(),recognizedBounds.width(),recognizedBounds.height()};
                                    recorded["selected_attempt"]=conditionRefinements.size();
                                    recorded["refinement_basis"]="same_frame_detached_thin_left_border";
                                }
                                fieldAttempts.append(recorded);
                            };
                            recordAttempt(1,price?3:2,price?QStringLiteral("en-US"):QStringLiteral("zh-Hans-CN"),reply);
                            if(price &&(numericPrice?!reply.ok ||!collectionPriceWordsAreNumeric(reply.observation["words"].toArray()):reply.ok &&!collectionPriceWordsAreNumeric(reply.observation["words"].toArray()))){
                                // Both empty text and non-numeric readings
                                // (actual run04: "goo") request new OCR of the
                                // SAME pixels. Never repair letters into digits.
                                reply=numericPrice?latinRecognizer.recognizeNumericRegion(ocrFrame,asRect(card[field]),2):latinRecognizer.recognizeRegion(ocrFrame,asRect(card[field]),2,true);
                                recordAttempt(2,2,QStringLiteral("en-US"),reply);
                                if((numericPrice &&!reply.ok) ||(reply.ok &&!collectionPriceWordsAreNumeric(reply.observation["words"].toArray()))){
                                    reply=numericPrice?recognizer.recognizeNumericRegion(ocrFrame,asRect(card[field]),2):recognizer.recognizeRegion(ocrFrame,asRect(card[field]),2,true);
                                    recordAttempt(3,2,QStringLiteral("zh-Hans-CN"),reply);
                                }
                            }
                            ok=ok &&reply.ok;if(!reply.ok)regionError=reply.error;
                            for(const auto& word:reply.observation["words"].toArray())words.append(word);
                        }
                        const bool truncated=words.size()>256;while(words.size()>256)words.removeLast();
                        regions.append(QJsonObject{{"kind","card_fields"},{"card_id",id},{"bounds",card["fields_bounds"]},
                            {"ok",ok},{"error",regionError},{"truncated",truncated},{"words",words},{"split_fields_refinement",true},
                            {"field_attempts",fieldAttempts},{"scope",receiptOnly?"receipt_only":"visible_layout"}});
                    }
                    if(!requestedId.isEmpty())result["collection_requested_card_id"]=requestedId;
                    if(!requestedFound)result["collection_requested_card_error"]="E_COLLECTION_CARD_ID";
                }
                result["collection_observation"]=QJsonObject{{"page",toString(classified.page)},{"regions",regions},
                    {"frame_id",ocrFrame.frameId},{"frame_sha256",collectionFrameSha},
                    {"coverage","field_projection"},{"same_frame",true},{"image_file_writes",0}};
                if((classified.page==SkinPage::SkinListings
                    ||(parser.isSet("purchase-observation") &&classified.page==SkinPage::WatchlistListings)) && classified.overlay==PageOverlay::None
                    && ocrFrame.width==2560 && ocrFrame.height==1440 && ocrFrame.strideBytes==2560*4){
                    const auto mean=[&](const QRect& box){double sum=0;
                        for(int y=box.top();y<=box.bottom();++y)for(int x=box.left();x<=box.right();++x){
                            const auto* p=reinterpret_cast<const unsigned char*>(ocrFrame.pixels.constData()+qint64(y)*ocrFrame.strideBytes+x*4);
                            sum+=(int(p[0])+int(p[1])+int(p[2]))/3.0;
                        }return sum/(box.width()*box.height());};
                    QJsonArray borders;
                    if(!dynamicLayout)for(const auto& box:QList<QRect>{QRect(400+cardDx,303+cardDy,400,3),QRect(120+cardDx,350+cardDy,3,160),QRect(982+cardDx,350+cardDy,3,160),QRect(400+cardDx,566+cardDy,400,3)})borders.append(mean(box));
                    bool selected=true;for(const auto& v:borders)selected=selected && v.toDouble()>90;
                    int warm=0,bright=0,total=0;
                    for(int y=303;y<337;++y)for(int x=2320;x<2356;++x){const auto* p=reinterpret_cast<const unsigned char*>(ocrFrame.pixels.constData()+qint64(y)*ocrFrame.strideBytes+x*4);
                        ++total;warm+=int(p[2])>110 && int(p[2])-int(p[1])>15 && int(p[1])-int(p[0])>15;bright+=p[0]>140 && p[1]>140 && p[2]>140;}
                    if(!dynamicLayout)result["collection_selected_card"]=QJsonObject{{"index",cardIndex},{"selected",selected},{"border_means",borders},
                        {"favorite_warm_fraction",double(warm)/total},{"favorite_bright_fraction",double(bright)/total}};
                    else {
                        QJsonObject packet{{"selected",false},{"same_frame",true},{"frame_id",ocrFrame.frameId},
                            {"frame_sha256",dynamicGeometry["frame_sha256"]},{"favorite_warm_fraction",double(warm)/total},
                            {"favorite_bright_fraction",double(bright)/total}};
                        if(dynamicGeometry["complete"].toBool())for(const auto& value:dynamicGeometry["cards"].toArray()){
                            const auto card=value.toObject();if(!card["selected"].toBool())continue;
                            packet["card_id"]=card["id"];packet["selected"]=true;packet["bounds"]=card["bounds"];packet["fields_bounds"]=card["fields_bounds"];
                        }
                        else if(receiptOnly){
                            const auto card=receiptProof["card"].toObject();
                            packet["card_id"]=card["id"];packet["selected"]=card["selected"];packet["bounds"]=card["bounds"];packet["fields_bounds"]=card["fields_bounds"];
                            packet["scope"]="receipt_only";packet["unique_selection_proven"]=false;packet["actions_enabled"]=false;
                        }
                        result["collection_selected_card"]=packet;
                    }
                }
                if(classified.overlay==PageOverlay::ListingFilter && ocrFrame.width==2560 && ocrFrame.height==1440){
                    QJsonObject states;
                    const QStringList names{"all","S","A","B","C"};
                    const QPoint starts[]={{1843,446},{2169,446},{1843,521},{2169,521},{1843,595}};
                    for(int i=0;i<names.size();++i){const auto measured=measureFilterCheckbox(ocrFrame,QRect(starts[i],QSize(36,36)),names[i]);
                        states[names[i]]=QJsonObject{{"state",toString(measured.state)},{"measurement",measured.measurement}};
                    }
                    result["collection_condition_filter"]=states;
                }
            }
            if (parser.isSet("catalog-filter-state")) {
                if (classified.page == SkinPage::CatalogFilter && classified.overlay == PageOverlay::None
                    && ocrFrame.width == 2560 && ocrFrame.height == 1440 && ocrFrame.dpiX == 144 && ocrFrame.dpiY == 144) {
                    const QRect region(510,410,1390,435);
                    const auto labels=recognizer.recognizeRegion(ocrFrame,region,2,true);
                    result["catalog_filter_roi_ok"]=labels.ok;
                    result["catalog_filter_roi_error"]=labels.error;
                    // Never fall back from a failed refinement to apparently
                    // complete field values. Page evidence is kept separately.
                    QJsonArray words;
                    for(const auto& value:pageObservation["words"].toArray()){
                        const auto w=value.toObject();
                        if(!QRectF(region).contains(QPointF(w["x"].toDouble()+w["width"].toDouble()/2,
                            w["y"].toDouble()+w["height"].toDouble()/2))) words.append(value);
                    }
                    if(labels.ok) for(const auto& w:labels.observation["words"].toArray())words.append(w);
                    pageObservation["words"]=words;
                }
                pageObservation["frame_id"]=ocrFrame.frameId;
                pageObservation["frame_sha256"]=ocrFrameSha;
                auto filter=readCatalogFilter(ocrFrame,pageObservation);
                const QRect seasonRegion=catalogSeasonLabelRefinementRegion(ocrFrame,pageObservation);
                if(!seasonRegion.isEmpty()){
                    const auto season=recognizer.recognizeRegion(ocrFrame,seasonRegion,3,true);
                    auto local=season.observation;
                    local["same_frame"]=season.ok &&local["frame_id"].toString()==ocrFrame.frameId;
                    local["frame_sha256"]=pageObservation["frame_sha256"];
                    QJsonObject evidence;
                    const auto refined=refineCatalogSeasonLabel(ocrFrame,pageObservation,local,seasonRegion,&evidence);
                    evidence["ocr_ok"]=season.ok;evidence["ocr_error"]=season.error;evidence["timing"]=season.timing;
                    evidence["scale"]=3;evidence["provider"]="Windows.Media.Ocr";evidence["language"]="zh-Hans-CN";
                    result["catalog_filter_season_refinement"]=evidence;
                    if(evidence["replacement_applied"].toBool()){
                        pageObservation=refined;filter=readCatalogFilter(ocrFrame,pageObservation);
                    }
                }
                QJsonArray fieldRefinements;
                // A bright selected square can disrupt the adjacent Chinese
                // label in broad OCR. Retry only affected label boxes from the
                // very same pixels; never substitute a guessed character.
                for(const auto& id:QStringList{"owned","unowned","legendary","epic","rare","common"}){
                    const auto box=filter.boxes.value(id);
                    if(box.reason!="E_FILTER_LABEL")continue;
                    const QRect roi(box.bounds.right()+12,box.bounds.top()-2,150,40);
                    const auto refined=recognizer.recognizeRegion(ocrFrame,roi,3,true);
                    QJsonArray fieldWords;
                    for(const auto& w:refined.observation["words"].toArray()){
                        if(fieldWords.size()>=16)break;
                        fieldWords.append(w.toObject()["text"].toString().left(64));
                    }
                    fieldRefinements.append(QJsonObject{{"field",id},{"ok",refined.ok},{"error",refined.error},{"label_words",fieldWords}});
                    if(!refined.ok)continue;
                    QJsonArray words;
                    for(const auto& value:pageObservation["words"].toArray()){
                        const auto w=value.toObject();
                        if(!QRectF(roi).contains(QPointF(w["x"].toDouble()+w["width"].toDouble()/2,
                            w["y"].toDouble()+w["height"].toDouble()/2)))words.append(value);
                    }
                    for(const auto& value:refined.observation["words"].toArray())words.append(value);
                    pageObservation["words"]=words;
                    filter=readCatalogFilter(ocrFrame,pageObservation);
                }
                result["catalog_filter_label_refinements"]=fieldRefinements;
                result["catalog_filter_state"]=filter.toJson();
            }
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
            summary["frame_sha256"] = ocrFrameSha;
            summary["coverage"]=localHotpath?"calibrated_listing_fields":"full_client";
            summary["full_frame_ocr_performed"]=!localHotpath;
            result["ocr"] = summary;
        } else {
            error = recognized.error;
            result["ocr_error"] = error;
        }
        // One hidden helper is shared by all language/ROI recognizers. In
        // server mode its owner spans requests, but image mappings never do.
        result["ocr_session"] = captureRequestOcrMetrics(sessionBefore, recognizer.sessionMetrics());
        if(!localHotpath)result["ocr_full_frame_timing"] = recognized.timing;
        if(pageContext &&!localHotpath) {
            const bool remembered=error.isEmpty() &&pageContext->remember(ocrFrame,target,result);
            if(!remembered)pageContext->clear();
            result["collection_page_context_calibrated"]=remembered;
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
    if (result.contains("ocr")) {
        auto summary = result["ocr"].toObject();
        summary["frame_age_before_output_ms"] = captureClockMs() - ocrFrame.sourceMonoMs;
        result["ocr"] = summary;
    }
    const bool focusFinished = focus.finish();
    const bool finalRestored = foregroundWindow() == returnHwnd;
    result["ide_foreground_restored"] = finalRestored;
    result["return_window_bound"] = returnWindowBound;
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
    if(pageContext &&(!error.isEmpty() ||!focusFinished))pageContext->clear();
    if(streaming &&frameStream) {
        auto metrics=frameStream->metrics();
        metrics["produced_during_native_recognition"]=std::max(0,metrics["produced"].toInt()-streamProducedBeforeOcr);
        metrics["producer_lifetime"]="foreground_collection_worker_session";
        result["capture_stream"]=metrics;
    }
    return {result, !focusFinished && !callerOwned ? 3 : error.isEmpty() ? 0 : 1};
}
} // namespace

int runLiveCaptureCheck(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    WindowsOcrRecognizer recognizer(QCoreApplication::applicationDirPath() + QStringLiteral("/vision/windows_ocr_worker.ps1"));
    const auto captured = capture(app.arguments(), recognizer);
    if (captured.packet.contains("help")) {
        const auto help = captured.packet.value("help").toString().toUtf8();
        std::fwrite(help.constData(), 1, size_t(help.size()), stdout);
        std::fflush(stdout);
        return captured.status;
    }
    return emitResult(captured.packet, captured.status);
}

int runLiveCaptureServer(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments() != QStringList{app.arguments().first(), QStringLiteral("--live-capture-server")})
        return emitResult(captureServiceError(QStringLiteral("E_CAPTURE_SERVER_STARTUP_ARGUMENTS")), 2);
#ifdef Q_OS_WIN
    // stdin/stdout are pipes owned by the caller. Binary mode makes the byte
    // ceiling and JSONL delimiter independent of CRT text transformations.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // Its hidden helper is started lazily on the first valid OCR request.
    // Destruction drains the shared session on normal EOF/error exit. Native
    // Leases remain request-local. The opt-in producer retains at most one
    // pending frame; EOF destroys it and joins before GPU resources release.
    WindowsOcrRecognizer recognizer(QCoreApplication::applicationDirPath() + QStringLiteral("/vision/windows_ocr_worker.ps1"));
#ifdef Q_OS_WIN
    // Per-process 1 ms timer resolution for the bounded native waits; the
    // default ~15.6 ms quantum otherwise dominates per-frame wake-ups.
    timeBeginPeriod(1);
#endif
    // Three helpers plus the request thread recognize one frame's listing
    // ROIs concurrently. They start hidden in the background right now.
    WindowsOcrPool ocrPool(QCoreApplication::applicationDirPath() + QStringLiteral("/vision/windows_ocr_worker.ps1"), 3);
    const auto captureResources=std::make_shared<DxgiCaptureResources>();
    CollectionPageContext pageContext;
    DxgiFrameStream frameStream;
    quint64 sequence = 0;
    while (true) {
        QByteArray line;
        bool oversized = false;
        int value = EOF;
        while ((value = std::fgetc(stdin)) != EOF && value != '\n') {
            if (line.size() == CaptureRequestMaximumBytes) { oversized = true; break; }
            line.append(char(value));
        }
        if (oversized) {
            emitResult(QJsonObject{{"id", QJsonValue::Null}, {"exit_status", 2},
                {"result", captureServiceError(QStringLiteral("E_CAPTURE_SERVER_LINE_LIMIT"))}}, 2);
            return 2; // Do not drain an unbounded peer-controlled line.
        }
        if (value == EOF && line.isEmpty()) return std::ferror(stdin) ? 2 : 0;
        if (line.endsWith('\r')) line.chop(1);
        const auto request = parseCaptureServiceRequest(line);
        CaptureResult captured;
        if (!request.error.isEmpty()) {frameStream.stop();pageContext.clear();captured = {captureServiceError(request.error), 2};}
        else {
            try { captured = capture(request.arguments, recognizer, captureResources, &pageContext,&frameStream,&ocrPool); }
            catch (...) {frameStream.stop();pageContext.clear();captured = {captureServiceError(QStringLiteral("E_CAPTURE_SERVER_EXCEPTION")), 1}; }
            // A scroll that is not yet stable observed no page and needs no
            // new capture owner: the coordinator reads the next frames again.
            const bool stillSettling=captured.packet["error"]=="E_COLLECTION_SCROLL_NOT_STABLE"
                &&captured.packet["target_foreground_retained"]==true;
            if(captured.status!=0 &&!stillSettling){frameStream.stop();pageContext.clear();}
            captured.packet["capture_server"] = QJsonObject{{"protocol", "live-capture-server-v1"},
                {"pid", QString::number(QCoreApplication::applicationPid())},
                {"request_sequence", double(++sequence)}, {"fresh_capture_per_request", true},
                {"ocr_owner_reused", sequence > 1}};
        }
        emitResult(QJsonObject{{"id", request.id}, {"exit_status", captured.status}, {"result", captured.packet}}, 0);
        if (std::ferror(stdout)) return 2;
        if (value == EOF) return std::ferror(stdin) ? 2 : 0;
    }
}
} // namespace relink::diagnostics
