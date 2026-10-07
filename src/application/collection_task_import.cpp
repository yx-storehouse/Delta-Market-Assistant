#include "application/collection_task_import.h"
#include "application/catalog_configuration.h"
#include "config/savedvalue_reader.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <utility>

namespace relink::application {
namespace {
bool fail(QString* error, const QString& text) {
    if (error) *error = text;
    return false;
}
QString compactName(QString value) {
    // Whitespace is presentation-only. Never fuzzy-match or substitute words,
    // notably 疾风魅影 and 疾光魅影, which remain different labels.
    value.remove(QRegularExpression(QStringLiteral("\\s+")));
    return value;
}
QString originKey(const TaskImportSource& source) {
    return source.sourceSha256 + QLatin1Char(':') + QString::number(source.row);
}
bool sameSource(const TaskImportSource& a, const TaskImportSource& b) {
    return a.format == b.format && a.sourceSha256 == b.sourceSha256
        && a.dictionarySha256 == b.dictionarySha256 && a.row == b.row
        && a.productId == b.productId && a.conditionId == b.conditionId && a.fields == b.fields;
}
bool validateConfiguration(const AppState& state, const QVector<Task>& tasks, QString* error) {
    QVector<::Skin> checkedSkins;
    QVector<Task> checkedTasks;
    RunSettings checkedRun;
    return decodeV1Config(QJsonDocument(encodeV1Config(state.skins, tasks, state.run))
                              .toJson(QJsonDocument::Compact),
                          checkedSkins, checkedTasks, checkedRun, error);
}
}

QString collectionImportTaskId(const QString& sourceSha256, int sourceRow) {
    return QStringLiteral("savedvalue:") + sourceSha256 + QLatin1Char(':') + QString::number(sourceRow);
}

bool previewCollectionTaskImport(const QByteArray& bytes, const QJsonObject& dictionary,
                                 const catalog::Catalog& catalog, const AppState& state,
                                 CollectionTaskImportPreview* preview, QString* error) {
    if (error) error->clear();
    if (!preview) return fail(error, QStringLiteral("缺少任务导入预览接收对象"));
    const auto decoded = config::savedValueCollectionSnapshot(bytes, dictionary);
    if (!decoded.value(QStringLiteral("valid")).toBool())
        return fail(error, QStringLiteral("任务文件解析失败：%1").arg(decoded.value(QStringLiteral("error")).toString()));
    const auto rows = decoded.value(QStringLiteral("rows")).toArray();
    if (rows.isEmpty()) return fail(error, QStringLiteral("任务文件中没有收藏任务"));
    if (dictionary.value(QStringLiteral("collection_limit_zero_semantics")).toString()
        != QStringLiteral("no_quantity_limit"))
        return fail(error, QStringLiteral("来源字典未确认限量0表示不限，任务未导入"));
    catalog::Catalog checkedCatalog;
    if (!catalog::parseCatalog(catalog::serializeCatalog(catalog), &checkedCatalog, error)) return false;
    if (!validateConfiguration(state, state.tasks, error)) return false;

    CollectionTaskImportPreview result;
    result.sourceSha256 = decoded.value(QStringLiteral("source_sha256")).toString();
    result.dictionarySha256 = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(dictionary).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    result.inactiveSettingsMetadata = decoded.value(QStringLiteral("inactive_settings_metadata")).toObject();
    result.mergedTasks = state.tasks;
    QHash<QString, const Task*> existingIds, existingOrigins;
    for (const auto& task : state.tasks) {
        existingIds.insert(task.id, &task);
        if (!task.importSource.format.isEmpty()) existingOrigins.insert(originKey(task.importSource), &task);
    }
    QHash<QString, const ::Skin*> projectedSkins;
    for (const auto& skin : state.skins) {
        if (skin.catalogProductId.isEmpty()) continue;
        if (projectedSkins.contains(skin.catalogProductId))
            return fail(error, QStringLiteral("当前配置存在重复商品 %1，任务未导入").arg(skin.catalogProductId));
        projectedSkins.insert(skin.catalogProductId, &skin);
    }
    QSet<QString> enabledProducts, allProducts;
    for (const auto& value : rows) {
        const auto raw = value.toObject();
        const int sourceRow = raw.value(QStringLiteral("row_index")).toInt(-1);
        const QString productId = raw.value(QStringLiteral("source_product_id")).toString();
        const QString context = QStringLiteral("原始第%1行（索引%2，商品%3）")
            .arg(sourceRow + 1).arg(sourceRow).arg(productId);
        if (!raw.value(QStringLiteral("dictionary_resolved")).toBool())
            return fail(error, context + QStringLiteral("的商品或成色ID未知，整份任务未导入"));
        const auto* skin = checkedCatalog.findSkin(productId);
        if (!skin) return fail(error, context + QStringLiteral("未在真实皮肤目录中找到，整份任务未导入"));
        if (compactName(skin->displayName) != compactName(raw.value(QStringLiteral("display_name")).toString())
            || compactName(skin->seasonLabel) != compactName(raw.value(QStringLiteral("season_label")).toString())
            || compactName(skin->displayName.section(QLatin1Char('|'), 1))
                != compactName(raw.value(QStringLiteral("product_name")).toString()))
            return fail(error, context + QStringLiteral("的商品名称或赛季与真实目录不一致，整份任务未导入"));
        const auto* projected = projectedSkins.value(productId, nullptr);
        if (!projected || projected->id != catalogSkinId(productId)
            || projected->name != skin->displayName || projected->menuColor != skin->menuColor
            || projected->variant != skin->variantLabel || projected->skinSeries != skin->skinSeries
            || projected->series != skin->seasonId + QStringLiteral(" · ") + skin->seasonLabel)
            return fail(error, context + QStringLiteral("的当前目录投影不一致，请先重新加载皮肤资料"));

        CollectionTaskImportRow row;
        row.productId = productId;
        row.seasonId = skin->seasonId;
        row.seasonLabel = skin->seasonLabel;
        row.displayName = skin->displayName;
        row.menuColor = skin->menuColor;
        row.variantLabel = skin->variantLabel;
        auto& task = row.task;
        task.id = collectionImportTaskId(result.sourceSha256, sourceRow);
        task.skinId = projected->id;
        task.condition = raw.value(QStringLiteral("condition_label")).toString();
        task.name = skin->displayName + QStringLiteral(" · ") + task.condition;
        task.minPrice = raw.value(QStringLiteral("price_min")).toString().toDouble();
        task.maxPrice = raw.value(QStringLiteral("price_max")).toString().toDouble();
        task.maxWear = raw.value(QStringLiteral("max_wear")).toString().toDouble();
        const double quantity = raw.value(QStringLiteral("limit_raw")).toDouble();
        if (quantity > 9999) return fail(error, context + QStringLiteral("的限量超过9999，整份任务未导入"));
        task.quantity = static_cast<int>(quantity);
        task.enabled = raw.value(QStringLiteral("enabled")).toBool();
        task.status = task.enabled ? QStringLiteral("待启动") : QStringLiteral("未启用");
        task.importSource = {QStringLiteral("savedValue"), result.sourceSha256, result.dictionarySha256,
                             productId, raw.value(QStringLiteral("source_condition_id")).toString(),
                             sourceRow, raw.value(QStringLiteral("source_fields")).toObject()};
        const Task* existing = existingOrigins.value(originKey(task.importSource), nullptr);
        const Task* sameId = existingIds.value(task.id, nullptr);
        if (sameId && sameId != existing)
            return fail(error, context + QStringLiteral("的稳定任务ID已被其它任务占用，整份任务未导入"));
        if (existing) {
            if (!sameSource(existing->importSource, task.importSource))
                return fail(error, context + QStringLiteral("的历史导入来源与本次不一致，整份任务未导入"));
            row.alreadyImported = true;
            ++result.duplicateCount;
            // Preserve every current editable value; the preview still shows
            // the original input so a skipped row never appears overwritten.
        } else {
            result.mergedTasks.append(task);
            ++result.addedCount;
        }
        allProducts.insert(productId);
        if (task.enabled) { ++result.enabledCount; enabledProducts.insert(productId); }
        result.rows.append(std::move(row));
    }
    result.rowCount = result.rows.size();
    result.distinctProducts = enabledProducts.size();
    result.totalDistinctProducts = allProducts.size();
    if (!validateConfiguration(state, result.mergedTasks, error)) return false;
    *preview = std::move(result);
    return true;
}

bool applyCollectionTaskImport(const QByteArray& bytes, const QJsonObject& dictionary,
                               const catalog::Catalog& catalog, AppState& state,
                               CollectionTaskImportPreview* report, QString* error) {
    CollectionTaskImportPreview prepared;
    if (!previewCollectionTaskImport(bytes, dictionary, catalog, state, &prepared, error)) return false;
    if (prepared.addedCount > 0) {
        state.tasks = prepared.mergedTasks;
        state.notifyChanged();
    }
    if (report) *report = std::move(prepared);
    return true;
}

} // namespace relink::application
