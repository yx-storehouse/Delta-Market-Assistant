#include "application/catalog_configuration.h"
#include "domain.h"

#include <QHash>
#include <QSet>

namespace relink::application {
namespace {
// Historical default fingerprints are for removal only. They are never loaded
// into the application as a catalogue or an observation.
const QVector<::Skin>& defaults() {
    static const QVector<::Skin> rows{
        {"demo-01", QStringLiteral("AUG · 天命"), QStringLiteral("S6 典藏"), QStringLiteral("成色S"), QStringLiteral("橙色"), 2.18, 628, -2.4, true},
        {"demo-02", QStringLiteral("M4A1 · 星海回响"), QStringLiteral("S11 典藏"), QStringLiteral("成色S"), QStringLiteral("橙色"), 1.42, 468, 1.8, true},
        {"demo-03", QStringLiteral("AKM · 赤焰"), QStringLiteral("S9 典藏"), QStringLiteral("成色A"), QStringLiteral("紫色"), 6.35, 215, -1.2, true},
        {"demo-04", QStringLiteral("AWM · 极光"), QStringLiteral("S8 典藏"), QStringLiteral("成色S"), QStringLiteral("橙色"), 3.06, 892, -4.1, true},
        {"demo-05", QStringLiteral("Vector · 霓虹脉冲"), QStringLiteral("S10 典藏"), QStringLiteral("成色A"), QStringLiteral("紫色"), 7.12, 176, 3.2, true},
        {"demo-06", QStringLiteral("SCAR-H · 暮光"), QStringLiteral("S7 典藏"), QStringLiteral("成色S"), QStringLiteral("橙色"), 2.64, 538, -0.7, true},
        {"demo-07", QStringLiteral("MP5 · 冰川"), QStringLiteral("S9 典藏"), QStringLiteral("成色A"), QStringLiteral("蓝色"), 8.48, 92, 0.0, true},
        {"demo-08", QStringLiteral("G3 · 深空"), QStringLiteral("S10 典藏"), QStringLiteral("成色A"), QStringLiteral("紫色"), 5.73, 264, -2.0, true},
        {"demo-09", QStringLiteral("M700 · 沙漠之眼"), QStringLiteral("S8 典藏"), QStringLiteral("成色B"), QStringLiteral("紫色"), 14.25, 158, 1.3, true},
        {"demo-10", QStringLiteral("P90 · 碧海"), QStringLiteral("S11 典藏"), QStringLiteral("成色S"), QStringLiteral("蓝色"), 4.05, 126, -0.8, true},
        {"demo-11", QStringLiteral("SR-3M · 幻影"), QStringLiteral("S7 典藏"), QStringLiteral("成色A"), QStringLiteral("紫色"), 9.16, 198, 2.1, false},
        {"demo-12", QStringLiteral("M249 · 机械黎明"), QStringLiteral("S6 典藏"), QStringLiteral("成色B"), QStringLiteral("橙色"), 12.28, 384, -3.6, false}
    };
    return rows;
}

const ::Skin* knownDemoIdentity(const ::Skin& skin) {
    if (!skin.catalogProductId.isEmpty()) return nullptr;
    for (const auto& known : defaults())
        if (skin.id == known.id && skin.name == known.name && skin.series == known.series)
            return &known;
    return nullptr;
}

bool exactDefaultSkin(const ::Skin& skin) {
    // Follow/unfollow toggles never make the historical invented identity or
    // its market numbers real. Retain it only if a non-default task needs it.
    const auto* known = knownDemoIdentity(skin);
    return known && skin.condition == known->condition && skin.rarity == known->rarity
        && skin.wear == known->wear && skin.price == known->price && skin.change == known->change
        && skin.menuColor.isEmpty()
        && skin.variant.isEmpty() && skin.skinSeries.isEmpty();
}

bool exactDefaultTask(const Task& task) {
    // Earlier releases deliberately disabled all five defaults. Enable state
    // alone is therefore not evidence that a default became a user rule.
    static const QVector<Task> rows{
        {"task-01", QStringLiteral("天命 · 低磨收藏"), "demo-01", 100, 650, 5, 3, true, {}, QStringLiteral("成色S")},
        {"task-02", QStringLiteral("星海回响 · 价格观察"), "demo-02", 100, 450, 5, 2, true, {}, QStringLiteral("成色S")},
        {"task-03", QStringLiteral("赤焰 · 区间筛选"), "demo-03", 150, 240, 10, 4, true, {}, QStringLiteral("成色A")},
        {"task-04", QStringLiteral("极光 · 收藏备选"), "demo-04", 600, 850, 5, 1, false, {}, QStringLiteral("成色S")},
        {"task-05", QStringLiteral("暮光 · 低价提醒"), "demo-06", 300, 550, 5, 2, true, {}, QStringLiteral("成色S")}
    };
    for (const auto& known : rows) {
        if (task.id == known.id && task.name == known.name && task.skinId == known.skinId
            && task.minPrice == known.minPrice && task.maxPrice == known.maxPrice
            && task.maxWear == known.maxWear && task.quantity == known.quantity
            && (task.condition == known.condition
                || (!task.conditionExplicit && task.condition == QStringLiteral("不限")))) return true;
    }
    return false;
}

bool fail(QString* error, const QString& text) {
    if (error) *error = text;
    return false;
}

QString productFor(const ::Skin& skin, const catalog::Catalog& catalog) {
    if (!skin.catalogProductId.isEmpty())
        return catalog.findSkin(skin.catalogProductId) ? skin.catalogProductId : QString();
    for (const auto& item : catalog.skins)
        if (skin.id == item.productId || skin.id == catalogSkinId(item.productId)) return item.productId;
    return {};
}

bool clearKnownSynthetic(::Skin& skin) {
    const auto* known = knownDemoIdentity(skin);
    const bool fixture = skin.dataSource == QStringLiteral("test_fixture");
    if (!known && !fixture) return false;
    // Keep user-edited identities and task conditions, but an explicitly
    // synthetic observation never becomes a real quote after a rename.
    if (!known) {
        const bool changed = skin.priceKnown || skin.wearKnown || skin.changeKnown
            || skin.price != 0 || skin.wear != 0 || skin.change != 0;
        skin.price = skin.wear = skin.change = 0;
        skin.priceKnown = skin.wearKnown = skin.changeKnown = false;
        skin.condition = QStringLiteral("未采集");
        skin.dataSource = QStringLiteral("legacy_configuration");
        return changed;
    }
    bool changed = false;
    if (fixture || skin.price == known->price) {
        changed |= skin.priceKnown || skin.price != 0;
        skin.price = 0; skin.priceKnown = false;
    }
    if (fixture || skin.wear == known->wear) {
        changed |= skin.wearKnown || skin.wear != 0;
        skin.wear = 0; skin.wearKnown = false;
    }
    if (fixture || skin.change == known->change) {
        changed |= skin.changeKnown || skin.change != 0;
        skin.change = 0; skin.changeKnown = false;
    }
    if (skin.condition == known->condition) skin.condition = QStringLiteral("未采集");
    if (skin.rarity == known->rarity) skin.rarity = QStringLiteral("待核对");
    skin.dataSource = QStringLiteral("legacy_configuration");
    return changed;
}
} // namespace

QString catalogSkinId(const QString& productId) { return QStringLiteral("catalog:") + productId; }

bool applyCatalogConfiguration(AppState& state, const catalog::Catalog& catalog,
                               CatalogProjectionReport* report, QString* error) {
    if (error) error->clear();
    catalog::Catalog checked;
    if (!catalog::parseCatalog(catalog::serializeCatalog(catalog), &checked, error)) return false;
    if (checked.skins.isEmpty()) return fail(error, QStringLiteral("真实皮肤目录为空，原配置未改动"));
    CatalogProjectionReport changes;
    QSet<QString> exactDefaults, originalIds, taskIds;
    for (const auto& skin : state.skins) {
        if (originalIds.contains(skin.id)) return fail(error, QStringLiteral("原配置含重复皮肤 ID"));
        originalIds.insert(skin.id);
        if (exactDefaultSkin(skin)) exactDefaults.insert(skin.id);
    }
    QVector<Task> nextTasks;
    QSet<QString> referenced;
    for (const auto& task : state.tasks) {
        if (taskIds.contains(task.id)) return fail(error, QStringLiteral("原配置含重复任务 ID"));
        taskIds.insert(task.id);
        if (exactDefaults.contains(task.skinId) && exactDefaultTask(task)) {
            ++changes.removedDefaultTasks;
            continue;
        }
        nextTasks.append(task);
        referenced.insert(task.skinId);
    }
    QHash<QString, ::Skin> previousCatalog;
    QHash<QString, QString> remap;
    QVector<::Skin> legacy;
    for (auto skin : state.skins) {
        if (exactDefaults.contains(skin.id) && !referenced.contains(skin.id)) {
            ++changes.removedDefaultSkins;
            continue;
        }
        const auto product = productFor(skin, checked);
        if (product.isEmpty()) {
            if (clearKnownSynthetic(skin)) ++changes.clearedSyntheticObservations;
            legacy.append(skin);
            ++changes.preservedLegacySkins;
            continue;
        }
        if (previousCatalog.contains(product))
            return fail(error, QStringLiteral("商品 %1 对应多条旧皮肤记录；原配置未改动").arg(product));
        previousCatalog.insert(product, skin);
        remap.insert(skin.id, catalogSkinId(product));
    }
    QVector<::Skin> nextSkins;
    QSet<QString> nextIds;
    for (const auto& item : checked.skins) {
        const auto existing = previousCatalog.constFind(item.productId);
        ::Skin skin;
        if (existing != previousCatalog.cend()) {
            skin = existing.value();
            ++changes.updatedSkins;
            if (skin.dataSource == QStringLiteral("test_fixture")) {
                skin.price = skin.wear = skin.change = 0;
                skin.priceKnown = skin.wearKnown = skin.changeKnown = false;
                skin.condition = QStringLiteral("未采集");
                ++changes.clearedSyntheticObservations;
            }
        } else {
            ++changes.addedSkins;
            skin.condition = QStringLiteral("未采集");
        }
        skin.id = catalogSkinId(item.productId);
        skin.name = item.displayName;
        skin.series = item.seasonId + QStringLiteral(" · ") + item.seasonLabel;
        skin.rarity = item.gameQualityName.isEmpty() ? QStringLiteral("待核对") : item.gameQualityName;
        if (skin.condition.isEmpty()) skin.condition = QStringLiteral("未采集");
        skin.catalogProductId = item.productId;
        skin.menuColor = item.menuColor;
        skin.variant = item.variantLabel;
        skin.skinSeries = item.skinSeries;
        if (skin.dataSource.isEmpty() || skin.dataSource == QStringLiteral("test_fixture"))
            skin.dataSource = QStringLiteral("catalog");
        nextIds.insert(skin.id);
        nextSkins.append(skin);
    }
    for (const auto& skin : legacy) {
        if (nextIds.contains(skin.id))
            return fail(error, QStringLiteral("保留的用户皮肤 ID %1 与目录冲突；原配置未改动").arg(skin.id));
        nextIds.insert(skin.id);
        nextSkins.append(skin);
    }
    for (auto& task : nextTasks) {
        if (remap.contains(task.skinId)) task.skinId = remap.value(task.skinId);
        if (!nextIds.contains(task.skinId))
            return fail(error, QStringLiteral("用户任务 %1 的皮肤记录缺失；原配置未改动").arg(task.id));
    }
    state.pauseSimulation();
    state.setProperty("testFixture", false);
    state.simulatedScans = state.simulatedMatches = state.simulatedSuccess = 0;
    state.skins = std::move(nextSkins);
    state.tasks = std::move(nextTasks);
    if (report) *report = changes;
    state.notifyChanged();
    return true;
}

} // namespace relink::application
