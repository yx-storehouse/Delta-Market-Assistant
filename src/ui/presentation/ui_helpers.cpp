#include "ui_helpers.h"
#include "domain.h"
#include "fluenttheme.h"
#include <QCheckBox>
#include <QDialog>
#include <QEvent>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <cmath>

namespace relink::ui {
// Display-only translations: controller values, combo item data and exported
// records keep their original machine-readable identifiers.
QString workspaceText(const QString& value)
{
    static const QHash<QString, QString> labels{
        {QStringLiteral("SyntheticOnly"), QStringLiteral("内置回放样例")},
        {QStringLiteral("ReviewedDisabled"), QStringLiteral("已审定·未启用")},
        {QStringLiteral("Replay"), QStringLiteral("回放")},
        {QStringLiteral("Demo"), QStringLiteral("本地演示")},
        {QStringLiteral("synthetic_replay"), QStringLiteral("合成回放")},
        {QStringLiteral("synthetic_demo"), QStringLiteral("本地演示")},
        {QStringLiteral("synthetic_fixture"), QStringLiteral("内置合成样例")},
        {QStringLiteral("committed_recovery_audit"), QStringLiteral("恢复审计")},
        {QStringLiteral("Ready"), QStringLiteral("就绪")},
        {QStringLiteral("Observe"), QStringLiteral("观察中")},
        {QStringLiteral("Watchlist"), QStringLiteral("关注列表")},
        {QStringLiteral("Paused"), QStringLiteral("已暂停")},
        {QStringLiteral("Stopped"), QStringLiteral("已停止")},
        {QStringLiteral("Completed"), QStringLiteral("已完成")},
        {QStringLiteral("Recovered"), QStringLiteral("已恢复")},
        {QStringLiteral("Recover"), QStringLiteral("等待恢复")},
        {QStringLiteral("Reconcile"), QStringLiteral("待核验")},
        {QStringLiteral("PersistResult"), QStringLiteral("保存回执")},
        {QStringLiteral("AwaitDeadline"), QStringLiteral("等待计划")},
        {QStringLiteral("Revalidate"), QStringLiteral("重新核验")},
        {QStringLiteral("Start"), QStringLiteral("开始回放")},
        {QStringLiteral("Observation"), QStringLiteral("采集观测")},
        {QStringLiteral("FixtureStep"), QStringLiteral("样例推进")},
        {QStringLiteral("DispatchSimulated"), QStringLiteral("模拟发起")},
        {QStringLiteral("ReceiptConfirmed"), QStringLiteral("回执确认")},
        {QStringLiteral("ReceiptFailed"), QStringLiteral("回执失败")},
        {QStringLiteral("ReceiptSkipped"), QStringLiteral("跳过回执")},
        {QStringLiteral("Pause"), QStringLiteral("暂停回放")},
        {QStringLiteral("Resume"), QStringLiteral("继续回放")},
        {QStringLiteral("Stop"), QStringLiteral("停止回放")},
        {QStringLiteral("Recovery"), QStringLiteral("恢复记录")},
        {QStringLiteral("Match"), QStringLiteral("匹配")},
        {QStringLiteral("NoMatch"), QStringLiteral("不匹配")},
        {QStringLiteral("NeedsReview"), QStringLiteral("待审定")},
        {QStringLiteral("Unknown"), QStringLiteral("结果未知")},
        {QStringLiteral("MATCH"), QStringLiteral("条件匹配")},
        {QStringLiteral("OBSERVATION_STALE"), QStringLiteral("观测已过期")},
        {QStringLiteral("FIELD_MISSING"), QStringLiteral("字段缺失")},
        {QStringLiteral("FIELD_INVALID"), QStringLiteral("字段无效")},
        {QStringLiteral("FIELD_AMBIGUOUS"), QStringLiteral("字段待核验")},
        {QStringLiteral("PRICE_BELOW_MIN"), QStringLiteral("价格低于下限")},
        {QStringLiteral("PRICE_ABOVE_MAX"), QStringLiteral("价格高于上限")},
        {QStringLiteral("WEAR_ABOVE_MAX"), QStringLiteral("磨损高于上限")},
        {QStringLiteral("PRODUCT_MISMATCH"), QStringLiteral("商品不匹配")},
        {QStringLiteral("RULE_DISABLED"), QStringLiteral("规则未启用")},
        {QStringLiteral("RULE_INVALID"), QStringLiteral("规则待修正")},
        {QStringLiteral("SYNTHETIC_ONLY"), QStringLiteral("仅合成样例")},
        {QStringLiteral("SAVED_PROFILE_DISABLED"), QStringLiteral("已保存方案未启用")},
        {QStringLiteral("SYNTHETIC_REJECTED"), QStringLiteral("模拟回执拒绝")},
        {QStringLiteral("SIMULATED_CONFIRMED_RECEIPT"), QStringLiteral("模拟回执已确认")},
        {QStringLiteral("SKIPPED_UNKNOWN_NO_AUTOMATIC_RECONCILIATION"), QStringLiteral("结果未知，未自动核验")},
        {QStringLiteral("price"), QStringLiteral("价格")},
        {QStringLiteral("product_ref"), QStringLiteral("商品关联")}
    };
    return labels.value(value, value.isEmpty() ? QStringLiteral("—") : value);
}

QString shortWorkspaceId(const QString& value)
{
    if (value.isEmpty()) return QStringLiteral("—");
    return value.size() > 18 ? value.left(13) + QStringLiteral("…") + value.right(4) : value;
}

QColor toneColor(int tone)
{
    switch (tone) {
    case ToneAccent: return FluentTheme::accent;
    case ToneSuccess: return FluentTheme::positive;
    case ToneCaution: return FluentTheme::caution;
    case ToneCritical: return FluentTheme::negative;
    case ToneInactive: return FluentTheme::muted;
    default: return QColor("#6E6E6E");
    }
}

int taskTone(const Task& task)
{
    if (!task.enabled) return ToneInactive;
    if (task.status == QStringLiteral("演示已完成")) return ToneSuccess;
    if (task.status == QStringLiteral("演示配置无效")) return ToneCritical;
    if (task.status == QStringLiteral("演示已暂停") || task.status == QStringLiteral("演示等待条件")) return ToneCaution;
    if (task.status == QStringLiteral("演示进行中")) return ToneAccent;
    return ToneNeutral;
}

int levelTone(const QString& level)
{
    if (level == QStringLiteral("SUCCESS")) return ToneSuccess;
    if (level == QStringLiteral("WARN")) return ToneCaution;
    if (level == QStringLiteral("ERROR")) return ToneCritical;
    if (level == QStringLiteral("DEMO")) return ToneAccent;
    return ToneNeutral;
}

QLabel* label(const QString& text, const QString& name)
{
    auto* item = new QLabel(text);
    // Imported names and diagnostics are data, never markup. The one rich-text
    // status badge explicitly opts in below.
    item->setTextFormat(Qt::PlainText);
    if (!name.isEmpty()) item->setObjectName(name);
    return item;
}

QPushButton* button(const QString& text, const QString& name, ButtonKind kind, char16_t glyph)
{
    auto* item = new QPushButton(text);
    item->setObjectName(name);
    // Clicks leave focus where it was; Tab still reaches every button.
    item->setFocusPolicy(Qt::TabFocus);
    item->setMinimumHeight(32);
    if (kind == ButtonKind::Accent) item->setProperty("accent", true);
    if (kind == ButtonKind::Subtle) item->setProperty("subtle", true);
    if (kind == ButtonKind::Link) item->setProperty("link", true);
    if (glyph) {
        const QColor ink = kind == ButtonKind::Accent ? FluentTheme::onAccent : FluentTheme::text;
        item->setIcon(FluentTheme::glyphIcon(glyph, ink, ink));
        item->setIconSize(QSize(16, 16));
    }
    return item;
}

// Store-style "查看全部 >" link: text followed by a small chevron.
QPushButton* link(const QString& text, const QString& name)
{
    auto* item = button(text, name, ButtonKind::Link, Glyph::ChevronRight);
    item->setIconSize(QSize(12, 12));
    item->setLayoutDirection(Qt::RightToLeft);
    return item;
}

QFrame* card(const QString& name)
{
    auto* item = new QFrame;
    item->setObjectName(name);
    item->setProperty("card", true);
    return item;
}

QFrame* rule(const QString& name)
{
    auto* item = new QFrame;
    item->setObjectName(name);
    return item;
}

QVBoxLayout* pageLayout(QWidget* page)
{
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(32, 24, 32, 28);
    layout->setSpacing(0);
    return layout;
}

// Title (28 px) with an optional caption on its baseline; actions go on the right.
QHBoxLayout* pageHeader(QVBoxLayout* layout, const QString& title, QLabel** caption)
{
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(label(title, QStringLiteral("pageTitle")), 0, Qt::AlignBottom);
    if (caption) {
        *caption = label(QString(), QStringLiteral("pageCaption"));
        row->addSpacing(4);
        row->addWidget(*caption, 0, Qt::AlignBottom);
    }
    row->addStretch();
    layout->addLayout(row);
    return row;
}

// Store-style section title; links such as "查看全部" are added on the right.
QHBoxLayout* sectionHeader(QVBoxLayout* layout, const QString& title)
{
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(label(title, QStringLiteral("sectionTitle")));
    row->addStretch();
    layout->addLayout(row);
    layout->addSpacing(10);
    return row;
}

bool numericHeading(const QString& title)
{
    return title.contains(QStringLiteral("报价")) || title.contains(QStringLiteral("价格"))
        || title == QStringLiteral("数量") || title.contains(QStringLiteral("磨损"));
}

QFont numberFont()
{
    QFont font = FluentTheme::font(14);
    font.setFeature(QFont::Tag("tnum"), 1);
    return font;
}

QString amount(double value) { return QLocale(QLocale::English).toString(value, 'f', 2); }
QString changeText(double change) { return QStringLiteral("%1%2%").arg(change >= 0 ? "+" : "").arg(change, 0, 'f', 2); }
QString selectedId(QTableWidget* view)
{
    if (!view || view->currentRow() < 0) return {};
    for (int c = 0; c < view->columnCount(); ++c) {
        if (auto* item = view->item(view->currentRow(), c)) {
            if (!item->data(Qt::UserRole).toString().isEmpty()) return item->data(Qt::UserRole).toString();
        }
    }
    return {};
}
const Skin* findSkin(const AppState* state, const QString& id)
{
    for (const auto& skin : state->skins) if (skin.id == id) return &skin;
    return nullptr;
}
QVector<double> series(double price)
{
    // Deterministic synthetic samples, not observed market history.
    static const double ratios[] = {.83,.89,.94,.91,.86,.87,.92,1.0,1.10,1.03,.94,.89,.94,.98,.93,.88,.90,.94,.90,.97,.92,.94,.96,1.0};
    QVector<double> values;
    const double phase = std::fmod(price, 17.0) * 0.34;
    for (int i = 0; i < 24; ++i) values.append(price * (ratios[i] + (i == 23 ? 0 : 0.025 * std::sin(0.85 * i + phase))));
    return values;
}
QString skinCategory(const QString& name)
{
    const QString base = name.section(' ', 0, 0);
    if (base == "AWM" || base == "M700") return "sniper";
    if (base == "M249") return "lmg";
    if (base == "Vector" || base == "MP5" || base == "P90" || base == "SR-3M") return "smg";
    return "rifle";
}
int skinArtIndex(const Skin& skin)
{
    const QStringList names = {"AUG","M4A1","AKM","AWM","Vector","SCAR-H","MP5","G3","M700","P90","SR-3M","M249"};
    return names.indexOf(skin.name.section(' ', 0, 0));
}
QPixmap skinArtwork(int index)
{
    static QVector<QPixmap> sprites;
    if (sprites.isEmpty()) {
        const QImage atlas(":/assets/demo_skin_atlas.png");
        if (atlas.isNull()) return {};
        for (int i = 0; i < 12; ++i) {
            const int left = (i % 4) * atlas.width() / 4, right = (i % 4 + 1) * atlas.width() / 4;
            const int top = (i / 4) * atlas.height() / 3, bottom = (i / 4 + 1) * atlas.height() / 3;
            const QImage cell = atlas.copy(left, top, right - left, bottom - top);
            int x0 = cell.width(), y0 = cell.height(), x1 = -1, y1 = -1;
            for (int y = 0; y < cell.height(); ++y)
                for (int x = 0; x < cell.width(); ++x)
                    if (qAlpha(cell.pixel(x, y)) > 64) { x0 = qMin(x0, x); y0 = qMin(y0, y); x1 = qMax(x1, x); y1 = qMax(y1, y); }
            // Texture coordinates trim transparent padding at render time;
            // the original generated atlas is preserved byte-for-byte.
            sprites.append(x1 >= x0 ? QPixmap::fromImage(cell.copy(QRect(QPoint(x0, y0), QPoint(x1, y1)))) : QPixmap());
        }
    }
    return index >= 0 && index < sprites.size() ? sprites[index] : QPixmap();
}
// Mirrors the original "S6|AUG 突击步枪 - 天命" skin selector: season, then name.
QString skinChoice(const Skin& skin) { return skin.series.section(' ', 0, 0) + QStringLiteral(" | ") + skin.name; }
QStringList conditionOptions(const AppState* state)
{
    QStringList options = {QStringLiteral("不限"), QStringLiteral("成色S"), QStringLiteral("成色A"), QStringLiteral("成色B")};
    for (const auto& skin : state->skins)
        if (!options.contains(skin.condition)) options.append(skin.condition);
    return options;
}

// Paints table rows the Windows 11 way: either separate rounded cards
// (Store library list) or full-width rows with dividers inside a card.
class RowDelegate final : public QStyledItemDelegate
{
public:
    RowDelegate(QTableWidget* view, RowStyle style) : QStyledItemDelegate(view), m_view(view), m_style(style)
    {
        view->setMouseTracking(true);
        view->viewport()->installEventFilter(this);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        painter->save();
        painter->setClipRect(option.rect);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = index.row() == m_hover;
        // Every cell paints its slice of one plate that spans the whole row.
        const QRectF row(m_view->columnViewportPosition(0), option.rect.top(),
                         m_view->horizontalHeader()->length(), option.rect.height());
        if (m_style == RowStyle::Cards) {
            const QRectF plate = row.adjusted(0.5, 2.5, -0.5, -2.5);
            painter->setPen(QPen(FluentTheme::stroke, 1));
            painter->setBrush(selected ? FluentTheme::selected : (hovered ? FluentTheme::hover : FluentTheme::surface));
            painter->drawRoundedRect(plate, 6, 6);
            if (selected) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(FluentTheme::accent);
                painter->drawRoundedRect(QRectF(plate.left() + 4, plate.center().y() - 8, 3, 16), 1.5, 1.5);
            }
        } else {
            if (index.row() + 1 < index.model()->rowCount()) {
                painter->setPen(QPen(FluentTheme::divider, 1));
                const qreal y = option.rect.bottom() + 0.5;
                painter->drawLine(QPointF(row.left() + 8, y), QPointF(row.right() - 8, y));
            }
            if (selected || hovered) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(QColor(0, 0, 0, selected ? 10 : 6));
                painter->drawRoundedRect(row.adjusted(2, 3, -2, -3), 4, 4);
            }
            if (selected) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(FluentTheme::accent);
                painter->drawRoundedRect(QRectF(row.left() + 2, row.center().y() - 8, 3, 16), 1.5, 1.5);
            }
        }
        switch (index.data(KindRole).toInt()) {
        case ItemCell: paintItem(painter, option, index, selected); break;
        case SparkCell: paintSpark(painter, option, index); break;
        case StatusCell: paintStatus(painter, option, index); break;
        default: paintText(painter, option, index); break;
        }
        painter->restore();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_view->viewport()) {
            if (event->type() == QEvent::MouseMove)
                setHover(m_view->rowAt(static_cast<QMouseEvent*>(event)->position().toPoint().y()));
            else if (event->type() == QEvent::Leave)
                setHover(-1);
            return false;
        }
        return QStyledItemDelegate::eventFilter(watched, event);
    }

private:
    void setHover(int row)
    {
        if (row == m_hover) return;
        m_hover = row;
        m_view->viewport()->update();
    }
    static QString elided(QPainter* painter, const QString& text, qreal width)
    {
        return painter->fontMetrics().elidedText(text, Qt::ElideRight, qMax(0, qRound(width)));
    }
    void paintText(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        const QVariant font = index.data(Qt::FontRole);
        painter->setFont(font.isValid() ? qvariant_cast<QFont>(font) : FluentTheme::font(14));
        const QVariant foreground = index.data(Qt::ForegroundRole);
        painter->setPen(foreground.canConvert<QBrush>() ? qvariant_cast<QBrush>(foreground).color() : FluentTheme::text);
        const QRectF textRect = QRectF(option.rect).adjusted(10, 0, -10, 0);
        const QVariant alignment = index.data(Qt::TextAlignmentRole);
        painter->drawText(textRect, alignment.isValid() ? alignment.toInt() : static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter),
                          elided(painter, index.data().toString(), textRect.width()));
    }
    void paintItem(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index, bool selected) const
    {
        const QRectF cell = QRectF(option.rect).adjusted(8, 0, -8, 0);
        const qreal tileHeight = option.rect.height() - 20.0;
        const QRectF tile(cell.left(), option.rect.top() + 10.0, std::round(tileHeight * 1.75), tileHeight);
        painter->setPen(Qt::NoPen);
        painter->setBrush(selected ? QColor("#E9E9E9") : FluentTheme::inset);
        painter->drawRoundedRect(tile, 4, 4);
        const QPixmap art = skinArtwork(index.data(ArtRole).toInt());
        if (!art.isNull()) {
            const QSizeF size = QSizeF(art.size()).scaled(tile.size() - QSizeF(10, 6), Qt::KeepAspectRatio);
            painter->drawPixmap(QRectF(tile.center().x() - size.width() / 2, tile.center().y() - size.height() / 2,
                                       size.width(), size.height()), art, QRectF(art.rect()));
        }
        const qreal left = tile.right() + 12;
        const qreal width = cell.right() - left;
        const qreal middle = option.rect.center().y();
        painter->setFont(FluentTheme::font(14));
        painter->setPen(FluentTheme::text);
        painter->drawText(QRectF(left, middle - 20, width, 20), Qt::AlignLeft | Qt::AlignVCenter,
                          elided(painter, index.data().toString(), width));
        painter->setFont(FluentTheme::font(12));
        painter->setPen(FluentTheme::secondary);
        painter->drawText(QRectF(left, middle + 1, width, 18), Qt::AlignLeft | Qt::AlignVCenter,
                          elided(painter, index.data(SubtitleRole).toString(), width));
    }
    void paintSpark(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        const QVariantList points = index.data(SamplesRole).toList();
        if (points.size() < 2) return;
        double low = points.first().toDouble(), high = low;
        for (const auto& value : points) { low = qMin(low, value.toDouble()); high = qMax(high, value.toDouble()); }
        const qreal inset = option.rect.height() >= 60 ? 20 : 15;
        const QRectF area = QRectF(option.rect).adjusted(12, inset, -12, -inset);
        QPainterPath path;
        for (int i = 0; i < points.size(); ++i) {
            const QPointF point(area.left() + area.width() * i / (points.size() - 1),
                                area.bottom() - (points[i].toDouble() - low) / qMax(1.0, high - low) * area.height());
            if (i == 0) path.moveTo(point); else path.lineTo(point);
        }
        painter->setPen(QPen(QColor("#707070"), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(path);
        painter->setPen(Qt::NoPen);
        painter->setBrush(FluentTheme::accent);
        painter->drawEllipse(path.currentPosition(), 2.5, 2.5);
    }
    void paintStatus(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        const int tone = index.data(ToneRole).toInt();
        const QRectF area = QRectF(option.rect).adjusted(10, 0, -10, 0);
        const QPointF dot(area.left() + 4, option.rect.center().y() + 0.5);
        if (tone == ToneInactive) {
            painter->setPen(QPen(toneColor(tone), 1.2));
            painter->setBrush(Qt::NoBrush);
            painter->drawEllipse(dot, 3.5, 3.5);
        } else {
            painter->setPen(Qt::NoPen);
            painter->setBrush(toneColor(tone));
            painter->drawEllipse(dot, 4, 4);
        }
        painter->setFont(FluentTheme::font(14));
        painter->setPen(tone == ToneInactive ? FluentTheme::secondary : FluentTheme::text);
        const QRectF textRect = area.adjusted(16, 0, 0, 0);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided(painter, index.data().toString(), textRect.width()));
    }

    QTableWidget* m_view;
    RowStyle m_style;
    int m_hover = -1;
};

QTableWidget* table(const QStringList& columns, const QString& name, RowStyle style)
{
    auto* item = new QTableWidget(0, int(columns.size()));
    item->setObjectName(name);
    item->setHorizontalHeaderLabels(columns);
    item->setSelectionBehavior(QAbstractItemView::SelectRows);
    item->setSelectionMode(QAbstractItemView::SingleSelection);
    item->setEditTriggers(QAbstractItemView::NoEditTriggers);
    item->setShowGrid(false);
    item->setWordWrap(false);
    item->setFrameShape(QFrame::NoFrame);
    item->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    item->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    item->verticalHeader()->setVisible(false);
    item->verticalHeader()->setDefaultSectionSize(style == RowStyle::Cards ? 64 : 44);
    auto* header = item->horizontalHeader();
    header->setHighlightSections(false);
    header->setSectionsClickable(false);
    header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setStretchLastSection(true);
    header->setFixedHeight(34);
    item->setMinimumHeight(130);
    for (int column = 0; column < columns.size(); ++column)
        if (numericHeading(columns[column]))
            item->horizontalHeaderItem(column)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    item->setItemDelegate(new RowDelegate(item, style));
    return item;
}

QTableWidgetItem* put(QTableWidget* table, int row, int column, const QString& text,
                      const QColor& color, const QString& id)
{
    auto* item = new QTableWidgetItem(text);
    if (color.isValid()) item->setForeground(color);
    if (!id.isEmpty()) item->setData(Qt::UserRole, id);
    if (numericHeading(table->horizontalHeaderItem(column)->text())) {
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        item->setFont(numberFont());
    }
    table->setItem(row, column, item);
    return item;
}

void putStatus(QTableWidget* table, int row, int column, const QString& text, int tone)
{
    auto* item = put(table, row, column, text);
    item->setData(KindRole, StatusCell);
    item->setData(ToneRole, tone);
}

QWidget* centered(QWidget* control)
{
    auto* host = new QWidget;
    auto* row = new QHBoxLayout(host);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(control, 0, Qt::AlignCenter);
    return host;
}

QLabel* glyphLabel(char16_t glyph)
{
    auto* item = new QLabel(QString(QChar(glyph)));
    item->setProperty("glyph", true);
    item->setAlignment(Qt::AlignCenter);
    item->setFixedSize(24, 24);
    return item;
}

// Windows 11 Settings card: icon, title and description, control on the right.
// Indented cards have no icon and read as the expanded rows of the card above.
QFrame* settingsCard(QWidget* icon, const QString& title, const QString& description, QWidget* trailing,
                     const QString& name, bool indented)
{
    auto* frame = new QFrame;
    frame->setProperty("settingsCard", true);
    if (!name.isEmpty()) frame->setObjectName(name);
    frame->setMinimumHeight(description.isEmpty() ? 56 : 68);
    auto* row = new QHBoxLayout(frame);
    row->setContentsMargins(indented ? 56 : 16, 10, 16, 10);
    row->setSpacing(16);
    if (icon) row->addWidget(icon, 0, Qt::AlignVCenter);
    auto* text = new QVBoxLayout;
    text->setSpacing(2);
    auto* heading = label(title);
    heading->setWordWrap(true);
    text->addWidget(heading);
    if (!description.isEmpty()) {
        auto* detail = label(description, QStringLiteral("cardCaption"));
        detail->setWordWrap(true);
        text->addWidget(detail);
    }
    row->addLayout(text, 1);
    if (trailing) row->addWidget(trailing, 0, Qt::AlignVCenter);
    return frame;
}

// Sizes a ContentDialog to its content at a fixed width.
void fitDialog(QDialog& dialog, int width)
{
    dialog.setFixedWidth(width);
    QLayout* layout = dialog.layout();
    dialog.setFixedHeight(layout->hasHeightForWidth() ? layout->totalHeightForWidth(width) : layout->totalSizeHint().height());
}

// Windows 11 ContentDialog: title and message above a Mica command bar.
bool confirm(QWidget* parent, const QString& title, const QString& text, const QString& accept)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("confirmDialog"));
    dialog.setWindowTitle(title);
    auto* outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* content = new QFrame;
    content->setObjectName(QStringLiteral("dialogContent"));
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(24, 22, 24, 24);
    body->setSpacing(0);
    body->addWidget(label(title, QStringLiteral("dialogTitle")));
    body->addSpacing(12);
    auto* message = label(text);
    message->setWordWrap(true);
    body->addWidget(message);
    outer->addWidget(content, 1);
    auto* commands = new QFrame;
    commands->setObjectName(QStringLiteral("dialogCommands"));
    commands->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* row = new QHBoxLayout(commands);
    row->setContentsMargins(24, 20, 24, 20);
    row->setSpacing(8);
    auto* yes = button(accept, QStringLiteral("confirmAcceptButton"), ButtonKind::Accent);
    auto* no = button(QStringLiteral("取消"), QStringLiteral("confirmCancelButton"));
    no->setDefault(true);
    row->addWidget(yes, 1);
    row->addWidget(no, 1);
    outer->addWidget(commands);
    QObject::connect(yes, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(no, &QPushButton::clicked, &dialog, &QDialog::reject);
    fitDialog(dialog, 440);
    return dialog.exec() == QDialog::Accepted;
}

QScrollArea* scrollablePage(QWidget* page)
{
    auto* scroll = new QScrollArea;
    scroll->setObjectName(page->objectName() + QStringLiteral("ScrollArea"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setWidget(page);
    return scroll;
}
} // namespace relink::ui
