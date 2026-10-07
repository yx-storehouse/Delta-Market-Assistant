#include "startup_observer_self_test.h"
#include "application/runtime/startup_observer.h"
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <cstdio>

namespace relink::diagnostics {
int runStartupObserverSelfTest(const QString& fixturePath) {
    QJsonObject report{{"diagnostic", "startup_observer_self_test"}, {"game_connected", false},
        {"system_input_sent", false}, {"frame_capture_count", 0}, {"image_file_writes", 0}};
    int failed = 0, pageChecks = 0, traceChecks = 0;
    QFile file(fixturePath);
    QHash<QString, QJsonObject> pages;
    QJsonArray checks;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1048576) {
        report["error"] = "E_STARTUP_FIXTURE_FILE"; ++failed;
    } else {
        const auto root = QJsonDocument::fromJson(file.readAll()).object();
        const auto cases = root.value("cases").toArray();
        if (root.value("schema_version").toInt() != 1 || cases.isEmpty() || cases.size() > 128) {
            report["error"] = "E_STARTUP_FIXTURE_SCHEMA"; ++failed;
        } else for (const auto& value : cases) {
            const auto item = value.toObject();
            const auto id = item.value("id").toString();
            const auto ocr = item.value("observation").toObject();
            const auto result = vision::classifySkinPage(ocr);
            const bool pass = !id.isEmpty() && !pages.contains(id) && result.validInput
                && vision::toString(result.page) == item.value("expected_page").toString()
                && vision::toString(result.overlay) == item.value("expected_modal").toString();
            pages.insert(id, ocr); ++pageChecks; if (!pass) ++failed;
            checks.append(QJsonObject{{"id", id}, {"passed", pass}, {"page", vision::toString(result.page)}, {"reason", result.reason}});
        }
    }
    const auto trace = [&](const QString& name, const QStringList& sequence, runtime::StartupPhase expected) {
        runtime::StartupObserver observer;
        runtime::RuntimeContext context{name, name, QStringLiteral("fixture:clock"), QStringLiteral("startup"), 0, 1};
        bool ok = observer.start(context, 0);
        int index = 0;
        for (const auto& key : sequence) {
            ++index;
            const qint64 now = index * 100;
            runtime::StartupPageObservation page{QStringLiteral("%1:%2").arg(name).arg(index), context, now - 1, now, pages.value(key)};
            ok = pages.contains(key) && observer.observe(page, now) && ok;
        }
        ok = ok && observer.snapshot().phase == expected && !observer.snapshot().toJson().value("actions_enabled").toBool();
        ++traceChecks; if (!ok) ++failed;
        checks.append(QJsonObject{{"id", name}, {"passed", ok}, {"snapshot", observer.snapshot().toJson()}});
    };
    if (!pages.isEmpty()) {
        trace(QStringLiteral("original_empty_watchlist_route"), {"p1_lobby", "p1_warehouse", "p1_mandel", "p1_skin_home",
            "p1_transition", "p1_empty_watchlist_1", "p1_empty_watchlist_2", "p1_home_after_empty", "p1_catalog_filter"}, runtime::StartupPhase::FilterObserved);
        trace(QStringLiteral("existing_list_route"), {"p1_watch_list"}, runtime::StartupPhase::ExistingListObserved);
        trace(QStringLiteral("ordinary_existing_skin_list_route"), {"p1_skin_list"}, runtime::StartupPhase::ExistingListObserved);
        trace(QStringLiteral("filter_without_precheck_does_not_advance"), {"p1_catalog_filter"}, runtime::StartupPhase::Locating);
        trace(QStringLiteral("listing_overlay_does_not_advance"), {"p3_list_filter_open"}, runtime::StartupPhase::Locating);
    }
    int liveProjectionChecks = 0;
    QFile liveFixture(QStringLiteral(":/fixtures/lobby_live_20261007.json"));
    bool liveProjectionPass = liveFixture.open(QIODevice::ReadOnly) && liveFixture.size() <= 65536;
    if (liveProjectionPass) {
        const auto live = QJsonDocument::fromJson(liveFixture.readAll()).object();
        const auto actual = vision::classifySkinPage(live.value("observation").toObject());
        liveProjectionPass = live.value("source_kind") == "live_ocr_anchor_projection"
            && !live.value("pixels_included").toBool() && actual.validInput
            && actual.page == vision::SkinPage::Lobby && actual.overlay == vision::PageOverlay::None
            && actual.calibrationEvidence == "lobby_live_sample_20261007";
        ++liveProjectionChecks;
    }
    if (!liveProjectionPass) ++failed;
    checks.append(QJsonObject{{"id", "recorded_lobby_anchor_projection"}, {"passed", liveProjectionPass},
        {"live_capture_performed", false}});
    report["live_projection_checks"] = liveProjectionChecks;
    int marketChecks=0;
    QFile marketFile(QStringLiteral(":/fixtures/market_live_20261007.json"));
    if(!marketFile.open(QIODevice::ReadOnly) || marketFile.size()>1048576){++failed;}
    else {
        const auto market=QJsonDocument::fromJson(marketFile.readAll()).object();
        const auto samples=market.value("cases").toArray();
        if(samples.size()!=6 || market.value("source_kind")!="live_ocr_anchor_projection" || market.value("pixels_included").toBool())++failed;
        for(const auto& value:samples){const auto sample=value.toObject();const auto ocr=sample.value("observation").toObject();
            const auto actual=vision::classifySkinPage(ocr);
            const bool ok=actual.validInput && vision::toString(actual.page)==sample.value("expected_page").toString();
            if(!ok)++failed;++marketChecks;pages.insert("live:"+sample.value("id").toString(),ocr);
            checks.append(QJsonObject{{"id","live:"+sample.value("id").toString()},{"passed",ok},{"live_capture_performed",false}});
        }
        trace(QStringLiteral("recorded_live_empty_watchlist_route"),{"live:skin_home","live:empty_1","live:empty_2","live:home_after_empty","live:catalog_filter"},runtime::StartupPhase::FilterObserved);
    }
    report["market_projection_checks"]=marketChecks;
    report["page_checks"] = pageChecks; report["trace_checks"] = traceChecks;
    report["checks"] = checks; report["failures"] = failed; report["passed"] = failed == 0;
    report["live_calibrated"] = false;
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout); std::fputc('\n', stdout);
    std::printf("STARTUP_OBSERVER_SELF_TEST=%s; page_checks=%d; trace_checks=%d; failures=%d; game_connected=false; system_input_sent=false\n",
        failed == 0 ? "PASS" : "FAIL", pageChecks, traceChecks, failed);
    std::fflush(stdout);
    return failed == 0 ? 0 : 1;
}
} // namespace relink::diagnostics
