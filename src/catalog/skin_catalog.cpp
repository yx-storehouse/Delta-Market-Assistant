#include "catalog/skin_catalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringDecoder>
#include <QUuid>

namespace relink::catalog {
namespace {
constexpr qsizetype MaxBytes = 4 * 1024 * 1024;
constexpr qsizetype MaxSkins = 20000;
constexpr qsizetype MaxSeasons = 999;
const QString Schema = QStringLiteral("relink-skin-catalog-v2");
const QString LegacySchema = QStringLiteral("relink-skin-catalog-v1");

bool fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}

bool text(const QJsonObject& object, const char* name, bool required,
          QString* out, QString* error, qsizetype max = 160) {
    const auto value = object.value(QLatin1String(name));
    if (!value.isString() && !(value.isNull() || value.isUndefined()))
        return fail(error, QStringLiteral("字段 %1 必须是文本").arg(QLatin1String(name)));
    const auto s = value.toString();
    if (required && s.trimmed().isEmpty())
        return fail(error, QStringLiteral("缺少字段 %1").arg(QLatin1String(name)));
    if (s != s.trimmed() || s.size() > max)
        return fail(error, QStringLiteral("字段 %1 长度或首尾空白不合法").arg(QLatin1String(name)));
    for (const auto ch : s) {
        if (ch.category() == QChar::Other_Control || ch == QChar(0x2028) || ch == QChar(0x2029))
            return fail(error, QStringLiteral("字段 %1 含控制字符").arg(QLatin1String(name)));
    }
    *out = s;
    return true;
}

QString compact(const QString& value) {
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    return value.normalized(QString::NormalizationForm_KC).remove(spaces);
}

// "S6|AUG 突击步枪 - 天命": the collection runner identifies a product, and
// checks the game's title, by this name. It follows from the other fields, so
// a missing or differently written name (no "Sx|" prefix, as older dialog
// additions and hand-filled templates had) is rebuilt; the same rule runs in
// tests/manual/collection_run_config.py. A name already equal up to spacing
// keeps its original spelling.
void canonicalizeDisplayName(Skin& skin) {
    QString suffix = skin.skinSeries;
    if (!skin.skinSeries.isEmpty() && !skin.variantLabel.isEmpty()) suffix += QStringLiteral(" - ");
    suffix += skin.variantLabel;
    const QString name = skin.weapon + (suffix.isEmpty() ? QString() : QStringLiteral(" - ") + suffix);
    const auto parts = skin.displayName.split(QLatin1Char('|'));
    if (parts.size() == 2 && parts[0].trimmed() == skin.seasonId && compact(parts[1]) == compact(name)) return;
    skin.displayName = skin.seasonId + QStringLiteral("|") + name;
}

bool knownKeys(const QJsonObject& object, const QSet<QString>& allowed, QString* error) {
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key()))
            return fail(error, QStringLiteral("不支持的字段：%1").arg(it.key()));
    return true;
}

bool validSeason(const Season& season, QString* error) {
    static const QRegularExpression id(QStringLiteral("^S[1-9][0-9]{0,2}$"));
    if (!id.match(season.id).hasMatch())
        return fail(error, QStringLiteral("赛季编号应为 S1 至 S999"));
    if (season.label.trimmed().isEmpty()) return fail(error, QStringLiteral("赛季名称为空"));
    return true;
}

QJsonObject seasonJson(const Season& season) {
    return {{"id", season.id}, {"label", season.label}};
}
QJsonObject skinJson(const Skin& skin) {
    return {{"product_id", skin.productId}, {"season_id", skin.seasonId},
            {"season_label", skin.seasonLabel}, {"weapon", skin.weapon},
            {"skin_series", skin.skinSeries}, {"variant_label", skin.variantLabel},
            {"grade", skin.grade}, {"display_name", skin.displayName}, {"thumbnail_path", skin.thumbnailPath}};
}

bool merge(const Catalog& base, const Catalog& extra, Catalog* result, QString* error) {
    Catalog next = base;
    for (const auto& season : extra.seasons) {
        const auto* existing = next.findSeason(season.id);
        if (existing && existing->label != season.label)
            return fail(error, QStringLiteral("赛季 %1 已存在，名称与原记录不一致").arg(season.id));
        if (!existing) next.seasons.append(season);
    }
    QSet<QString> ids;
    for (const auto& skin : base.skins) ids.insert(skin.productId);
    for (const auto& skin : extra.skins) {
        if (ids.contains(skin.productId))
            return fail(error, QStringLiteral("商品 ID %1 已存在，未覆盖原数据").arg(skin.productId));
        const auto* season = next.findSeason(skin.seasonId);
        if (!season || season->label != skin.seasonLabel)
            return fail(error, QStringLiteral("商品 %1 的赛季与目录不一致").arg(skin.productId));
        ids.insert(skin.productId);
        next.skins.append(skin);
    }
    if (next.seasons.size() > MaxSeasons || next.skins.size() > MaxSkins)
        return fail(error, QStringLiteral("目录条数超过上限"));
    *result = next;
    return true;
}

bool readExtension(const QString& path, QByteArray* bytes, bool* exists, QString* error) {
    const QFileInfo info(path);
    *exists = info.exists();
    bytes->clear();
    if (!*exists) return true;
    if (!info.isFile() || info.isSymLink())
        return fail(error, QStringLiteral("扩展目录路径不是普通文件"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("读取皮肤扩展数据失败：%1").arg(file.errorString()));
    if (file.size() > MaxBytes) return fail(error, QStringLiteral("皮肤扩展数据超过 4 MiB"));
    *bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
        return fail(error, QStringLiteral("读取皮肤扩展数据失败：%1").arg(file.errorString()));
    return true;
}

} // namespace

const QStringList& gradeLabels() {
    static const QStringList labels{QStringLiteral("传说品阶"), QStringLiteral("史诗品阶"), QStringLiteral("稀有品阶")};
    return labels;
}

QString gradeFromMenuColor(const QString& color) {
    if (color == QStringLiteral("orange") || color == QStringLiteral("red")) return QStringLiteral("传说品阶");
    if (color == QStringLiteral("purple")) return QStringLiteral("史诗品阶");
    if (color == QStringLiteral("blue")) return QStringLiteral("稀有品阶");
    return {};
}

const Skin* Catalog::findSkin(const QString& id) const {
    for (const auto& skin : skins) if (skin.productId == id) return &skin;
    return nullptr;
}
const Season* Catalog::findSeason(const QString& id) const {
    for (const auto& season : seasons) if (season.id == id) return &season;
    return nullptr;
}

bool parseCatalog(const QByteArray& bytes, Catalog* result, QString* error) {
    if (error) error->clear();
    if (!result) return fail(error, QStringLiteral("目录输出对象为空"));
    if (bytes.isEmpty() || bytes.size() > MaxBytes)
        return fail(error, QStringLiteral("目录文件为空或超过 4 MiB"));
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder(bytes);
    Q_UNUSED(decoded);
    if (decoder.hasError()) return fail(error, QStringLiteral("目录文件必须使用有效 UTF-8 编码"));
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("皮肤目录 JSON 格式错误：%1").arg(parseError.errorString()));
    const auto root = document.object();
    if (!knownKeys(root, {"schema", "seasons", "skins"}, error)) return false;
    const bool legacy = root.value("schema").toString() == LegacySchema;
    if (!legacy && root.value("schema").toString() != Schema)
        return fail(error, QStringLiteral("皮肤目录 schema 不匹配"));
    if (!root.value("seasons").isArray() || !root.value("skins").isArray())
        return fail(error, QStringLiteral("seasons 与 skins 必须为数组"));
    const auto seasons = root.value("seasons").toArray();
    const auto skins = root.value("skins").toArray();
    if (seasons.size() > MaxSeasons || skins.size() > MaxSkins)
        return fail(error, QStringLiteral("目录条数超过上限"));
    Catalog parsed;
    QSet<QString> seasonIds, productIds;
    for (const auto& value : seasons) {
        if (!value.isObject()) return fail(error, QStringLiteral("赛季条目必须是对象"));
        const auto object = value.toObject();
        Season season;
        if (!knownKeys(object, {"id", "label"}, error)
            || !text(object, "id", true, &season.id, error)
            || !text(object, "label", true, &season.label, error)
            || !validSeason(season, error)) return false;
        if (seasonIds.contains(season.id))
            return fail(error, QStringLiteral("目录包含重复赛季：%1").arg(season.id));
        seasonIds.insert(season.id);
        parsed.seasons.append(season);
    }
    static const QRegularExpression productId(QStringLiteral("^(?:[1-9][0-9]{0,17}|user:[a-zA-Z0-9][a-zA-Z0-9._-]{0,79})$"));
    static const QSet<QString> colors{"red", "orange", "purple", "blue", "unknown"};
    const QSet<QString> skinKeys = legacy
        ? QSet<QString>{"product_id", "season_id", "season_label", "weapon", "skin_series",
                        "variant_label", "menu_color", "game_quality_name", "display_name", "thumbnail_path"}
        : QSet<QString>{"product_id", "season_id", "season_label", "weapon", "skin_series",
                        "variant_label", "grade", "display_name", "thumbnail_path"};
    for (const auto& value : skins) {
        if (!value.isObject()) return fail(error, QStringLiteral("皮肤条目必须是对象"));
        const auto object = value.toObject();
        Skin skin;
        QString menuColor, qualityName;
        if (!knownKeys(object, skinKeys, error)
            || !text(object, "product_id", true, &skin.productId, error)
            || !text(object, "season_id", true, &skin.seasonId, error)
            || !text(object, "season_label", true, &skin.seasonLabel, error)
            || !text(object, "weapon", true, &skin.weapon, error)
            || !text(object, "skin_series", false, &skin.skinSeries, error)
            || !text(object, "variant_label", false, &skin.variantLabel, error)
            || (legacy ? !text(object, "menu_color", true, &menuColor, error)
                             || !text(object, "game_quality_name", false, &qualityName, error)
                       : !text(object, "grade", false, &skin.grade, error))
            || !text(object, "display_name", false, &skin.displayName, error, 320)
            || !text(object, "thumbnail_path", false, &skin.thumbnailPath, error)) return false;
        if (!productId.match(skin.productId).hasMatch())
            return fail(error, QStringLiteral("商品 ID 应为原始数字 ID 或 user: 开头的自定义 ID"));
        if (legacy) {
            if (!colors.contains(menuColor))
                return fail(error, QStringLiteral("菜单颜色应为 red、orange、purple、blue 或 unknown"));
            // The old dialog saved free text here. A text naming a 品阶 (with or
            // without the 品阶 suffix) wins; anything else falls back to the colour.
            const QString named = qualityName.endsWith(QStringLiteral("品阶")) ? qualityName
                                                                              : qualityName + QStringLiteral("品阶");
            skin.grade = gradeLabels().contains(named) ? named : gradeFromMenuColor(menuColor);
        }
        if (!skin.grade.isEmpty() && !gradeLabels().contains(skin.grade))
            return fail(error, QStringLiteral("商品 %1 的品阶（grade）应为 传说品阶、史诗品阶、稀有品阶 或空白（未记录）")
                                   .arg(skin.productId));
        if (!skin.thumbnailPath.isEmpty())
            return fail(error, QStringLiteral("当前版本的皮肤缩略图字段应留空"));
        if (!skin.variantLabel.isEmpty() && skin.variantLabel != QStringLiteral("极品")
            && skin.variantLabel != QStringLiteral("优品"))
            return fail(error, QStringLiteral("版本标记应为 极品、优品 或空白"));
        const auto* season = parsed.findSeason(skin.seasonId);
        if (!season || season->label != skin.seasonLabel)
            return fail(error, QStringLiteral("商品 %1 缺少一致的赛季定义").arg(skin.productId));
        canonicalizeDisplayName(skin);
        if (skin.displayName.size() > 320)
            return fail(error, QStringLiteral("商品 %1 的显示名称过长").arg(skin.productId));
        if (productIds.contains(skin.productId))
            return fail(error, QStringLiteral("目录包含重复商品 ID：%1").arg(skin.productId));
        productIds.insert(skin.productId);
        parsed.skins.append(skin);
    }
    *result = parsed;
    return true;
}

QByteArray serializeCatalog(const Catalog& catalog) {
    QJsonArray seasons, skins;
    for (const auto& season : catalog.seasons) seasons.append(seasonJson(season));
    for (const auto& skin : catalog.skins) skins.append(skinJson(skin));
    return QJsonDocument(QJsonObject{{"schema", Schema}, {"seasons", seasons}, {"skins", skins}})
        .toJson(QJsonDocument::Indented);
}

QString nextUserProductId() {
    return QStringLiteral("user:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool CatalogStore::load(const QByteArray& builtinBytes, const QString& extensionPath, QString* error) {
    if (error) error->clear();
    if (extensionPath.isEmpty()) return fail(error, QStringLiteral("扩展目录保存路径为空"));
    Catalog builtin, extension, combined;
    if (!parseCatalog(builtinBytes, &builtin, error)) return false;
    const auto absolute = QFileInfo(extensionPath).absoluteFilePath();
    QByteArray bytes;
    bool existed = false;
    if (!readExtension(absolute, &bytes, &existed, error)) return false;
    if (existed && !parseCatalog(bytes, &extension, error)) return false;
    if (!merge(builtin, extension, &combined, error)) return false;
    m_builtin = builtin;
    m_extension = extension;
    m_catalog = combined;
    m_extensionPath = absolute;
    m_extensionBytes = bytes;
    m_extensionExisted = existed;
    m_loaded = true;
    return true;
}

bool CatalogStore::appendSeason(const Season& season, const QVector<Skin>& skins, QString* error) {
    if (error) error->clear();
    if (m_catalog.findSeason(season.id))
        return fail(error, QStringLiteral("赛季 %1 已存在；请在已有赛季中追加皮肤").arg(season.id));
    Catalog addition;
    addition.seasons.append(season);
    addition.skins = skins;
    return commitAddition(addition, error);
}

bool CatalogStore::appendSkins(const QVector<Skin>& skins, QString* error) {
    if (error) error->clear();
    if (skins.isEmpty()) return fail(error, QStringLiteral("请至少添加一项皮肤"));
    Catalog addition;
    addition.skins = skins;
    QSet<QString> seen;
    for (const auto& skin : skins) {
        if (!seen.contains(skin.seasonId)) {
            const auto* season = m_catalog.findSeason(skin.seasonId);
            if (!season) return fail(error, QStringLiteral("请先创建赛季 %1").arg(skin.seasonId));
            addition.seasons.append(*season);
            seen.insert(skin.seasonId);
        }
    }
    return commitAddition(addition, error);
}

bool CatalogStore::importJson(const QByteArray& bytes, QString* error) {
    Catalog addition;
    if (!parseCatalog(bytes, &addition, error)) return false;
    if (addition.seasons.isEmpty() && addition.skins.isEmpty())
        return fail(error, QStringLiteral("导入文件中没有赛季或皮肤"));
    return commitAddition(addition, error);
}

bool CatalogStore::commitAddition(const Catalog& addition, QString* error) {
    if (error) error->clear();
    if (!m_loaded) return fail(error, QStringLiteral("请先加载皮肤目录"));
    Catalog checked, nextCatalog, nextExtension;
    if (!parseCatalog(serializeCatalog(addition), &checked, error)
        || !merge(m_catalog, checked, &nextCatalog, error)
        || !merge(m_extension, checked, &nextExtension, error)) return false;
    if (m_commitValidator) {
        QString validationError;
        if (!m_commitValidator(nextCatalog, &validationError))
            return fail(error, validationError.isEmpty()
                ? QStringLiteral("新增目录与当前用户配置冲突，原数据未改动") : validationError);
    }
    const auto bytes = serializeCatalog(nextExtension);
    if (bytes.size() > MaxBytes) return fail(error, QStringLiteral("皮肤扩展数据超过 4 MiB"));
    const QFileInfo target(m_extensionPath);
    if (!QDir().mkpath(target.absolutePath()))
        return fail(error, QStringLiteral("创建皮肤数据目录失败"));
    QLockFile lock(m_extensionPath + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("皮肤目录正在由其他窗口更新，请稍后重试"));
    QByteArray current;
    bool existed = false;
    if (!readExtension(m_extensionPath, &current, &existed, error)) return false;
    if (existed != m_extensionExisted || current != m_extensionBytes)
        return fail(error, QStringLiteral("皮肤扩展文件已被修改，请重新打开目录后再追加；现有数据已保留"));
    // The first save over an earlier v1 file (menu colours) keeps its bytes,
    // so a previous program version can still be restored with its data.
    if (existed && current.contains("\"relink-skin-catalog-v1\"")) {
        const QString backup = m_extensionPath + QStringLiteral(".before-grade-v2.bak");
        if (!QFileInfo::exists(backup)) {
            QSaveFile copy(backup);
            copy.setDirectWriteFallback(false);
            if (!copy.open(QIODevice::WriteOnly) || copy.write(current) != current.size() || !copy.commit())
                return fail(error, QStringLiteral("备份旧版皮肤扩展数据失败，原数据未改动：%1").arg(copy.errorString()));
        }
    }
    QSaveFile file(m_extensionPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, QStringLiteral("保存皮肤目录失败：%1").arg(file.errorString()));
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return fail(error, QStringLiteral("写入皮肤目录失败：%1").arg(file.errorString()));
    }
    if (!file.commit())
        return fail(error, QStringLiteral("提交皮肤目录失败：%1").arg(file.errorString()));
    m_extension = nextExtension;
    m_catalog = nextCatalog;
    m_extensionBytes = bytes;
    m_extensionExisted = true;
    return true;
}

QByteArray CatalogStore::exportJson() const { return serializeCatalog(m_catalog); }
QByteArray CatalogStore::extensionJson() const { return serializeCatalog(m_extension); }

} // namespace relink::catalog
