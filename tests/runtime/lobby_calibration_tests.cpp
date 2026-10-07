#include "application/vision/skin_page_classifier.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <iostream>
#include <functional>

using namespace relink::vision;
namespace {
int assertions = 0, failures = 0;
void check(bool ok, const char* name) {
    ++assertions; failures += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
}
QJsonObject mutateWords(QJsonObject page, const std::function<void(QJsonArray&)>& mutate) {
    auto words = page.value("words").toArray(); mutate(words); page["words"] = words; return page;
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QFile file(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString());
    check(file.open(QIODevice::ReadOnly), "live_projection_file_open");
    const auto root = QJsonDocument::fromJson(file.readAll()).object();
    const auto page = root.value("observation").toObject();
    check(!root.value("pixels_included").toBool() && root.value("source_kind") == "live_ocr_anchor_projection",
        "fixture_is_text_projection_not_live_capture_test");
    check(page.value("words").toArray().size() == 10, "ten_nonpersonal_button_tokens");
    const auto result = classifySkinPage(page);
    check(result.validInput && result.page == SkinPage::Lobby && result.overlay == PageOverlay::None, "observed_windows_ocr_variant_is_lobby");
    check(result.calibrationEvidence == "lobby_live_sample_20261007", "sample_evidence_explicit");
    check(!result.toJson().value("actions_enabled").toBool() && !result.toJson().value("live_calibrated").toBool(), "single_sample_not_global_certification");
    for (double factor : {.5, .75, 1.5, 2.0}) {
        auto scaled = mutateWords(page, [=](QJsonArray& words) {
            for (int i = 0; i < words.size(); ++i) { auto word = words[i].toObject();
                for (const auto* key : {"x", "y", "width", "height"}) word[key] = word.value(key).toDouble()*factor;
                words[i] = word;
            }
        });
        scaled["width"] = page.value("width").toDouble()*factor;
        scaled["height"] = page.value("height").toDouble()*factor;
        check(classifySkinPage(scaled).page == SkinPage::Lobby, "proportional_scaling_preserves_scoped_match");
    }
    for (int i = 0; i < 10; ++i) {
        const auto missing = mutateWords(page, [=](auto& words) { words.removeAt(i); });
        check(classifySkinPage(missing).page == SkinPage::Unknown, "every_observed_anchor_character_required");
    }
    for (int index : {0, 4, 6}) {
        const auto moved = mutateWords(page, [=](auto& words) { auto w = words[index].toObject(); w["y"] = 600; words[index] = w; });
        check(classifySkinPage(moved).page == SkinPage::Unknown, "wrong_region_cannot_supply_anchor");
    }
    for (const auto* field : {"language", "protocol"}) {
        auto missing = page; missing.remove(field);
        check(classifySkinPage(missing).page == SkinPage::Unknown, "variant_requires_observed_provider");
    }
    auto wrongLanguage = page; wrongLanguage["language"] = "en-US";
    check(classifySkinPage(wrongLanguage).page == SkinPage::Unknown, "variant_is_not_global_ocr_alias");
    auto unrelated = mutateWords(page, [](auto& words) { auto w = words[3].toObject(); w["text"] = QStringLiteral("砚"); words[3] = w; });
    check(classifySkinPage(unrelated).page == SkinPage::Unknown, "arbitrary_one_character_errors_not_accepted");
    auto exact = mutateWords(page, [](auto& words) { auto w = words[3].toObject(); w["text"] = QStringLiteral("戏"); words[3] = w; });
    check(classifySkinPage(exact).page == SkinPage::Lobby && classifySkinPage(exact).calibrationEvidence == "historical_ocr_only", "original_exact_rule_preserved");
    auto low = mutateWords(page, [](auto& words) { auto w = words[3].toObject(); w["score"] = .69; words[3] = w; });
    check(classifySkinPage(low).page == SkinPage::Unknown, "variant_does_not_upgrade_low_score");
    auto distant = mutateWords(page, [](auto& words) { auto w = words[3].toObject(); w["x"] = 320; words[3] = w; });
    check(classifySkinPage(distant).page == SkinPage::Unknown, "variant_does_not_join_far_tokens");
    const auto projected = projectLobbyAnchorDiagnostics(page);
    check(projected.value("valid_input").toBool() && projected.value("coverage") == "anchor_projection", "debug_projection_explicitly_partial");
    check(!classifySkinPage(projected).validInput, "debug_projection_cannot_be_used_as_full_frame");
    int count = 0;
    for (const auto& region : projected.value("regions").toArray()) count += region.toObject().value("words").toArray().size();
    check(count == 10, "debug_preserves_observed_geometry_tokens");
    auto privateText = mutateWords(page, [](auto& words) {
        auto extra = words[0].toObject(); extra["text"] = "ACCOUNT-12345"; extra["unexpected"] = "secret"; words.append(extra);
        extra["text"] = QStringLiteral("开始游戏"); extra["y"] = 1100; words.append(extra);
        auto original = words[0].toObject(); original["unexpected"] = "secret"; words[0] = original;
    });
    const auto debug = projectLobbyAnchorDiagnostics(privateText);
    const auto serialized = QJsonDocument(debug).toJson();
    check(!serialized.contains("ACCOUNT") && !serialized.contains("secret"), "debug_omits_unknown_fields_and_nonanchor_text");
    count = 0; for (const auto& region : debug.value("regions").toArray()) count += region.toObject().value("words").toArray().size();
    check(count == 10, "out_of_region_and_unapproved_characters_excluded");
    auto bad = page; bad["width"] = 0;
    check(!projectLobbyAnchorDiagnostics(bad).value("valid_input").toBool(), "invalid_input_has_no_projection");
    auto many = mutateWords(page, [](auto& words) { for (int i = 0; i < 40; ++i) words.append(words[0]); });
    const auto first = projectLobbyAnchorDiagnostics(many).value("regions").toArray()[0].toObject();
    check(first.value("words").toArray().size() == 32 && first.value("truncated").toBool(), "projection_cap_is_explicit");
    std::cout << "LOBBY_CALIBRATION_TESTS=" << (failures ? "FAIL" : "PASS") << "; assertions=" << assertions
        << "; failures=" << failures << "; live_capture=false; image_file_writes=0\n";
    return failures ? 1 : 0;
}
