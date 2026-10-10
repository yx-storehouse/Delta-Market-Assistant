#include "domain.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSet>
#include <QTime>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr qint64 kMaxConfigBytes = 8 * 1024 * 1024;
constexpr int kMaxItems = 10000;

bool fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}

bool stringField(const QJsonObject& object, const char* key, QString& value,
                 QString* error, const QString& context) {
    const auto entry = object.value(QLatin1String(key));
    if (!entry.isString() || entry.toString().trimmed().isEmpty()
        || entry.toString().size() > 4096) {
        return fail(error, context + QStringLiteral("：字段 %1 必须是非空文本（最多4096字符）")
                                  .arg(QLatin1String(key)));
    }
    value = entry.toString();
    return true;
}

bool numberField(const QJsonObject& object, const char* key, double& value,
                 bool nonnegative, QString* error, const QString& context) {
    const auto entry = object.value(QLatin1String(key));
    if (!entry.isDouble() || !std::isfinite(entry.toDouble())
        || (nonnegative && entry.toDouble() < 0)) {
        return fail(error, context + QStringLiteral("：字段 %1 必须是有限%2数值")
                                      .arg(QLatin1String(key), nonnegative ? QStringLiteral("非负") : QString()));
    }
    value = entry.toDouble();
    return true;
}

bool hasDecimalPrecision(double value, double scale) {
    const double scaled = value * scale;
    // Permit binary floating-point representation noise, not meaningful extra digits.
    const double tolerance = std::max(1e-8, std::abs(scaled)
                                     * std::numeric_limits<double>::epsilon() * 4.0);
    return std::abs(scaled - std::round(scaled)) <= tolerance;
}

QJsonObject skinJson(const Skin& skin) {
    return {{QStringLiteral("id"), skin.id}, {QStringLiteral("name"), skin.name},
            {QStringLiteral("series"), skin.series}, {QStringLiteral("condition"), skin.condition},
            {QStringLiteral("rarity"), skin.rarity}, {QStringLiteral("wear"), skin.wear},
            {QStringLiteral("price"), skin.price}, {QStringLiteral("change"), skin.change},
            {QStringLiteral("followed"), skin.followed},
            {QStringLiteral("catalogProductId"), skin.catalogProductId},
            {QStringLiteral("variant"), skin.variant},
            {QStringLiteral("skinSeries"), skin.skinSeries},
            {QStringLiteral("priceKnown"), skin.priceKnown}, {QStringLiteral("wearKnown"), skin.wearKnown},
            {QStringLiteral("changeKnown"), skin.changeKnown}, {QStringLiteral("dataSource"), skin.dataSource}};
}

QJsonObject taskJson(const Task& task) {
    // Execution status is deliberately not persisted: a saved setting is not an order.
    QJsonObject object{{QStringLiteral("id"), task.id}, {QStringLiteral("name"), task.name},
            {QStringLiteral("skinId"), task.skinId}, {QStringLiteral("minPrice"), task.minPrice},
            {QStringLiteral("maxPrice"), task.maxPrice}, {QStringLiteral("maxWear"), task.maxWear},
            {QStringLiteral("quantity"), task.quantity}, {QStringLiteral("enabled"), task.enabled},
            {QStringLiteral("condition"), task.condition}};
    const auto& source = task.importSource;
    if (!source.format.isEmpty() || !source.sourceSha256.isEmpty() || !source.dictionarySha256.isEmpty()
        || !source.productId.isEmpty() || !source.conditionId.isEmpty() || source.row != -1
        || !source.fields.isEmpty()) {
        object.insert(QStringLiteral("importSource"), QJsonObject{
            {QStringLiteral("format"), source.format}, {QStringLiteral("sourceSha256"), source.sourceSha256},
            {QStringLiteral("dictionarySha256"), source.dictionarySha256}, {QStringLiteral("row"), source.row},
            {QStringLiteral("productId"), source.productId}, {QStringLiteral("conditionId"), source.conditionId},
            {QStringLiteral("fields"), source.fields}});
    }
    return object;
}

bool parseTaskImportSource(const QJsonObject& task, TaskImportSource& source,
                           QString* error, const QString& context) {
    const auto entry = task.value(QStringLiteral("importSource"));
    if (entry.isUndefined()) return true;
    if (!entry.isObject()) return fail(error, context + QStringLiteral("：导入来源必须是对象"));
    const auto object = entry.toObject();
    if (!stringField(object, "format", source.format, error, context)
        || !stringField(object, "sourceSha256", source.sourceSha256, error, context)
        || !stringField(object, "dictionarySha256", source.dictionarySha256, error, context)
        || !stringField(object, "productId", source.productId, error, context)
        || !stringField(object, "conditionId", source.conditionId, error, context)) return false;
    static const QRegularExpression hash(QStringLiteral("^[0-9a-f]{64}$"));
    static const QRegularExpression integerId(QStringLiteral("^(0|[1-9][0-9]{0,15})$"));
    const auto row = object.value(QStringLiteral("row"));
    if (source.format != QStringLiteral("savedValue")
        || !hash.match(source.sourceSha256).hasMatch() || !hash.match(source.dictionarySha256).hasMatch()
        || !integerId.match(source.productId).hasMatch() || !integerId.match(source.conditionId).hasMatch()
        || !row.isDouble() || row.toDouble() < 0 || row.toDouble() > 499
        || std::floor(row.toDouble()) != row.toDouble() || !object.value(QStringLiteral("fields")).isObject())
        return fail(error, context + QStringLiteral("：导入来源标识、哈希或行号无效"));
    source.row = row.toInt();
    source.fields = object.value(QStringLiteral("fields")).toObject();
    const QStringList fields{QStringLiteral("成色设置栏"), QStringLiteral("最低价格设置栏"),
        QStringLiteral("最高价格设置栏"), QStringLiteral("枪名设置栏"),
        QStringLiteral("磨损度设置栏"), QStringLiteral("限量设置栏")};
    if (source.fields.size() != 7 || !source.fields.value(QStringLiteral("启用")).isBool())
        return fail(error, context + QStringLiteral("：导入来源原始字段不完整"));
    for (const auto& field : fields) {
        const auto value = source.fields.value(field);
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0
            || value.toDouble() > 9007199254740991.0 || std::floor(value.toDouble()) != value.toDouble())
            return fail(error, context + QStringLiteral("：导入来源原始字段无效"));
    }
    if (source.productId != QString::number(source.fields.value(QStringLiteral("枪名设置栏")).toInteger())
        || source.conditionId != QString::number(source.fields.value(QStringLiteral("成色设置栏")).toInteger())
        || source.fields.value(QStringLiteral("最低价格设置栏")).toDouble()
               > source.fields.value(QStringLiteral("最高价格设置栏")).toDouble())
        return fail(error, context + QStringLiteral("：导入来源与原始字段不一致"));
    return true;
}

QJsonObject runJson(const RunSettings& run) {
    return {{QStringLiteral("profile"), run.profile}, {QStringLiteral("hotkey"), run.hotkey},
            {QStringLiteral("purchaseDelayMs"), run.purchaseDelayMs},
            {QStringLiteral("enterBeforeSeconds"), run.enterBeforeSeconds},
            {QStringLiteral("dynamicDelay"), run.dynamicDelay},
            {QStringLiteral("queueFullTrigger"), run.queueFullTrigger},
            {QStringLiteral("queueFullStepMs"), run.queueFullStepMs},
            {QStringLiteral("publicityTrigger"), run.publicityTrigger},
            {QStringLiteral("publicityStepMs"), run.publicityStepMs},
            {QStringLiteral("burstClick"), run.burstClick},
            {QStringLiteral("clickIntervalMs"), run.clickIntervalMs},
            {QStringLiteral("limitOrange"), run.limitOrange}, {QStringLiteral("limitPurple"), run.limitPurple},
            {QStringLiteral("limitBlue"), run.limitBlue}, {QStringLiteral("refreshPage"), run.refreshPage},
            {QStringLiteral("skipLotteryPage"), run.skipLotteryPage},
            {QStringLiteral("skipSuccessPage"), run.skipSuccessPage},
            {QStringLiteral("autoCollect"), run.autoCollect}, {QStringLiteral("collectOsd"), run.collectOsd},
            {QStringLiteral("scheduleEnabled"), run.scheduleEnabled},
            {QStringLiteral("scheduleStart"), run.scheduleStart},
            {QStringLiteral("scheduleStop"), run.scheduleStop}};
}

// Every run setting is optional so older files keep loading with defaults;
// a value that is present must have the right type and range.
bool parseRunSettings(const QJsonObject& root, RunSettings& run, QString* error) {
    run = RunSettings{};
    const auto entry = root.value(QStringLiteral("run_settings"));
    if (entry.isUndefined()) return true;
    if (!entry.isObject()) return fail(error, QStringLiteral("run_settings 必须为对象"));
    const auto object = entry.toObject();
    const QString context = QStringLiteral("运行参数");
    auto integer = [&](const char* key, int& value, int low, int high) {
        const auto field = object.value(QLatin1String(key));
        if (field.isUndefined()) return true;
        const double number = field.toDouble(-1.0);
        if (!field.isDouble() || !std::isfinite(number) || std::floor(number) != number
            || number < low || number > high)
            return fail(error, context + QStringLiteral("：%1 必须是%2至%3的整数")
                                         .arg(QLatin1String(key)).arg(low).arg(high));
        value = static_cast<int>(number);
        return true;
    };
    auto step = [&](const char* key, double& value) {
        const auto field = object.value(QLatin1String(key));
        if (field.isUndefined()) return true;
        if (!field.isDouble() || !std::isfinite(field.toDouble()) || field.toDouble() < 0
            || field.toDouble() > 1000.0 || !hasDecimalPrecision(field.toDouble(), 10.0))
            return fail(error, context + QStringLiteral("：%1 必须在0至1000之间，且最多保留1位小数")
                                         .arg(QLatin1String(key)));
        value = field.toDouble();
        return true;
    };
    auto seconds = [&](const char* key, double& value, double low, double high) {
        const auto field = object.value(QLatin1String(key));
        if (field.isUndefined()) return true;
        const double number = field.toDouble(-1.0);
        if (!field.isDouble() || !std::isfinite(number) || number < low || number > high
            || !hasDecimalPrecision(number, 10.0))
            return fail(error, context + QStringLiteral("：%1 必须在%2至%3之间，且最多保留1位小数")
                                         .arg(QLatin1String(key)).arg(low).arg(high));
        value = number;
        return true;
    };
    auto flag = [&](const char* key, bool& value) {
        const auto field = object.value(QLatin1String(key));
        if (field.isUndefined()) return true;
        if (!field.isBool())
            return fail(error, context + QStringLiteral("：%1 必须为布尔值").arg(QLatin1String(key)));
        value = field.toBool();
        return true;
    };
    auto time = [&](const char* key, QString& value) {
        const auto field = object.value(QLatin1String(key));
        if (field.isUndefined()) return true;
        if (!field.isString() || field.toString().size() != 5
            || !QTime::fromString(field.toString(), QStringLiteral("HH:mm")).isValid())
            return fail(error, context + QStringLiteral("：%1 必须是 HH:mm 格式的时间")
                                         .arg(QLatin1String(key)));
        value = field.toString();
        return true;
    };
    const auto profile = object.value(QStringLiteral("profile"));
    if (!profile.isUndefined()) {
        if (!profile.isString() || profile.toString().trimmed().isEmpty() || profile.toString().size() > 40)
            return fail(error, context + QStringLiteral("：方案名称必须是1至40个字符的文本"));
        run.profile = profile.toString();
    }
    const auto hotkey = object.value(QStringLiteral("hotkey"));
    if (!hotkey.isUndefined()) {
        static const QRegularExpression functionKey(QStringLiteral("^F([1-9]|1[0-2])$"));
        if (!hotkey.isString() || !functionKey.match(hotkey.toString()).hasMatch())
            return fail(error, context + QStringLiteral("：运行快捷键必须是 F1 至 F12"));
        run.hotkey = hotkey.toString();
    }
    return integer("purchaseDelayMs", run.purchaseDelayMs, 0, 60000)
        && seconds("enterBeforeSeconds", run.enterBeforeSeconds, 1.0, 5.0)
        && flag("dynamicDelay", run.dynamicDelay)
        && integer("queueFullTrigger", run.queueFullTrigger, 1, 9999)
        && step("queueFullStepMs", run.queueFullStepMs)
        && integer("publicityTrigger", run.publicityTrigger, 1, 9999)
        && step("publicityStepMs", run.publicityStepMs)
        && flag("burstClick", run.burstClick)
        && integer("clickIntervalMs", run.clickIntervalMs, 1, 10000)
        && integer("limitOrange", run.limitOrange, 0, 9999)
        && integer("limitPurple", run.limitPurple, 0, 9999)
        && integer("limitBlue", run.limitBlue, 0, 9999)
        && flag("refreshPage", run.refreshPage)
        && flag("skipLotteryPage", run.skipLotteryPage)
        && flag("skipSuccessPage", run.skipSuccessPage)
        && flag("autoCollect", run.autoCollect)
        && flag("collectOsd", run.collectOsd)
        && flag("scheduleEnabled", run.scheduleEnabled)
        && time("scheduleStart", run.scheduleStart)
        && time("scheduleStop", run.scheduleStop);
}

bool parseConfig(const QJsonObject& root, QVector<Skin>& parsedSkins,
                 QVector<Task>& parsedTasks, RunSettings& parsedRun, QString* error) {
    if (!root.value(QStringLiteral("schema_version")).isDouble()
        || root.value(QStringLiteral("schema_version")).toDouble() != 1.0)
        return fail(error, QStringLiteral("不支持的配置版本：schema_version 必须为1"));
    if (!root.value(QStringLiteral("demo")).isBool())
        return fail(error, QStringLiteral("demo 必须为布尔值"));
    if (!root.value(QStringLiteral("skins")).isArray()
        || !root.value(QStringLiteral("tasks")).isArray())
        return fail(error, QStringLiteral("skins 和 tasks 必须为数组"));
    const auto skinArray = root.value(QStringLiteral("skins")).toArray();
    const auto taskArray = root.value(QStringLiteral("tasks")).toArray();
    if (skinArray.size() > kMaxItems || taskArray.size() > kMaxItems)
        return fail(error, QStringLiteral("配置条目超过10000条上限"));

    QSet<QString> skinIds, taskIds, importedRows;
    for (int i = 0; i < skinArray.size(); ++i) {
        const QString context = QStringLiteral("皮肤[%1]").arg(i + 1);
        if (!skinArray.at(i).isObject()) return fail(error, context + QStringLiteral("必须为对象"));
        const auto object = skinArray.at(i).toObject();
        Skin skin;
        if (!stringField(object, "id", skin.id, error, context)
            || !stringField(object, "name", skin.name, error, context)
            || !stringField(object, "series", skin.series, error, context)
            || !stringField(object, "condition", skin.condition, error, context)
            || !stringField(object, "rarity", skin.rarity, error, context)
            || !numberField(object, "wear", skin.wear, true, error, context)
            || !numberField(object, "price", skin.price, true, error, context)
            || !numberField(object, "change", skin.change, false, error, context)) return false;
        if (!object.value(QStringLiteral("followed")).isBool())
            return fail(error, context + QStringLiteral("：followed 必须为布尔值"));
        skin.followed = object.value(QStringLiteral("followed")).toBool();
        auto optionalText = [&](const char* key, QString& field) {
            const auto value = object.value(QLatin1String(key));
            if (value.isUndefined()) return true;
            if (!value.isString() || value.toString().size() > 4096)
                return fail(error, context + QStringLiteral("：%1 必须是最多4096字符的文本")
                                               .arg(QLatin1String(key)));
            field = value.toString();
            return true;
        };
        // Old files have no availability flags: retain their values rather than
        // silently deleting user observations. Catalogue rows explicitly save false.
        auto optionalKnown = [&](const char* key, bool& field) {
            const auto value = object.value(QLatin1String(key));
            if (value.isUndefined()) { field = true; return true; }
            if (!value.isBool())
                return fail(error, context + QStringLiteral("：%1 必须为布尔值")
                                               .arg(QLatin1String(key)));
            field = value.toBool();
            return true;
        };
        skin.dataSource = root.value(QStringLiteral("source")).toString()
                == QStringLiteral("synthetic_frontend_fixture")
            ? QStringLiteral("test_fixture") : QStringLiteral("legacy_configuration");
        // Earlier files also saved the catalogue menu colour; the 品阶 in
        // rarity replaced it. The value is still validated, then dropped.
        QString legacyMenuColor;
        if (!optionalText("catalogProductId", skin.catalogProductId)
            || !optionalText("menuColor", legacyMenuColor)
            || !optionalText("variant", skin.variant)
            || !optionalText("skinSeries", skin.skinSeries)
            || !optionalText("dataSource", skin.dataSource)
            || !optionalKnown("priceKnown", skin.priceKnown)
            || !optionalKnown("wearKnown", skin.wearKnown)
            || !optionalKnown("changeKnown", skin.changeKnown)) return false;
        if (skinIds.contains(skin.id)) return fail(error, context + QStringLiteral("：皮肤ID重复"));
        skinIds.insert(skin.id);
        parsedSkins.append(skin);
    }

    for (int i = 0; i < taskArray.size(); ++i) {
        const QString context = QStringLiteral("任务[%1]").arg(i + 1);
        if (!taskArray.at(i).isObject()) return fail(error, context + QStringLiteral("必须为对象"));
        const auto object = taskArray.at(i).toObject();
        Task task;
        double quantity = 0;
        if (!stringField(object, "id", task.id, error, context)
            || !stringField(object, "name", task.name, error, context)
            || !stringField(object, "skinId", task.skinId, error, context)
            || !numberField(object, "minPrice", task.minPrice, true, error, context)
            || !numberField(object, "maxPrice", task.maxPrice, true, error, context)
            || !numberField(object, "maxWear", task.maxWear, true, error, context)
            || !numberField(object, "quantity", quantity, true, error, context)) return false;
        if (task.minPrice > task.maxPrice)
            return fail(error, context + QStringLiteral("：最低价不得大于最高价"));
        // QString::size() counts UTF-16 QChars, matching QLineEdit::maxLength(60).
        if (task.name.size() > 60)
            return fail(error, context + QStringLiteral("：任务名称最多60个UTF-16字符单元（QChar）"));
        if (task.minPrice > 999999999.0 || task.maxPrice > 999999999.0
            || !hasDecimalPrecision(task.minPrice, 100.0)
            || !hasDecimalPrecision(task.maxPrice, 100.0))
            return fail(error, context + QStringLiteral("：任务价格不得超过999999999，且最多保留2位小数"));
        if (task.maxWear > 100.0 || !hasDecimalPrecision(task.maxWear, 1000000.0))
            return fail(error, context + QStringLiteral("：最大磨损不得超过100，且最多保留6位小数"));
        if (quantity < 0 || quantity > 9999 || std::floor(quantity) != quantity)
            return fail(error, context + QStringLiteral("：限制数量必须是0至9999的整数（0为不限）"));
        task.quantity = static_cast<int>(quantity);
        if (!object.value(QStringLiteral("enabled")).isBool())
            return fail(error, context + QStringLiteral("：enabled 必须为布尔值"));
        task.enabled = object.value(QStringLiteral("enabled")).toBool();
        const auto condition = object.value(QStringLiteral("condition"));
        task.conditionExplicit = !condition.isUndefined();
        if (!condition.isUndefined()) {
            if (!condition.isString() || condition.toString().trimmed().isEmpty()
                || condition.toString().size() > 20)
                return fail(error, context + QStringLiteral("：成色必须是1至20个字符的文本"));
            task.condition = condition.toString();
        }
        if (!skinIds.contains(task.skinId)) return fail(error, context + QStringLiteral("：引用了未知皮肤ID"));
        if (taskIds.contains(task.id)) return fail(error, context + QStringLiteral("：任务ID重复"));
        if (!parseTaskImportSource(object, task.importSource, error, context)) return false;
        if (!task.importSource.format.isEmpty()) {
            const QString origin = task.importSource.sourceSha256 + QLatin1Char(':')
                + QString::number(task.importSource.row);
            if (importedRows.contains(origin))
                return fail(error, context + QStringLiteral("：同一来源行重复，请为复制任务清除来源标识"));
            importedRows.insert(origin);
        }
        taskIds.insert(task.id);
        task.status = task.enabled ? QStringLiteral("待启动") : QStringLiteral("未启用");
        parsedTasks.append(task);
    }
    return parseRunSettings(root, parsedRun, error);
}

QJsonObject configJson(const QVector<Skin>& skins, const QVector<Task>& tasks, const RunSettings& run) {
    QJsonArray skinArray, taskArray;
    for (const auto& skin : skins) skinArray.append(skinJson(skin));
    for (const auto& task : tasks) taskArray.append(taskJson(task));
    const bool fixture = !skins.isEmpty() && std::all_of(skins.cbegin(), skins.cend(), [](const Skin& skin) {
        return skin.dataSource == QStringLiteral("test_fixture");
    });
    return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("demo"), fixture},
            {QStringLiteral("source"), fixture ? QStringLiteral("synthetic_frontend_fixture")
                                               : QStringLiteral("catalog_configuration")},
            {QStringLiteral("skins"), skinArray}, {QStringLiteral("tasks"), taskArray},
            {QStringLiteral("run_settings"), runJson(run)}};
}

QString csvCell(QString text) {
    // Avoid spreadsheet formula interpretation of imported labels.
    const QString trimmed = text.trimmed();
    if (!trimmed.isEmpty() && QStringLiteral("=+-@\t\r").contains(trimmed.front()))
        text.prepend(QLatin1Char('\''));
    text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + text + QLatin1Char('"');
}
} // namespace

AppState::AppState(QObject* parent) : QObject(parent), m_timer(new QTimer(this)) {
    setProperty("testFixture", false);
    m_timer->setInterval(1600);
    connect(m_timer, &QTimer::timeout, this, &AppState::simulateTick);
}

void AppState::loadDemo() { loadTestFixture(); }

void AppState::loadTestFixture() {
    m_testFixture = true;
    setProperty("testFixture", true);
    m_timer->stop();
    simulationRunning = false;
    simulatedScans = simulatedMatches = simulatedSuccess = 0;
    m_simulatedByTask.clear();
    configPath.clear();
    skins = {
        {QStringLiteral("demo-01"), QStringLiteral("AUG · 天命"), QStringLiteral("S6 典藏"), QStringLiteral("成色S"), QStringLiteral("橙色"), 2.18, 628, -2.4, true},
        {QStringLiteral("demo-02"), QStringLiteral("M4A1 · 星海回响"), QStringLiteral("S11 典藏"), QStringLiteral("成色S"), QStringLiteral("橙色"), 1.42, 468, 1.8, true},
        {QStringLiteral("demo-03"), QStringLiteral("AKM · 赤焰"), QStringLiteral("S9 典藏"), QStringLiteral("成色A"), QStringLiteral("紫色"), 6.35, 215, -1.2, true},
        {QStringLiteral("demo-04"), QStringLiteral("AWM · 极光"), QStringLiteral("S8 典藏"), QStringLiteral("成色S"), QStringLiteral("橙色"), 3.06, 892, -4.1, true},
        {QStringLiteral("demo-05"), QStringLiteral("Vector · 霓虹脉冲"), QStringLiteral("S10 典藏"), QStringLiteral("成色A"), QStringLiteral("紫色"), 7.12, 176, 3.2, true},
        {QStringLiteral("demo-06"), QStringLiteral("SCAR-H · 暮光"), QStringLiteral("S7 典藏"), QStringLiteral("成色S"), QStringLiteral("橙色"), 2.64, 538, -0.7, true},
        {QStringLiteral("demo-07"), QStringLiteral("MP5 · 冰川"), QStringLiteral("S9 典藏"), QStringLiteral("成色A"), QStringLiteral("蓝色"), 8.48, 92, 0.0, true},
        {QStringLiteral("demo-08"), QStringLiteral("G3 · 深空"), QStringLiteral("S10 典藏"), QStringLiteral("成色A"), QStringLiteral("紫色"), 5.73, 264, -2.0, true},
        {QStringLiteral("demo-09"), QStringLiteral("M700 · 沙漠之眼"), QStringLiteral("S8 典藏"), QStringLiteral("成色B"), QStringLiteral("紫色"), 14.25, 158, 1.3, true},
        {QStringLiteral("demo-10"), QStringLiteral("P90 · 碧海"), QStringLiteral("S11 典藏"), QStringLiteral("成色S"), QStringLiteral("蓝色"), 4.05, 126, -0.8, true},
        {QStringLiteral("demo-11"), QStringLiteral("SR-3M · 幻影"), QStringLiteral("S7 典藏"), QStringLiteral("成色A"), QStringLiteral("紫色"), 9.16, 198, 2.1, false},
        {QStringLiteral("demo-12"), QStringLiteral("M249 · 机械黎明"), QStringLiteral("S6 典藏"), QStringLiteral("成色B"), QStringLiteral("橙色"), 12.28, 384, -3.6, false}
    };
    for (auto& skin : skins) {
        skin.priceKnown = skin.wearKnown = skin.changeKnown = true;
        skin.dataSource = QStringLiteral("test_fixture");
    }
    tasks = {
        {QStringLiteral("task-01"), QStringLiteral("天命 · 低磨收藏"), QStringLiteral("demo-01"), 100, 650, 5, 3, true, QStringLiteral("待启动"), QStringLiteral("成色S")},
        {QStringLiteral("task-02"), QStringLiteral("星海回响 · 价格观察"), QStringLiteral("demo-02"), 100, 450, 5, 2, true, QStringLiteral("待启动"), QStringLiteral("成色S")},
        {QStringLiteral("task-03"), QStringLiteral("赤焰 · 区间筛选"), QStringLiteral("demo-03"), 150, 240, 10, 4, true, QStringLiteral("待启动"), QStringLiteral("成色A")},
        {QStringLiteral("task-04"), QStringLiteral("极光 · 收藏备选"), QStringLiteral("demo-04"), 600, 850, 5, 1, false, QStringLiteral("未启用"), QStringLiteral("成色S")},
        {QStringLiteral("task-05"), QStringLiteral("暮光 · 低价提醒"), QStringLiteral("demo-06"), 300, 550, 5, 2, true, QStringLiteral("待启动"), QStringLiteral("成色S")}
    };
    run = RunSettings{};
    logs.clear();
    addLog(QStringLiteral("INFO"), QStringLiteral("离线前端已就绪。全部皮肤名称组合、价格与执行结果均为模拟数据。"));
    emit changed();
}

bool AppState::saveTo(const QString& path, QString* error) const {
    if (error) error->clear();
    const auto root = configJson(skins, tasks, run);
    QVector<Skin> checkedSkins;
    QVector<Task> checkedTasks;
    RunSettings checkedRun;
    if (!parseConfig(root, checkedSkins, checkedTasks, checkedRun, error)) return false;
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (bytes.size() > kMaxConfigBytes) return fail(error, QStringLiteral("配置文件超过8MB上限"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(error, file.errorString());
    if (file.write(bytes) != bytes.size()) return fail(error, file.errorString());
    if (!file.commit()) return fail(error, file.errorString());
    return true;
}

bool AppState::loadFrom(const QString& path, QString* error) {
    if (error) error->clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
    if (file.size() > kMaxConfigBytes) return fail(error, QStringLiteral("配置文件超过8MB上限"));
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) return fail(error, file.errorString());
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail(error, QStringLiteral("JSON格式错误：") + parseError.errorString());
    if (!document.isObject()) return fail(error, QStringLiteral("配置根节点必须为对象"));
    QVector<Skin> parsedSkins;
    QVector<Task> parsedTasks;
    RunSettings parsedRun;
    if (!parseConfig(document.object(), parsedSkins, parsedTasks, parsedRun, error)) return false;

    // Commit only after the complete input has passed validation.
    m_timer->stop();
    simulationRunning = false;
    skins = std::move(parsedSkins);
    tasks = std::move(parsedTasks);
    run = parsedRun;
    m_testFixture = false;
    setProperty("testFixture", false);
    simulatedScans = simulatedMatches = simulatedSuccess = 0;
    m_simulatedByTask.clear();
    configPath = path;
    addLog(QStringLiteral("INFO"), QStringLiteral("已导入配置；运行状态与计数已重置。"));
    emit changed();
    return true;
}

bool AppState::exportPricesCsv(const QString& path, QString* error) const {
    if (error) error->clear();
    QString csv = QStringLiteral("data_source,skin_id,name,series,condition,rarity,wear,price,change_percent,followed,exported_at\r\n");
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    for (const auto& skin : skins) {
        if (!std::isfinite(skin.price) || skin.price < 0 || !std::isfinite(skin.wear)
            || skin.wear < 0 || !std::isfinite(skin.change))
            return fail(error, QStringLiteral("价格数据中存在无效数值"));
        csv += csvCell(skin.dataSource == QStringLiteral("test_fixture")
                           ? QStringLiteral("DEMO_SYNTHETIC") : skin.dataSource)
            + QLatin1Char(',') + csvCell(skin.id) + QLatin1Char(',')
            + csvCell(skin.name) + QLatin1Char(',') + csvCell(skin.series) + QLatin1Char(',')
            + csvCell(skin.condition) + QLatin1Char(',') + csvCell(skin.rarity) + QLatin1Char(',')
            + (skin.wearKnown ? QString::number(skin.wear, 'f', 6) : QString()) + QLatin1Char(',')
            + (skin.priceKnown ? QString::number(skin.price, 'f', 2) : QString()) + QLatin1Char(',')
            + (skin.changeKnown ? QString::number(skin.change, 'f', 2) : QString()) + QLatin1Char(',')
            + (skin.followed ? QStringLiteral("true") : QStringLiteral("false")) + QLatin1Char(',')
            + csvCell(now) + QStringLiteral("\r\n");
    }
    const QByteArray bytes = QByteArray("\xEF\xBB\xBF", 3) + csv.toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(error, file.errorString());
    if (file.write(bytes) != bytes.size()) return fail(error, file.errorString());
    if (!file.commit()) return fail(error, file.errorString());
    return true;
}

void AppState::notifyChanged() { emit changed(); }

void AppState::addLog(const QString& level, const QString& message) {
    logs.prepend({QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), level, message});
    if (logs.size() > 500) logs.resize(500);
}

void AppState::startSimulation() {
    // A catalogue/configuration is not an observed listing or a replay fixture.
    if (!m_testFixture || !property("testFixture").toBool() || simulationRunning) return;
    bool anyEnabled = false;
    for (const auto& task : tasks) anyEnabled |= task.enabled;
    if (!anyEnabled) {
        addLog(QStringLiteral("WARN"), QStringLiteral("请先启用至少一个演示任务。"));
        emit changed();
        return;
    }
    simulationRunning = true;
    m_timer->start();
    addLog(QStringLiteral("INFO"), QStringLiteral("本地演示开始：仅推进模拟状态，不读取游戏，不发送点击，不产生订单。"));
    emit changed();
}

void AppState::pauseSimulation() {
    if (!simulationRunning) return;
    simulationRunning = false;
    m_timer->stop();
    for (auto& task : tasks)
        if (task.enabled && task.status != QStringLiteral("演示已完成")) task.status = QStringLiteral("演示已暂停");
    addLog(QStringLiteral("INFO"), QStringLiteral("本地演示已暂停。"));
    emit changed();
}

void AppState::resetTaskSimulation(const QString& id) {
    for (auto& task : tasks) {
        if (task.id != id) continue;
        m_simulatedByTask.remove(id);
        task.status = task.enabled ? QStringLiteral("待启动") : QStringLiteral("未启用");
        addLog(QStringLiteral("INFO"), QStringLiteral("已重置任务“%1”的模拟进度；累计扫描、命中与成功统计保留。")
                   .arg(task.name));
        emit changed();
        return;
    }
}

void AppState::simulateTick() {
    if (!m_testFixture || !property("testFixture").toBool() || !simulationRunning) return;
    bool hasPending = false;
    for (auto& task : tasks) {
        if (!task.enabled) { task.status = QStringLiteral("未启用"); continue; }
        if (task.quantity > 0 && m_simulatedByTask.value(task.id) >= task.quantity) {
            task.status = QStringLiteral("演示已完成");
            continue;
        }
        hasPending = true;
        if (simulatedScans < std::numeric_limits<int>::max()) ++simulatedScans;
        const Skin* skin = nullptr;
        for (const auto& entry : skins) if (entry.id == task.skinId) { skin = &entry; break; }
        if (!skin || task.quantity < 0 || !std::isfinite(task.minPrice)
            || !std::isfinite(task.maxPrice) || !std::isfinite(task.maxWear)
            || task.minPrice < 0 || task.maxWear < 0 || task.minPrice > task.maxPrice) {
            task.status = QStringLiteral("演示配置无效");
            continue;
        }
        const bool conditionMatches = task.condition == QStringLiteral("不限")
            || task.condition == skin->condition;
        if (!skin->priceKnown || !skin->wearKnown
            || !std::isfinite(skin->price) || !std::isfinite(skin->wear)
            || skin->price < task.minPrice || skin->price > task.maxPrice
            || skin->wear < 0 || skin->wear > task.maxWear || !conditionMatches) {
            task.status = QStringLiteral("演示等待条件");
            continue;
        }
        if (simulatedMatches < std::numeric_limits<int>::max()) ++simulatedMatches;
        if (simulatedSuccess < std::numeric_limits<int>::max()) ++simulatedSuccess;
        const int done = ++m_simulatedByTask[task.id];
        task.status = task.quantity > 0 && done >= task.quantity
            ? QStringLiteral("演示已完成") : QStringLiteral("演示进行中");
        addLog(QStringLiteral("DEMO"), QStringLiteral("%1：模拟匹配价格 %2，模拟完成 %3/%4；无实际订单。")
                   .arg(task.name, QString::number(skin->price, 'f', 2)).arg(done)
                   .arg(task.quantity == 0 ? QStringLiteral("不限") : QString::number(task.quantity)));
    }
    if (!hasPending) {
        simulationRunning = false;
        m_timer->stop();
        addLog(QStringLiteral("INFO"), QStringLiteral("启用的演示任务已全部结束。"));
    }
    emit changed();
}

bool decodeV1Config(const QByteArray& bytes, QVector<Skin>& skins, QVector<Task>& tasks,
                    RunSettings& run, QString* error) {
    if (error) error->clear();
    if (bytes.size() > kMaxConfigBytes) return fail(error, QStringLiteral("配置文件超过8MB上限"));
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail(error, QStringLiteral("JSON格式错误：") + parseError.errorString());
    if (!document.isObject()) return fail(error, QStringLiteral("配置根节点必须为对象"));

    // Parse into locals first: a rejected input never touches the outputs.
    QVector<Skin> parsedSkins;
    QVector<Task> parsedTasks;
    RunSettings parsedRun;
    if (!parseConfig(document.object(), parsedSkins, parsedTasks, parsedRun, error)) return false;
    skins = std::move(parsedSkins);
    tasks = std::move(parsedTasks);
    run = parsedRun;
    return true;
}

QJsonObject encodeV1Config(const QVector<Skin>& skins, const QVector<Task>& tasks,
                           const RunSettings& run) {
    return configJson(skins, tasks, run);
}

QJsonObject encodeRunSettings(const RunSettings& run) { return runJson(run); }
