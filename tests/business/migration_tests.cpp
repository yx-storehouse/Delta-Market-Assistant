#include "config/v1_adapter.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace relink::config;

namespace {

bool hasCode(const QJsonArray& values, const QString& code) {
    for (const auto& value : values)
        if (value.toObject().value(QStringLiteral("code")).toString() == code) return true;
    return false;
}

bool hasRowCode(const QJsonObject& row, const QString& code) {
    return hasCode(row.value(QStringLiteral("diagnostics")).toArray(), code);
}

QJsonObject validV1() {
    return {
        {QStringLiteral("schema_version"), 1}, {QStringLiteral("demo"), true},
        {QStringLiteral("source"), QStringLiteral("synthetic_frontend_fixture")},
        {QStringLiteral("skins"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("skin-1")}, {QStringLiteral("name"), QStringLiteral("AUG Demo")}, {QStringLiteral("series"), QStringLiteral("S1")}, {QStringLiteral("condition"), QStringLiteral("S")}, {QStringLiteral("rarity"), QStringLiteral("orange")}, {QStringLiteral("wear"), 2.18}, {QStringLiteral("price"), 628.0}, {QStringLiteral("change"), -2.4}, {QStringLiteral("followed"), true}}}},
        {QStringLiteral("tasks"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("task-1")}, {QStringLiteral("name"), QStringLiteral("Demo task")}, {QStringLiteral("skinId"), QStringLiteral("skin-1")}, {QStringLiteral("minPrice"), 230.0}, {QStringLiteral("maxPrice"), 600.0}, {QStringLiteral("maxWear"), 0.399}, {QStringLiteral("quantity"), 3}, {QStringLiteral("enabled"), true}}}}
    };
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QByteArray v1 = QJsonDocument(validV1()).toJson(QJsonDocument::Compact);
    const auto preview = previewV1(v1);
    Q_ASSERT(preview.value(QStringLiteral("kind")).toString() == QStringLiteral("LegacyImportPreview"));
    Q_ASSERT(preview.value(QStringLiteral("source_format")).toString() == QStringLiteral("schema_v1"));
    Q_ASSERT(!preview.value(QStringLiteral("committable")).toBool());
    Q_ASSERT(preview.value(QStringLiteral("rows")).toArray().size() == 1);
    const auto row = preview.value(QStringLiteral("rows")).toArray().first().toObject();
    Q_ASSERT(row.value(QStringLiteral("status")).toString() == QStringLiteral("review"));
    Q_ASSERT(hasRowCode(row, QStringLiteral("PRODUCT_UNRESOLVED")));
    Q_ASSERT(row.value(QStringLiteral("candidate")).toObject().value(QStringLiteral("enabled")).toBool() == false);
    Q_ASSERT(row.value(QStringLiteral("candidate")).toObject().value(QStringLiteral("review_required")).toBool());
    Q_ASSERT(preview.value(QStringLiteral("extensions")).toObject().value(QStringLiteral("x-live_market_data")).toBool() == false);
    const auto v1Extensions = preview.value(QStringLiteral("extensions")).toObject();
    Q_ASSERT(v1Extensions.value(QStringLiteral("x-target_mode")).toString() == QStringLiteral("demo"));
    Q_ASSERT(v1Extensions.value(QStringLiteral("x-ui_run_settings")).toObject().value(QStringLiteral("profile")).toString() == QStringLiteral("S11新赛季1103"));
    Q_ASSERT(v1Extensions.value(QStringLiteral("x-ui_run_settings_sources")).toObject().value(QStringLiteral("profile")).toString() == QStringLiteral("default"));
    const auto candidate = row.value(QStringLiteral("candidate")).toObject();
    Q_ASSERT(candidate.value(QStringLiteral("filters")).toObject().value(QStringLiteral("condition")).toObject().value(QStringLiteral("op")).toString() == QStringLiteral("any"));
    Q_ASSERT(candidate.value(QStringLiteral("quantity_candidate")).toInt() == 3);
    Q_ASSERT(candidate.value(QStringLiteral("quantity_semantics")).toString() == QStringLiteral("demo_count_only"));

    const QByteArray normal = QByteArrayLiteral("AUG Demo|any|unowned|orange|S|none|230|600|0.399|rare|default|7|9\n");
    const auto p13 = preview13Columns(normal);
    Q_ASSERT(p13.value(QStringLiteral("rows")).toArray().size() == 1);
    const auto p13row = p13.value(QStringLiteral("rows")).toArray().first().toObject();
    Q_ASSERT(p13row.value(QStringLiteral("status")).toString() == QStringLiteral("review"));
    Q_ASSERT(hasRowCode(p13row, QStringLiteral("UNKNOWN_TRAILING_COLUMNS")));
    Q_ASSERT(p13row.value(QStringLiteral("columns")).toArray().size() == 13);

    const auto reversed = preview13Columns(QByteArrayLiteral("Demo|any|any|any|S|none|601|600|0.399|any|default||\n"));
    const auto reversedRow = reversed.value(QStringLiteral("rows")).toArray().first().toObject();
    Q_ASSERT(reversedRow.value(QStringLiteral("status")).toString() == QStringLiteral("invalid"));
    Q_ASSERT(hasRowCode(reversedRow, QStringLiteral("PRICE_RANGE_REVERSED")));

    const auto grouped = preview13Columns(QByteArrayLiteral("Demo|any|any|any|S|none|1,000|1,500|0.399|any|default||\n"));
    const auto groupedRow = grouped.value(QStringLiteral("rows")).toArray().first().toObject();
    Q_ASSERT(groupedRow.value(QStringLiteral("candidate")).toObject().value(QStringLiteral("price_range")).toObject().value(QStringLiteral("min")).toObject().value(QStringLiteral("value")).toObject().value(QStringLiteral("unscaled")).toString() == QStringLiteral("1000"));
    const auto badGrouping = preview13Columns(QByteArrayLiteral("Demo|any|any|any|S|none|1,00|600|0.399|any|default||\n"));
    Q_ASSERT(hasRowCode(badGrouping.value(QStringLiteral("rows")).toArray().first().toObject(), QStringLiteral("DECIMAL_GROUPING_INVALID")));

    QJsonObject noisy = validV1();
    noisy[QStringLiteral("tasks")] = QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("task-noise")}, {QStringLiteral("name"), QStringLiteral("Noise")}, {QStringLiteral("skinId"), QStringLiteral("skin-1")}, {QStringLiteral("minPrice"), 0.30000000000000004}, {QStringLiteral("maxPrice"), 600.0}, {QStringLiteral("maxWear"), 0.399}, {QStringLiteral("quantity"), 1}, {QStringLiteral("enabled"), true}}};
    const auto noisePreview = previewV1(QJsonDocument(noisy).toJson(QJsonDocument::Compact));
    const auto noiseRow = noisePreview.value(QStringLiteral("rows")).toArray().first().toObject();
    const auto noiseMin = noiseRow.value(QStringLiteral("candidate")).toObject().value(QStringLiteral("price_range")).toObject().value(QStringLiteral("min")).toObject().value(QStringLiteral("value")).toObject();
    Q_ASSERT(noiseMin.value(QStringLiteral("unscaled")).toString() == QStringLiteral("30"));
    Q_ASSERT(noiseMin.value(QStringLiteral("scale")).toInt() == 2);
    Q_ASSERT(hasRowCode(noiseRow, QStringLiteral("V1_FLOAT_NOISE_NORMALIZED")) || hasCode(noisePreview.value(QStringLiteral("diagnostics")).toArray(), QStringLiteral("V1_FLOAT_NOISE_NORMALIZED")));

    QJsonObject invalidRun = validV1();
    invalidRun[QStringLiteral("run_settings")] = QJsonObject{{QStringLiteral("profile"), QString{}}, {QStringLiteral("hotkey"), QStringLiteral("F13")}, {QStringLiteral("purchaseDelayMs"), -1}, {QStringLiteral("queueFullTrigger"), 0}, {QStringLiteral("queueFullStepMs"), 1.25}, {QStringLiteral("scheduleStart"), QStringLiteral("25:00")}};
    const auto invalidRunPreview = previewV1(QJsonDocument(invalidRun).toJson(QJsonDocument::Compact));
    const auto invalidRunExt = invalidRunPreview.value(QStringLiteral("extensions")).toObject();
    const auto invalidRunSettings = invalidRunExt.value(QStringLiteral("x-ui_run_settings")).toObject();
    const auto invalidSources = invalidRunExt.value(QStringLiteral("x-ui_run_settings_sources")).toObject();
    Q_ASSERT(invalidRunSettings.value(QStringLiteral("profile")).toString() == QStringLiteral("S11新赛季1103"));
    Q_ASSERT(invalidRunSettings.value(QStringLiteral("hotkey")).toString() == QStringLiteral("F2"));
    Q_ASSERT(invalidSources.value(QStringLiteral("profile")).toString() == QStringLiteral("invalid"));
    Q_ASSERT(invalidSources.value(QStringLiteral("hotkey")).toString() == QStringLiteral("invalid"));
    Q_ASSERT(invalidSources.value(QStringLiteral("purchaseDelayMs")).toString() == QStringLiteral("invalid"));
    Q_ASSERT(invalidSources.value(QStringLiteral("queueFullStepMs")).toString() == QStringLiteral("invalid"));
    Q_ASSERT(invalidSources.value(QStringLiteral("scheduleStart")).toString() == QStringLiteral("invalid"));
    Q_ASSERT(hasCode(invalidRunPreview.value(QStringLiteral("diagnostics")).toArray(), QStringLiteral("RUN_SETTING_INVALID")));

    QJsonObject invalidPrecision = validV1();
    invalidPrecision[QStringLiteral("tasks")] = QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("task-precision")}, {QStringLiteral("name"), QStringLiteral("Precision")}, {QStringLiteral("skinId"), QStringLiteral("skin-1")}, {QStringLiteral("minPrice"), 0.301}, {QStringLiteral("maxPrice"), 600.0}, {QStringLiteral("maxWear"), 0.399}, {QStringLiteral("quantity"), 1}, {QStringLiteral("enabled"), true}}};
    const auto invalidPrecisionPreview = previewV1(QJsonDocument(invalidPrecision).toJson(QJsonDocument::Compact));
    const auto precisionRow = invalidPrecisionPreview.value(QStringLiteral("rows")).toArray().first().toObject();
    Q_ASSERT(precisionRow.value(QStringLiteral("status")).toString() == QStringLiteral("invalid"));
    Q_ASSERT(hasRowCode(precisionRow, QStringLiteral("V1_PRECISION_INVALID")));

    const auto unsupported = previewV1(QByteArrayLiteral("{\"schema_version\":2,\"demo\":true}"));
    Q_ASSERT(previewHasErrors(unsupported));
    qInfo() << "business_migration_tests: PASS";
    return 0;
}
