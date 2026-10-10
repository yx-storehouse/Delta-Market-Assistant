#include "catalog/skin_catalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <iostream>

using namespace relink::catalog;
namespace {
QByteArray read(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}
bool write(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
Skin fixture(const QString& id = QStringLiteral("user:test-s12-one"),
             const QString& season = QStringLiteral("S12"),
             const QString& seasonLabel = QStringLiteral("测试赛季，仅单测")) {
    Skin s;
    s.productId = id;
    s.seasonId = season;
    s.seasonLabel = seasonLabel;
    s.weapon = QStringLiteral("测试武器，仅单测");
    s.skinSeries = QStringLiteral("测试皮肤，仅单测");
    s.grade = QStringLiteral("史诗品阶");
    s.displayName = season + QStringLiteral(" | 测试武器 - 测试皮肤，仅单测");
    return s;
}
Catalog addition(const Skin& skin) {
    Catalog c;
    c.seasons.append({skin.seasonId, skin.seasonLabel});
    c.skins.append(skin);
    return c;
}
QByteArray changedRoot(const QByteArray& original, const QString& key, const QJsonValue& value) {
    auto root = QJsonDocument::fromJson(original).object();
    root[key] = value;
    return QJsonDocument(root).toJson();
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int count = 0, failures = 0;
    const auto check = [&](bool ok, const char* name) {
        ++count;
        if (!ok) ++failures;
        std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    };
    const auto builtinPath = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                     : QStringLiteral("src/assets/catalog/skins.json");
    const auto bytes = read(builtinPath);
    check(!bytes.isEmpty(), "builtin_file_read");
    Catalog catalog;
    QString error;
    check(parseCatalog(bytes, &catalog, &error), "builtin_parses");
    check(catalog.skins.size() == 149 && catalog.seasons.size() == 11, "149_real_skins_11_seasons");
    bool blankImages = true, everyGrade = true, idsNumeric = true;
    QHash<QString, int> grades;
    for (const auto& skin : catalog.skins) {
        blankImages &= skin.thumbnailPath.isEmpty();
        everyGrade &= gradeLabels().contains(skin.grade);
        ++grades[skin.grade];
        bool ok = false;
        skin.productId.toLongLong(&ok);
        idsNumeric &= ok;
    }
    check(blankImages, "every_thumbnail_reserved_empty");
    check(everyGrade && grades.value(QStringLiteral("传说品阶")) == 28 && grades.value(QStringLiteral("史诗品阶")) == 33
          && grades.value(QStringLiteral("稀有品阶")) == 88, "every_skin_carries_its_game_grade");
    check(!bytes.contains("menu_color") && !bytes.contains("game_quality_name")
          && bytes.contains("\"relink-skin-catalog-v2\""), "builtin_has_no_colour_field");
    check(idsNumeric, "original_numeric_ids_preserved");
    const auto* aug = catalog.findSkin(QStringLiteral("10602"));
    check(aug && aug->grade == QStringLiteral("史诗品阶") && aug->skinSeries == QStringLiteral("天命")
          && aug->seasonId == "S6", "known_epic_aug_identity");
    check(catalog.findSkin("10100") && catalog.findSkin("10101")
          && catalog.findSkin("10100")->variantLabel == QStringLiteral("极品")
          && catalog.findSkin("10101")->variantLabel == QStringLiteral("优品")
          && catalog.findSkin("10100")->productId != catalog.findSkin("10101")->productId,
          "same_weapon_variants_remain_distinct");
    check(!catalog.findSkin("unknown") && !catalog.findSeason("S99"), "missing_lookup_is_null");
    bool seasonsOrdered = true;
    for (int i = 0; i < catalog.seasons.size(); ++i)
        seasonsOrdered &= catalog.seasons[i].id == "S" + QString::number(i + 1);
    check(seasonsOrdered, "season_numeric_source_order");
    Catalog roundtrip;
    check(parseCatalog(serializeCatalog(catalog), &roundtrip, &error)
          && serializeCatalog(roundtrip) == serializeCatalog(catalog), "canonical_roundtrip");
    if (argc > 2) {
        const auto reference = QJsonDocument::fromJson(read(QString::fromLocal8Bit(argv[2]))).object();
        const auto rows = reference.value("rows").toArray();
        bool identical = rows.size() == catalog.skins.size();
        for (const auto& value : rows) {
            const auto row = value.toObject();
            const auto* skin = catalog.findSkin(row.value("product_id").toString());
            identical &= skin && skin->seasonId == row.value("season_id").toString()
                && skin->seasonLabel == row.value("season_label").toString()
                && skin->weapon == row.value("weapon").toString()
                && skin->skinSeries == row.value("skin_series").toString()
                && skin->variantLabel == row.value("variant_label").toString()
                && skin->grade == gradeFromMenuColor(row.value("menu_color").toString())
                && skin->displayName == row.value("display_name").toString();
        }
        check(identical, "all_149_items_match_user_verified_reference_and_its_colour_grade");
    }
    const auto preserved = serializeCatalog(catalog);
    check(!parseCatalog("{", &catalog, &error) && serializeCatalog(catalog) == preserved,
          "malformed_parse_preserves_output");
    check(!parseCatalog(QByteArray(4 * 1024 * 1024 + 1, 'x'), &catalog, &error), "bounded_file_size");
    check(!parseCatalog(changedRoot(bytes, "schema", "unknown"), &catalog, &error), "schema_checked");
    check(!parseCatalog(changedRoot(bytes, "prices", QJsonArray{1}), &catalog, &error), "market_data_not_catalog_fields");
    check(!parseCatalog(changedRoot(bytes, "skins", false), &catalog, &error), "wrong_array_type_rejected");
    auto one = addition(fixture());
    one.skins.append(one.skins.first());
    check(!parseCatalog(serializeCatalog(one), &catalog, &error), "duplicate_product_in_import_rejected");
    one = addition(fixture()); one.seasons.append(one.seasons.first());
    check(!parseCatalog(serializeCatalog(one), &catalog, &error), "duplicate_season_in_import_rejected");
    one = addition(fixture()); one.skins[0].seasonLabel = "different";
    check(!parseCatalog(serializeCatalog(one), &catalog, &error), "conflicting_skin_season_rejected");
    one = addition(fixture()); one.skins[0].thumbnailPath = "https://example.invalid/test.png";
    check(!parseCatalog(serializeCatalog(one), &catalog, &error), "thumbnail_paths_rejected_in_current_version");
    for (const auto& bad : {QStringLiteral("S"), QStringLiteral("purple"), QStringLiteral("普通品阶"), QStringLiteral("史诗")}) {
        one = addition(fixture()); one.skins[0].grade = bad;
        check(!parseCatalog(serializeCatalog(one), &catalog, &error), "only_game_grade_labels_accepted");
    }
    one = addition(fixture()); one.skins[0].grade.clear();
    check(parseCatalog(serializeCatalog(one), &catalog, &error) && catalog.skins[0].grade.isEmpty(),
          "unrecorded_grade_stays_empty");
    {
        // An earlier v1 file (menu colour) still loads; the colour becomes its 品阶,
        // a recorded game_quality_name wins, and v2 is written back.
        auto legacy = QJsonDocument::fromJson(serializeCatalog(addition(fixture()))).object();
        legacy["schema"] = QStringLiteral("relink-skin-catalog-v1");
        auto skins = legacy["skins"].toArray();
        const QList<QPair<QString, QString>> colours{{"orange", "传说品阶"}, {"red", "传说品阶"},
            {"purple", "史诗品阶"}, {"blue", "稀有品阶"}, {"unknown", ""}};
        bool converted = true;
        for (const auto& [colour, grade] : colours) {
            auto skin = skins[0].toObject();
            skin.remove("grade");
            skin["menu_color"] = colour;
            skin["game_quality_name"] = QString();
            legacy["skins"] = QJsonArray{skin};
            Catalog old;
            converted &= parseCatalog(QJsonDocument(legacy).toJson(), &old, &error) && old.skins[0].grade == grade
                && !serializeCatalog(old).contains("menu_color") && serializeCatalog(old).contains("relink-skin-catalog-v2");
        }
        check(converted, "v1_menu_colour_converted_to_grade");
        auto skin = skins[0].toObject();
        skin.remove("grade");
        skin["menu_color"] = QStringLiteral("blue");
        skin["game_quality_name"] = QStringLiteral("史诗品阶");
        legacy["skins"] = QJsonArray{skin};
        Catalog old;
        check(parseCatalog(QJsonDocument(legacy).toJson(), &old, &error) && old.skins[0].grade == QStringLiteral("史诗品阶"),
              "v1_recorded_quality_wins_over_colour");
        // The old dialog's free text: a grade name (with or without 品阶) wins, anything else uses the colour.
        bool freeText = true;
        for (const auto& [typed, expected] : QList<QPair<QString, QString>>{{"传说", "传说品阶"}, {"紫色", "稀有品阶"}, {"高级", "稀有品阶"}}) {
            skin["game_quality_name"] = typed;
            legacy["skins"] = QJsonArray{skin};
            freeText &= parseCatalog(QJsonDocument(legacy).toJson(), &old, &error) && old.skins[0].grade == expected;
        }
        check(freeText, "v1_free_text_quality_never_blocks_loading");
        skin["game_quality_name"] = QStringLiteral("史诗品阶");
        skin["menu_color"] = QStringLiteral("green");
        legacy["skins"] = QJsonArray{skin};
        check(!parseCatalog(QJsonDocument(legacy).toJson(), &old, &error), "v1_unknown_colour_rejected");
        auto mixed = QJsonDocument::fromJson(serializeCatalog(addition(fixture()))).object();
        auto mixedSkin = mixed["skins"].toArray()[0].toObject();
        mixedSkin["menu_color"] = QStringLiteral("purple");
        mixed["skins"] = QJsonArray{mixedSkin};
        check(!parseCatalog(QJsonDocument(mixed).toJson(), &old, &error), "v2_rejects_colour_field");
    }
    one = addition(fixture()); one.skins[0].productId = "../test";
    check(!parseCatalog(serializeCatalog(one), &catalog, &error), "unsafe_nonstable_id_rejected");
    one = addition(fixture()); one.skins[0].weapon = "bad\nname";
    check(!parseCatalog(serializeCatalog(one), &catalog, &error), "control_characters_rejected");
    one = addition(fixture()); one.skins[0].weapon = QString(161, QChar('x'));
    check(!parseCatalog(serializeCatalog(one), &catalog, &error), "field_length_bounded");
    auto invalidUtf8 = serializeCatalog(addition(fixture()));
    invalidUtf8.replace("S12", QByteArray(1, char(0xff)));
    check(!parseCatalog(invalidUtf8, &catalog, &error), "invalid_utf8_rejected");
    CatalogStore uninitialized;
    check(!uninitialized.appendSeason({"S12", "test"}, {}, &error), "unloaded_store_cannot_save");

    QTemporaryDir temp;
    check(temp.isValid(), "isolated_temp_directory");
    const auto path = temp.filePath("user/skins.json");
    CatalogStore store;
    check(store.load(bytes, path, &error) && store.catalog().skins.size() == 149,
          "missing_user_extension_loads_builtin");
    check(!QFile::exists(path), "read_only_load_creates_no_extension");
    const auto newSkin = fixture();
    check(store.appendSeason({newSkin.seasonId, newSkin.seasonLabel}, {newSkin}, &error), "append_new_season_commits");
    check(store.catalog().skins.size() == 150 && store.catalog().seasons.size() == 12
          && !read(path).isEmpty(), "appended_skin_available_and_persisted");
    const auto saved = read(path);
    const auto exported = store.exportJson();
    check(!store.appendSeason({newSkin.seasonId, newSkin.seasonLabel}, {}, &error)
          && read(path) == saved, "existing_season_append_rejected_without_rewrite");
    check(!store.appendSkins({newSkin}, &error) && read(path) == saved
          && store.exportJson() == exported, "duplicate_id_never_overwrites");
    check(!store.importJson(exported, &error) && read(path) == saved
          && store.exportJson() == exported, "self_full_import_rejected_atomically");
    auto second = fixture("user:second");
    check(store.appendSkins({second}, &error) && store.catalog().skins.size() == 151,
          "append_skin_existing_season");
    CatalogStore reload;
    check(reload.load(bytes, path, &error) && reload.catalog().skins.size() == 151
          && reload.catalog().findSkin(second.productId), "user_skins_survive_restart");
    Catalog exportedExtension;
    check(parseCatalog(store.extensionJson(), &exportedExtension, &error)
          && exportedExtension.skins.size() == 2 && exportedExtension.seasons.size() == 1,
          "extension_export_excludes_builtin");
    const auto savedAgain = read(path);
    auto conflict = fixture("user:conflict", "S1", "not original season label");
    check(!store.importJson(serializeCatalog(addition(conflict)), &error)
          && read(path) == savedAgain, "conflicting_existing_season_import_rejected");
    const auto beforeLoadFailure = store.exportJson();
    const auto corrupt = temp.filePath("corrupt.json");
    check(write(corrupt, "broken") && !store.load(bytes, corrupt, &error)
          && store.exportJson() == beforeLoadFailure && read(corrupt) == "broken",
          "corrupt_extension_neither_erased_nor_replaces_memory");
    auto mixed = addition(fixture("user:third")); mixed.skins.append(second);
    check(!store.importJson(serializeCatalog(mixed), &error)
          && !store.catalog().findSkin("user:third") && read(path) == savedAgain,
          "mixed_import_failure_has_no_partial_addition");
    QLockFile heldLock(path + ".lock");
    heldLock.setStaleLockTime(0);
    check(heldLock.tryLock(0), "test_holds_writer_lock");
    check(!store.appendSkins({fixture("user:locked")}, &error)
          && read(path) == savedAgain && !store.catalog().findSkin("user:locked"),
          "writer_lock_preserves_disk_and_memory");
    heldLock.unlock();
    check(write(path, savedAgain + "\n") && !store.appendSkins({fixture("user:stale")}, &error)
          && read(path) == savedAgain + "\n" && !store.catalog().findSkin("user:stale"),
          "external_changes_are_not_lost");
    const auto blockedParent = temp.filePath("regular_file");
    check(write(blockedParent, "unchanged"), "write_failure_fixture");
    CatalogStore unwritable;
    check(unwritable.load(bytes, blockedParent + "/skins.json", &error), "missing_extension_below_blocked_parent_loads");
    check(!unwritable.appendSeason({newSkin.seasonId, newSkin.seasonLabel}, {newSkin}, &error)
          && unwritable.catalog().skins.size() == 149 && read(blockedParent) == "unchanged",
          "failed_save_does_not_modify_memory_or_original_file");
    const auto sharedPath = temp.filePath("shared.json");
    CatalogStore first, stale;
    check(first.load(bytes, sharedPath, &error) && stale.load(bytes, sharedPath, &error), "two_readers_load_missing_extension");
    check(first.appendSeason({newSkin.seasonId, newSkin.seasonLabel}, {newSkin}, &error), "first_writer_commits");
    const auto firstSaved = read(sharedPath);
    check(!stale.appendSeason({newSkin.seasonId, newSkin.seasonLabel}, {second}, &error)
          && read(sharedPath) == firstSaved && stale.catalog().skins.size() == 149,
          "second_stale_writer_cannot_overwrite_first");
    CatalogStore imported;
    check(imported.load(bytes, temp.filePath("import.json"), &error)
          && imported.importJson(first.extensionJson(), &error)
          && imported.catalog().findSkin(newSkin.productId), "extension_import_on_other_store");
    const auto guardedPath = temp.filePath("guarded.json");
    CatalogStore guarded;
    check(guarded.load(bytes, guardedPath, &error), "commit_guard_store_loads");
    bool sawMergedCatalog = false;
    guarded.setCommitValidator([&](const Catalog& next, QString* reason) {
        sawMergedCatalog = next.skins.size() == 150 && next.findSkin(newSkin.productId);
        *reason = "user configuration collision";
        return false;
    });
    const auto beforeGuard = guarded.exportJson();
    check(!guarded.appendSeason({newSkin.seasonId, newSkin.seasonLabel}, {newSkin}, &error)
          && sawMergedCatalog && error == "user configuration collision"
          && guarded.exportJson() == beforeGuard && !QFile::exists(guardedPath),
          "projection_guard_rejects_before_first_disk_or_memory_mutation");
    guarded.setCommitValidator({});
    check(guarded.appendSeason({newSkin.seasonId, newSkin.seasonLabel}, {newSkin}, &error),
          "standalone_store_with_no_guard_can_commit");
    const auto guardedBytes = read(guardedPath), guardedMemory = guarded.exportJson();
    guarded.setCommitValidator([](const Catalog&, QString*) { return false; });
    check(!guarded.appendSkins({fixture("user:guarded-second")}, &error)
          && !error.isEmpty() && read(guardedPath) == guardedBytes
          && guarded.exportJson() == guardedMemory,
          "projection_guard_failure_preserves_existing_bytes_and_memory");
    {
        // An existing v1 extension (menu colours) loads, and the first append
        // keeps its original bytes as a backup before writing v2.
        const auto legacyPath = temp.filePath("legacy/catalog_extensions.json");
        QDir().mkpath(QFileInfo(legacyPath).absolutePath());
        auto legacy = QJsonDocument::fromJson(serializeCatalog(addition(fixture("user:legacy")))).object();
        legacy["schema"] = QStringLiteral("relink-skin-catalog-v1");
        auto skin = legacy["skins"].toArray()[0].toObject();
        skin.remove("grade");
        skin["menu_color"] = QStringLiteral("orange");
        skin["game_quality_name"] = QString();
        legacy["skins"] = QJsonArray{skin};
        const auto legacyBytes = QJsonDocument(legacy).toJson();
        check(write(legacyPath, legacyBytes), "legacy_extension_written");
        CatalogStore upgraded;
        check(upgraded.load(bytes, legacyPath, &error) && upgraded.catalog().findSkin("user:legacy")
              && upgraded.catalog().findSkin("user:legacy")->grade == QStringLiteral("传说品阶"),
              "legacy_extension_loads_with_grade");
        check(upgraded.appendSkins({fixture("user:after-upgrade")}, &error)
              && read(legacyPath + ".before-grade-v2.bak") == legacyBytes
              && read(legacyPath).contains("relink-skin-catalog-v2") && !read(legacyPath).contains("menu_color"),
              "legacy_extension_backed_up_before_v2_rewrite");
        const auto backup = read(legacyPath + ".before-grade-v2.bak");
        check(upgraded.appendSkins({fixture("user:second-after-upgrade")}, &error)
              && read(legacyPath + ".before-grade-v2.bak") == backup, "backup_written_once");
    }
    {
        // The product name follows from its fields; the runner checks the same rule.
        auto named = addition(fixture("user:named"));
        named.skins[0].displayName.clear();
        Catalog parsedNames;
        check(parseCatalog(serializeCatalog(named), &parsedNames, &error)
              && parsedNames.skins[0].displayName == QStringLiteral("S12|测试武器，仅单测 - 测试皮肤，仅单测"),
              "empty_display_name_built_from_fields");
        named.skins[0].displayName = QStringLiteral("测试武器 - 别的名字");
        check(parseCatalog(serializeCatalog(named), &parsedNames, &error)
              && parsedNames.skins[0].displayName == QStringLiteral("S12|测试武器，仅单测 - 测试皮肤，仅单测"),
              "unprefixed_or_different_name_rebuilt");
        named.skins[0].displayName = QStringLiteral("S12 | 测试武器，仅单测 -  测试皮肤，仅单测");
        check(parseCatalog(serializeCatalog(named), &parsedNames, &error)
              && parsedNames.skins[0].displayName == QStringLiteral("S12 | 测试武器，仅单测 -  测试皮肤，仅单测"),
              "equal_name_keeps_its_spelling");
        Catalog builtinAgain;
        bool builtinUnchanged = parseCatalog(bytes, &builtinAgain, &error) && builtinAgain.skins.size() == 149;
        const auto rawSkins = QJsonDocument::fromJson(bytes).object()["skins"].toArray();
        for (int i = 0; builtinUnchanged && i < rawSkins.size(); ++i)
            builtinUnchanged &= builtinAgain.skins[i].displayName == rawSkins[i].toObject()["display_name"].toString();
        check(builtinUnchanged && serializeCatalog(builtinAgain) == serializeCatalog(roundtrip), "builtin_names_unchanged");
    }
    const auto id1 = nextUserProductId(), id2 = nextUserProductId();
    check(id1.startsWith("user:") && id1 != id2, "local_ids_stable_namespace_and_unique");
    check(parseCatalog(serializeCatalog(addition(fixture(id1))), &catalog, &error), "generated_id_passes_schema");
    std::cout << "SKIN_CATALOG_TESTS=" << (failures ? "FAIL" : "PASS")
              << "; assertions=" << count << "; failures=" << failures << '\n';
    return failures ? 1 : 0;
}
