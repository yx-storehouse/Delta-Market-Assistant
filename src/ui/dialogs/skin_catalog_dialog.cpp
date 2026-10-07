#include "skin_catalog_dialog.h"
#include "catalog/skin_catalog.h"
#include "ui/presentation/ui_helpers.h"
#include "widgets.h"

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>

namespace relink::ui {
namespace {
QString colorLabel(const QString& value) {
    if (value == QStringLiteral("red")) return QStringLiteral("红色");
    if (value == QStringLiteral("orange")) return QStringLiteral("橙色");
    if (value == QStringLiteral("purple")) return QStringLiteral("紫色");
    if (value == QStringLiteral("blue")) return QStringLiteral("蓝色");
    return QStringLiteral("未记录");
}
QString visibleValue(const QString& value) {
    return value.isEmpty() ? QStringLiteral("—") : value;
}
bool saveJson(const QString& path, const QByteArray& bytes, QString* error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}
QLineEdit* input(const QString& name, const QString& placeholder = QString()) {
    auto* edit = new QLineEdit;
    edit->setObjectName(name);
    edit->setPlaceholderText(placeholder);
    edit->setMaxLength(160);
    edit->setClearButtonEnabled(true);
    return edit;
}
}

SkinCatalogDialog::SkinCatalogDialog(catalog::CatalogStore* store, QWidget* parent)
    : QDialog(parent), m_store(store)
{
    setObjectName(QStringLiteral("skinCatalogDialog"));
    setWindowTitle(QStringLiteral("皮肤目录"));
    resize(1050, 740);
    setMinimumSize(840, 620);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* content = new QFrame;
    content->setObjectName(QStringLiteral("dialogContent"));
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(24, 22, 24, 20);
    body->setSpacing(14);
    auto* heading = new QHBoxLayout;
    heading->addWidget(label(QStringLiteral("皮肤目录"), QStringLiteral("dialogTitle")));
    heading->addStretch();
    m_count = label(QString(), QStringLiteral("catalogCountLabel"));
    heading->addWidget(m_count);
    body->addLayout(heading);
    auto* caption = label(QStringLiteral("赛季、武器与皮肤固定属性统一维护；成色、价格和磨损仍在任务中设置。"),
                          QStringLiteral("cardCaption"));
    caption->setWordWrap(true);
    body->addWidget(caption);

    m_tabs = new QTabWidget;
    m_tabs->setObjectName(QStringLiteral("catalogTabs"));
    m_tabs->setDocumentMode(true);
    // Explicitly neutral tabs even when this dialog is embedded without the
    // application stylesheet. Menu colors are data labels, not UI accents.
    m_tabs->setStyleSheet(QStringLiteral(
        "QTabWidget::pane { border: 1px solid #E5E5E5; border-radius: 8px; background: #FFFFFF; }"
        "QTabBar::tab { color: #5E5E5E; background: #F3F3F3; padding: 10px 18px; margin-right: 4px; "
        "border-top-left-radius: 6px; border-top-right-radius: 6px; }"
        "QTabBar::tab:selected { color: #1F1F1F; background: #FFFFFF; border-bottom: 2px solid #1F1F1F; }"));

    auto* browse = new QWidget;
    auto* browseLayout = new QVBoxLayout(browse);
    browseLayout->setContentsMargins(16, 16, 16, 16);
    browseLayout->setSpacing(12);
    auto* filters = new QHBoxLayout;
    m_search = input(QStringLiteral("catalogSearch"), QStringLiteral("搜索武器、系列、赛季或商品 ID"));
    filters->addWidget(m_search, 1);
    m_filter = new FluentComboBox;
    m_filter->setObjectName(QStringLiteral("catalogSeasonFilter"));
    m_filter->setMinimumWidth(200);
    filters->addWidget(m_filter);
    browseLayout->addLayout(filters);
    m_table = table({QStringLiteral("赛季"), QStringLiteral("武器"), QStringLiteral("皮肤系列"),
                     QStringLiteral("极品/优品"), QStringLiteral("菜单颜色"), QStringLiteral("游戏品质名"),
                     QStringLiteral("商品 ID")}, QStringLiteral("catalogTable"), RowStyle::Lines);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->setColumnWidth(0, 72);
    m_table->setColumnWidth(1, 170);
    m_table->setColumnWidth(2, 145);
    m_table->setColumnWidth(3, 90);
    m_table->setColumnWidth(4, 90);
    m_table->setColumnWidth(5, 100);
    m_table->setColumnWidth(6, 110);
    browseLayout->addWidget(m_table, 1);
    auto* browseHint = label(QStringLiteral("目录不包含实时报价；皮肤缩略图暂时留空。游戏品质名未核实时保持空白。"),
                             QStringLiteral("cardCaption"));
    browseHint->setWordWrap(true);
    browseLayout->addWidget(browseHint);
    m_tabs->addTab(browse, QStringLiteral("全部皮肤"));

    auto* add = new QWidget;
    auto* addLayout = new QVBoxLayout(add);
    addLayout->setContentsMargins(20, 18, 20, 18);
    addLayout->setSpacing(12);
    auto* addHint = label(QStringLiteral("向新赛季或已有赛季追加皮肤。新增条目单独保存，既有目录不覆盖。"),
                          QStringLiteral("cardCaption"));
    addHint->setWordWrap(true);
    addLayout->addWidget(addHint);
    auto* form = new QGridLayout;
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(5);
    const auto field = [&](const QString& title, QWidget* widget, int row, int col, int span = 1) {
        form->addWidget(label(title, QStringLiteral("fieldLabel")), row, col, 1, span);
        form->addWidget(widget, row + 1, col, 1, span);
    };
    m_season = new FluentComboBox;
    m_season->setObjectName(QStringLiteral("catalogAddSeason"));
    field(QStringLiteral("所属赛季"), m_season, 0, 0, 2);
    m_seasonFields = new QWidget;
    auto* newSeason = new QGridLayout(m_seasonFields);
    newSeason->setContentsMargins(0, 0, 0, 0);
    newSeason->setHorizontalSpacing(18);
    newSeason->setVerticalSpacing(5);
    m_seasonNumber = new FluentSpinBox;
    m_seasonNumber->setObjectName(QStringLiteral("catalogSeasonNumber"));
    m_seasonNumber->setRange(1, 999);
    m_seasonNumber->setPrefix(QStringLiteral("S"));
    m_seasonName = input(QStringLiteral("catalogSeasonName"), QStringLiteral("填写新赛季的实际名称"));
    newSeason->addWidget(label(QStringLiteral("赛季编号"), QStringLiteral("fieldLabel")), 0, 0);
    newSeason->addWidget(label(QStringLiteral("赛季名称"), QStringLiteral("fieldLabel")), 0, 1);
    newSeason->addWidget(m_seasonNumber, 1, 0);
    newSeason->addWidget(m_seasonName, 1, 1);
    newSeason->setColumnStretch(0, 1);
    newSeason->setColumnStretch(1, 1);
    form->addWidget(m_seasonFields, 2, 0, 1, 2);
    m_weapon = input(QStringLiteral("catalogWeapon"), QStringLiteral("武器名称与类型"));
    m_skinSeries = input(QStringLiteral("catalogSkinSeries"), QStringLiteral("原菜单未标注时留空"));
    field(QStringLiteral("武器 *"), m_weapon, 3, 0);
    field(QStringLiteral("皮肤系列"), m_skinSeries, 3, 1);
    m_variant = new FluentComboBox;
    m_variant->setObjectName(QStringLiteral("catalogVariant"));
    m_variant->addItem(QStringLiteral("无标记"), QString());
    m_variant->addItem(QStringLiteral("极品"), QStringLiteral("极品"));
    m_variant->addItem(QStringLiteral("优品"), QStringLiteral("优品"));
    m_menuColor = new FluentComboBox;
    m_menuColor->setObjectName(QStringLiteral("catalogMenuColor"));
    m_menuColor->addItem(QStringLiteral("未记录"), QStringLiteral("unknown"));
    for (const auto& color : {QStringLiteral("red"), QStringLiteral("orange"), QStringLiteral("purple"), QStringLiteral("blue")})
        m_menuColor->addItem(colorLabel(color), color);
    field(QStringLiteral("极品/优品标记"), m_variant, 5, 0);
    field(QStringLiteral("原菜单颜色"), m_menuColor, 5, 1);
    m_productId = input(QStringLiteral("catalogProductId"), QStringLiteral("有原商品 ID 时填写；留空生成本地稳定 ID"));
    m_quality = input(QStringLiteral("catalogGameQuality"), QStringLiteral("未核实可留空，不按颜色自动推断"));
    field(QStringLiteral("商品 ID"), m_productId, 7, 0);
    field(QStringLiteral("游戏正式品质名"), m_quality, 7, 1);
    form->setColumnStretch(0, 1);
    form->setColumnStretch(1, 1);
    addLayout->addLayout(form);
    addLayout->addStretch(1);
    auto* addRow = new QHBoxLayout;
    auto* addNote = label(QStringLiteral("缩略图字段保留为空；保存目录不会执行游戏操作。"), QStringLiteral("cardCaption"));
    addNote->setWordWrap(true);
    addRow->addWidget(addNote, 1);
    auto* addButton = button(QStringLiteral("保存并继续添加"), QStringLiteral("catalogAddButton"), ButtonKind::Accent);
    addRow->addWidget(addButton);
    addLayout->addLayout(addRow);
    m_tabs->addTab(add, QStringLiteral("追加皮肤 / 新赛季"));
    body->addWidget(m_tabs, 1);

    m_message = label(QString(), QStringLiteral("catalogMessage"));
    m_message->setTextFormat(Qt::PlainText);
    m_message->setWordWrap(true);
    m_message->setMinimumHeight(22);
    body->addWidget(m_message);
    outer->addWidget(content, 1);
    auto* commands = new QFrame;
    commands->setObjectName(QStringLiteral("dialogCommands"));
    auto* commandRow = new QHBoxLayout(commands);
    commandRow->setContentsMargins(24, 16, 24, 16);
    commandRow->setSpacing(8);
    auto* import = button(QStringLiteral("导入追加数据"), QStringLiteral("catalogImportButton"));
    auto* exportAdded = button(QStringLiteral("导出追加数据"), QStringLiteral("catalogExportExtensionsButton"));
    auto* exportAll = button(QStringLiteral("导出完整目录"), QStringLiteral("catalogExportAllButton"));
    auto* templateButton = button(QStringLiteral("保存空白模板"), QStringLiteral("catalogTemplateButton"));
    auto* close = button(QStringLiteral("完成"), QStringLiteral("catalogCloseButton"));
    commandRow->addWidget(import);
    commandRow->addWidget(exportAdded);
    commandRow->addWidget(exportAll);
    commandRow->addWidget(templateButton);
    commandRow->addStretch(1);
    commandRow->addWidget(close);
    outer->addWidget(commands);

    connect(m_search, &QLineEdit::textChanged, this, &SkinCatalogDialog::refreshTable);
    connect(m_filter, &QComboBox::currentIndexChanged, this, &SkinCatalogDialog::refreshTable);
    connect(m_season, &QComboBox::currentIndexChanged, this, &SkinCatalogDialog::updateSeasonFields);
    connect(addButton, &QPushButton::clicked, this, &SkinCatalogDialog::addSkin);
    connect(import, &QPushButton::clicked, this, &SkinCatalogDialog::importFile);
    connect(exportAdded, &QPushButton::clicked, this, [this] { exportFile(true); });
    connect(exportAll, &QPushButton::clicked, this, [this] { exportFile(false); });
    connect(templateButton, &QPushButton::clicked, this, &SkinCatalogDialog::exportTemplate);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    refresh();
    if (!m_store) {
        addButton->setEnabled(false);
        import->setEnabled(false);
        exportAdded->setEnabled(false);
        exportAll->setEnabled(false);
        templateButton->setEnabled(false);
        showMessage(QStringLiteral("皮肤目录尚未加载。"), true);
    }
}

void SkinCatalogDialog::refresh() {
    const QString filter = m_filter->currentData().toString();
    const QString selected = m_season->currentData().toString();
    const QSignalBlocker filterBlocker(m_filter), seasonBlocker(m_season);
    m_filter->clear();
    m_filter->addItem(QStringLiteral("全部赛季"), QString());
    m_season->clear();
    m_season->addItem(QStringLiteral("＋ 新建赛季"), QString());
    int maxSeason = 0;
    if (m_store) {
        for (const auto& season : m_store->catalog().seasons) {
            const QString title = season.id + QStringLiteral(" · ") + season.label;
            m_filter->addItem(title, season.id);
            m_season->addItem(title, season.id);
            maxSeason = qMax(maxSeason, season.id.mid(1).toInt());
        }
    }
    const int filterIndex = m_filter->findData(filter);
    const int seasonIndex = m_season->findData(selected);
    m_filter->setCurrentIndex(qMax(0, filterIndex));
    m_season->setCurrentIndex(qMax(0, seasonIndex));
    m_seasonNumber->setValue(qMin(999, maxSeason + 1));
    updateSeasonFields();
    refreshTable();
}

void SkinCatalogDialog::refreshTable() {
    m_table->setRowCount(0);
    if (!m_store) { m_count->setText(QStringLiteral("未加载")); return; }
    const QString search = m_search->text().trimmed();
    const QString season = m_filter->currentData().toString();
    const auto& data = m_store->catalog();
    for (const auto& skin : data.skins) {
        if (!season.isEmpty() && skin.seasonId != season) continue;
        const QString searchable = QStringList{skin.productId, skin.seasonId, skin.seasonLabel, skin.weapon,
            skin.skinSeries, skin.variantLabel, skin.gameQualityName, skin.displayName, colorLabel(skin.menuColor)}.join(' ');
        if (!search.isEmpty() && !searchable.contains(search, Qt::CaseInsensitive)) continue;
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QStringList cells{skin.seasonId, skin.weapon, visibleValue(skin.skinSeries),
            visibleValue(skin.variantLabel), colorLabel(skin.menuColor), visibleValue(skin.gameQualityName), skin.productId};
        for (int col = 0; col < cells.size(); ++col) {
            auto* item = put(m_table, row, col, cells[col], QColor(), skin.productId);
            const QString tooltip = col == 0 ? skin.seasonId + QStringLiteral(" · ") + skin.seasonLabel : cells[col];
            item->setToolTip(QStringLiteral("<qt>%1</qt>").arg(tooltip.toHtmlEscaped()));
        }
    }
    m_count->setText(QStringLiteral("%1 个赛季 · %2 款皮肤 · 当前 %3 款")
        .arg(data.seasons.size()).arg(data.skins.size()).arg(m_table->rowCount()));
}

void SkinCatalogDialog::updateSeasonFields() {
    m_seasonFields->setVisible(m_season->currentData().toString().isEmpty());
}

void SkinCatalogDialog::showMessage(const QString& message, bool error) {
    m_message->setText(message);
    m_message->setProperty("validationError", error);
    // Meaning is expressed in text as well, without a colored surface.
    m_message->setAccessibleName(error ? QStringLiteral("目录校验提示") : QStringLiteral("目录保存结果"));
}

void SkinCatalogDialog::addSkin() {
    if (!m_store) return;
    if (m_weapon->text().trimmed().isEmpty()) {
        showMessage(QStringLiteral("请填写武器名称。"), true); m_weapon->setFocus(); return;
    }
    const bool newSeason = m_season->currentData().toString().isEmpty();
    catalog::Season season;
    if (newSeason) {
        season.id = QStringLiteral("S%1").arg(m_seasonNumber->value());
        season.label = m_seasonName->text().trimmed();
        if (season.label.isEmpty()) {
            showMessage(QStringLiteral("请填写新赛季名称。"), true); m_seasonName->setFocus(); return;
        }
        for (const auto& existing : m_store->catalog().seasons) {
            if (existing.id == season.id) {
                showMessage(QStringLiteral("%1 已存在，请从“所属赛季”选择该赛季后追加。 ").arg(season.id), true);
                return;
            }
        }
    } else {
        season.id = m_season->currentData().toString();
        for (const auto& existing : m_store->catalog().seasons)
            if (existing.id == season.id) { season.label = existing.label; break; }
    }
    catalog::Skin skin;
    skin.productId = m_productId->text().trimmed();
    if (skin.productId.isEmpty()) skin.productId = catalog::nextUserProductId();
    skin.seasonId = season.id;
    skin.seasonLabel = season.label;
    skin.weapon = m_weapon->text().trimmed();
    skin.skinSeries = m_skinSeries->text().trimmed();
    skin.variantLabel = m_variant->currentData().toString();
    skin.menuColor = m_menuColor->currentData().toString();
    skin.gameQualityName = m_quality->text().trimmed();
    skin.displayName = skin.weapon;
    if (!skin.skinSeries.isEmpty()) skin.displayName += QStringLiteral(" - ") + skin.skinSeries;
    if (!skin.variantLabel.isEmpty()) skin.displayName += QStringLiteral(" - ") + skin.variantLabel;
    skin.thumbnailPath.clear();
    QString error;
    const bool saved = newSeason ? m_store->appendSeason(season, {skin}, &error)
                                 : m_store->appendSkins({skin}, &error);
    if (!saved) { showMessage(QStringLiteral("尚未保存：%1").arg(error), true); return; }
    refresh();
    m_season->setCurrentIndex(m_season->findData(season.id));
    m_productId->clear();
    m_weapon->clear();
    m_skinSeries->clear();
    m_quality->clear();
    m_weapon->setFocus();
    showMessage(QStringLiteral("已追加 %1 · %2。可继续添加同赛季皮肤。").arg(season.id, skin.displayName));
    emit catalogChanged();
}

bool SkinCatalogDialog::importCatalogJson(const QByteArray& bytes, QString* outputError) {
    QString error;
    if (!m_store) error = QStringLiteral("皮肤目录尚未加载。");
    const int before = m_store ? int(m_store->catalog().skins.size()) : 0;
    if (!m_store || !m_store->importJson(bytes, &error)) {
        if (outputError) *outputError = error;
        showMessage(QStringLiteral("导入未保存：%1").arg(error), true);
        return false;
    }
    refresh();
    showMessage(QStringLiteral("已导入 %1 款皮肤，原目录保持不变。")
        .arg(m_store->catalog().skins.size() - before));
    if (outputError) outputError->clear();
    emit catalogChanged();
    return true;
}

void SkinCatalogDialog::importFile() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入新赛季 / 追加皮肤数据"),
        QString(), QStringLiteral("皮肤目录 JSON (*.json)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { showMessage(QStringLiteral("读取文件失败：%1").arg(file.errorString()), true); return; }
    constexpr qint64 MaxImportBytes = 4 * 1024 * 1024;
    if (file.size() > MaxImportBytes) { showMessage(QStringLiteral("目录文件超过 4 MB，请拆分后再导入。"), true); return; }
    importCatalogJson(file.readAll());
}

void SkinCatalogDialog::exportFile(bool extensionsOnly) {
    if (!m_store) return;
    const QString name = extensionsOnly ? QStringLiteral("追加皮肤目录.json") : QStringLiteral("完整皮肤目录.json");
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出皮肤目录"), name,
        QStringLiteral("皮肤目录 JSON (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    if (!saveJson(path, extensionsOnly ? m_store->extensionJson() : m_store->exportJson(), &error)) {
        showMessage(QStringLiteral("导出失败：%1").arg(error), true); return;
    }
    showMessage(extensionsOnly ? QStringLiteral("追加数据已导出，可用于导入没有这些条目的目录。")
                              : QStringLiteral("完整目录已导出。已有商品 ID 不会被重复导入或覆盖。"));
}

void SkinCatalogDialog::exportTemplate() {
    if (!m_store) return;
    int maxSeason = 0;
    for (const auto& season : m_store->catalog().seasons) maxSeason = qMax(maxSeason, season.id.mid(1).toInt());
    if (maxSeason >= 999) {
        showMessage(QStringLiteral("赛季编号已达到 S999；可在已有赛季中继续追加皮肤。"), true);
        return;
    }
    const QString seasonId = QStringLiteral("S%1").arg(maxSeason + 1);
    const QJsonObject skin{{"product_id", catalog::nextUserProductId()}, {"season_id", seasonId},
        {"season_label", ""}, {"weapon", ""}, {"skin_series", ""}, {"variant_label", ""},
        {"menu_color", "unknown"}, {"game_quality_name", ""}, {"display_name", ""}, {"thumbnail_path", ""}};
    const QJsonObject root{{"schema", "relink-skin-catalog-v1"},
        {"seasons", QJsonArray{QJsonObject{{"id", seasonId}, {"label", ""}}}}, {"skins", QJsonArray{skin}}};
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存空白新赛季模板"),
        QStringLiteral("新赛季皮肤模板.json"), QStringLiteral("皮肤目录 JSON (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    if (!saveJson(path, QJsonDocument(root).toJson(QJsonDocument::Indented), &error)) {
        showMessage(QStringLiteral("模板保存失败：%1").arg(error), true); return;
    }
    showMessage(QStringLiteral("模板已保存。填写赛季名称、武器、系列等真实字段后导入；每款皮肤的商品 ID 必须唯一，缩略图留空。"));
}
} // namespace relink::ui
