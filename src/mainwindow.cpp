#include "mainwindow.h"
#include "domain.h"
#include "config/v1_adapter.h"
#include "application/runtime/replay_controller.h"
#include "application/runtime/ui_projection.h"
#include "widgets.h"
#include "fluenttheme.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTime>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
const QStringList titles = {QStringLiteral("工作台"), QStringLiteral("我的关注"), QStringLiteral("自动任务"),
                            QStringLiteral("价格中心"), QStringLiteral("运行统计"), QStringLiteral("运行日志"),
                            QStringLiteral("设置"), QStringLiteral("运行设置")};
constexpr int RunPage = 7;

constexpr int SubtitleRole = Qt::UserRole + 1;
constexpr int ArtRole = Qt::UserRole + 2;
constexpr int SamplesRole = Qt::UserRole + 3;
constexpr int ToneRole = Qt::UserRole + 4;
constexpr int KindRole = Qt::UserRole + 10;
enum CellKind { TextCell, ItemCell, SparkCell, StatusCell };
enum Tone { ToneNeutral, ToneAccent, ToneSuccess, ToneCaution, ToneCritical, ToneInactive };
enum class RowStyle { Cards, Lines };
enum class ButtonKind { Standard, Accent, Subtle, Link };

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

QLabel* label(const QString& text, const QString& name = QString())
{
    auto* item = new QLabel(text);
    if (!name.isEmpty()) item->setObjectName(name);
    return item;
}

QPushButton* button(const QString& text, const QString& name, ButtonKind kind = ButtonKind::Standard, char16_t glyph = 0)
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
QHBoxLayout* pageHeader(QVBoxLayout* layout, const QString& title, QLabel** caption = nullptr)
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
const Skin* findSkin(AppState* state, const QString& id)
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
                      const QColor& color = QColor(), const QString& id = QString())
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

class AppMark final : public QWidget
{
public:
    explicit AppMark(int size) { setFixedSize(size, size); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        FluentTheme::paintAppIcon(&painter, QRectF(rect()));
    }
};

// Windows 11 Settings card: icon, title and description, control on the right.
// Indented cards have no icon and read as the expanded rows of the card above.
QFrame* settingsCard(QWidget* icon, const QString& title, const QString& description, QWidget* trailing,
                     const QString& name = QString(), bool indented = false)
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
} // namespace

MainWindow::MainWindow(AppState* state, QWidget* parent) : QMainWindow(parent), m_state(state)
{
    m_replayController = new relink::runtime::ReplayController(this);
    connect(m_replayController, &relink::runtime::ReplayController::changed, this, &MainWindow::refreshReplayProjection);
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(QStringLiteral("Relink Studio"));
    setWindowIcon(FluentTheme::appIcon());
    resize(1560, 980);
    setMinimumSize(1180, 820);
    auto* central = new QWidget;
    central->setObjectName(QStringLiteral("appShell"));
    setCentralWidget(central);
    auto* shell = new QVBoxLayout(central);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);
    shell->addWidget(buildTitleBar());
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(buildNavigationRail());
    // NavigationView content layer: lighter than Mica, rounded top-left corner.
    auto* layer = new QFrame;
    layer->setObjectName(QStringLiteral("contentLayer"));
    auto* layerLayout = new QVBoxLayout(layer);
    layerLayout->setContentsMargins(1, 1, 0, 0);
    layerLayout->setSpacing(0);
    m_pages = new QStackedWidget;
    m_pages->setObjectName(QStringLiteral("pageStack"));
    m_pages->addWidget(scrollablePage(buildOverview()));
    m_pages->addWidget(buildFavorites());
    m_pages->addWidget(scrollablePage(buildTasks()));
    m_pages->addWidget(scrollablePage(buildPrices()));
    m_pages->addWidget(scrollablePage(buildStats()));
    m_pages->addWidget(scrollablePage(buildLogs()));
    m_pages->addWidget(scrollablePage(buildSettings()));
    m_pages->addWidget(scrollablePage(buildRunSettings()));
    layerLayout->addWidget(m_pages);
    body->addWidget(layer, 1);
    shell->addLayout(body, 1);
    connect(m_state, &AppState::changed, this, &MainWindow::refreshAll);
    QSettings preferences;
    m_autoScrollLogs->setChecked(preferences.value(QStringLiteral("ui/autoScrollLogs"), true).toBool());
    m_compactTables->setChecked(preferences.value(QStringLiteral("ui/compactTables"), false).toBool());
    applyDensity();
    setPage(1);
    refreshAll();
}

QWidget* MainWindow::buildTitleBar()
{
    auto* bar = new QFrame;
    bar->setObjectName(QStringLiteral("titleBar"));
    bar->setFixedHeight(52);
    auto* actions = new QHBoxLayout(bar);
    actions->setContentsMargins(16, 8, 16, 10);
    actions->setSpacing(4);
    actions->addStretch();
    // Store-style search, centred on the window by resizeEvent; it filters the watchlist.
    m_favoriteSearch = new QLineEdit(bar);
    m_favoriteSearch->setObjectName(QStringLiteral("favoriteSearch"));
    m_favoriteSearch->setPlaceholderText(QStringLiteral("搜索皮肤、系列"));
    m_favoriteSearch->setFixedSize(520, 34);
    auto* searchAction = m_favoriteSearch->addAction(
        FluentTheme::glyphIcon(Glyph::Search, FluentTheme::secondary, FluentTheme::text), QLineEdit::TrailingPosition);
    searchAction->setToolTip(QStringLiteral("搜索关注条目"));
    m_searchClear = m_favoriteSearch->addAction(
        FluentTheme::glyphIcon(Glyph::Clear, FluentTheme::secondary, FluentTheme::text), QLineEdit::TrailingPosition);
    m_searchClear->setToolTip(QStringLiteral("清除"));
    m_searchClear->setVisible(false);
    connect(searchAction, &QAction::triggered, this, [this] { setPage(1); m_favoriteSearch->setFocus(); });
    connect(m_searchClear, &QAction::triggered, m_favoriteSearch, &QLineEdit::clear);
    auto* importButton = button(QStringLiteral("导入"), QStringLiteral("importConfigButton"), ButtonKind::Subtle, Glyph::Import);
    auto* exportButton = button(QStringLiteral("导出"), QStringLiteral("exportConfigButton"), ButtonKind::Subtle, Glyph::Export);
    auto* helpButton = button(QString(), QStringLiteral("helpButton"), ButtonKind::Subtle, Glyph::Help);
    helpButton->setToolTip(QStringLiteral("帮助"));
    helpButton->setAccessibleName(QStringLiteral("帮助"));
    helpButton->setFixedWidth(36);
    actions->addWidget(importButton);
    actions->addWidget(exportButton);
    actions->addWidget(helpButton);
    actions->addSpacing(8);
    m_statusBadge = label(QString(), QStringLiteral("demoBadge"));
    m_statusBadge->setTextFormat(Qt::RichText);
    actions->addWidget(m_statusBadge);
    connect(importButton, &QPushButton::clicked, this, &MainWindow::importConfiguration);
    connect(exportButton, &QPushButton::clicked, this, &MainWindow::exportConfiguration);
    connect(helpButton, &QPushButton::clicked, this, &MainWindow::showHelp);
    return bar;
}

QWidget* MainWindow::buildNavigationRail()
{
    auto* rail = new QFrame;
    rail->setObjectName(QStringLiteral("navRail"));
    rail->setFixedWidth(76);
    auto* column = new QVBoxLayout(rail);
    column->setContentsMargins(6, 2, 6, 12);
    column->setSpacing(4);
    struct Entry { int page; const char* name; const char* text; char16_t glyph; char16_t selectedGlyph; };
    const Entry entries[] = {
        {1, "navFavorites", "关注", Glyph::Star, Glyph::StarFill},
        {2, "navTasks", "任务", Glyph::Tasks, 0},
        {RunPage, "navRun", "运行", Glyph::Lightning, 0},
        {3, "navPrices", "价格", Glyph::Trend, 0},
        {0, "navOverview", "工作台", Glyph::Home, Glyph::HomeFill},
        {4, "navStats", "统计", Glyph::Pie, 0},
        {5, "navLogs", "日志", Glyph::History, 0},
        {6, "navSettings", "设置", Glyph::Settings, Glyph::SettingsFill},
    };
    m_nav.resize(titles.size());
    for (const auto& entry : entries) {
        if (entry.page == 6) column->addStretch();
        auto* nav = new NavRailButton(QString::fromUtf8(entry.text), entry.glyph, entry.selectedGlyph);
        nav->setObjectName(QString::fromLatin1(entry.name));
        column->addWidget(nav, 0, Qt::AlignHCenter);
        m_nav[entry.page] = nav;
        connect(nav, &QPushButton::clicked, this, [this, page = entry.page] { setPage(page); });
    }
    return rail;
}

QWidget* MainWindow::buildOverview()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("overviewPage"));
    auto* layout = pageLayout(page);
    auto* header = pageHeader(layout, titles[0]);
    m_pauseButton = button(QStringLiteral("暂停"), QStringLiteral("pauseSimulationButton"), ButtonKind::Standard, Glyph::Pause);
    m_startButton = button(QStringLiteral("开始模拟"), QStringLiteral("startSimulationButton"), ButtonKind::Accent, Glyph::Play);
    header->addWidget(m_pauseButton, 0, Qt::AlignVCenter);
    header->addWidget(m_startButton, 0, Qt::AlignVCenter);
    connect(m_startButton, &QPushButton::clicked, m_state, &AppState::startSimulation);
    connect(m_pauseButton, &QPushButton::clicked, m_state, &AppState::pauseSimulation);
    layout->addSpacing(20);

    auto* strip = card(QStringLiteral("metricStrip"));
    auto* metrics = new QHBoxLayout(strip);
    metrics->setContentsMargins(0, 8, 0, 8);
    metrics->setSpacing(0);
    const QStringList names = {QStringLiteral("我的关注"), QStringLiteral("启用任务"), QStringLiteral("扫描条目"), QStringLiteral("模拟确认")};
    for (int i = 0; i < names.size(); ++i) {
        auto* metric = new MetricCard(names[i], QStringLiteral("0"), QString());
        metric->setObjectName(QStringLiteral("overviewMetric%1").arg(i));
        metric->setProperty("separator", i < names.size() - 1);
        m_overviewCards.append(metric);
        metrics->addWidget(metric, 1);
    }
    layout->addWidget(strip);

    // M1 replay branch: this card is intentionally separate from the existing
    // local demo controls above. It drives only ReplayController/FakeClock and
    // never starts capture, OCR, mouse, keyboard, or an external process.
    auto* replayPanel = card(QStringLiteral("replayPanel"));
    auto* replayLayout = new QGridLayout(replayPanel);
    replayLayout->setContentsMargins(16, 12, 16, 12);
    replayLayout->setHorizontalSpacing(16);
    replayLayout->setVerticalSpacing(6);
    auto* replayTitle = label(QStringLiteral("M1 回放验证"), QStringLiteral("replayTitle"));
    replayTitle->setProperty("section", true);
    replayLayout->addWidget(replayTitle, 0, 0, 1, 2);
    m_replaySourceLabel = label(QString(), QStringLiteral("replaySourceLabel"));
    m_replayStateLabel = label(QString(), QStringLiteral("replayStateLabel"));
    m_replayReasonLabel = label(QString(), QStringLiteral("replayReasonLabel"));
    m_replayProgressLabel = label(QString(), QStringLiteral("replayProgressLabel"));
    m_replayReasonLabel->setWordWrap(true);
    m_replayProgressLabel->setProperty("tertiary", true);
    replayLayout->addWidget(label(QStringLiteral("来源"), QStringLiteral("replaySourceCaption")), 1, 0);
    replayLayout->addWidget(m_replaySourceLabel, 1, 1);
    replayLayout->addWidget(label(QStringLiteral("运行状态"), QStringLiteral("replayStateCaption")), 2, 0);
    replayLayout->addWidget(m_replayStateLabel, 2, 1);
    replayLayout->addWidget(label(QStringLiteral("匹配说明"), QStringLiteral("replayReasonCaption")), 3, 0, Qt::AlignTop);
    replayLayout->addWidget(m_replayReasonLabel, 3, 1);
    replayLayout->addWidget(m_replayProgressLabel, 4, 0, 1, 2);
    auto* replayCommands = new QHBoxLayout;
    replayCommands->setContentsMargins(0, 4, 0, 0);
    replayCommands->setSpacing(6);
    m_replayStartButton = button(QStringLiteral("开始回放"), QStringLiteral("replayStartButton"), ButtonKind::Accent, Glyph::Play);
    m_replayPauseButton = button(QStringLiteral("暂停回放"), QStringLiteral("replayPauseButton"), ButtonKind::Standard, Glyph::Pause);
    m_replayResumeButton = button(QStringLiteral("继续回放"), QStringLiteral("replayResumeButton"), ButtonKind::Standard, Glyph::Play);
    m_replayStopButton = button(QStringLiteral("停止回放"), QStringLiteral("replayStopButton"), ButtonKind::Subtle, Glyph::Clear);
    replayCommands->addWidget(m_replayStartButton);
    replayCommands->addWidget(m_replayPauseButton);
    replayCommands->addWidget(m_replayResumeButton);
    replayCommands->addWidget(m_replayStopButton);
    replayCommands->addStretch();
    replayLayout->addLayout(replayCommands, 5, 0, 1, 2);
    layout->addWidget(replayPanel);
    layout->addSpacing(28);
    connect(m_replayStartButton, &QPushButton::clicked, m_replayController, &relink::runtime::ReplayController::startReplay);
    connect(m_replayPauseButton, &QPushButton::clicked, m_replayController, &relink::runtime::ReplayController::pauseReplay);
    connect(m_replayResumeButton, &QPushButton::clicked, m_replayController, &relink::runtime::ReplayController::resumeReplay);
    connect(m_replayStopButton, &QPushButton::clicked, m_replayController, &relink::runtime::ReplayController::stopReplay);

    auto* middle = new QHBoxLayout;
    middle->setSpacing(16);
    auto* chartColumn = new QVBoxLayout;
    chartColumn->setSpacing(0);
    auto* pricesLink = link(QStringLiteral("查看全部价格"), QStringLiteral("overviewPricesLink"));
    sectionHeader(chartColumn, QStringLiteral("价格观察"))->addWidget(pricesLink);
    auto* chartPanel = card(QStringLiteral("overviewChartPanel"));
    auto* chartLayout = new QVBoxLayout(chartPanel);
    chartLayout->setContentsMargins(20, 16, 20, 12);
    m_overviewChart = new PriceChart;
    m_overviewChart->setObjectName(QStringLiteral("overviewPriceChart"));
    m_overviewChart->setMinimumHeight(240);
    chartLayout->addWidget(m_overviewChart, 1);
    chartColumn->addWidget(chartPanel, 1);
    middle->addLayout(chartColumn, 3);

    auto* taskColumn = new QVBoxLayout;
    taskColumn->setSpacing(0);
    auto* taskHeader = sectionHeader(taskColumn, QStringLiteral("任务队列"));
    m_overviewTaskCount = label(QString(), QStringLiteral("tertiaryLabel"));
    taskHeader->insertWidget(1, m_overviewTaskCount, 0, Qt::AlignVCenter);
    auto* tasksLink = link(QStringLiteral("管理任务"), QStringLiteral("overviewTasksLink"));
    taskHeader->addWidget(tasksLink);
    auto* taskPanel = card(QStringLiteral("overviewTaskPanel"));
    auto* taskLayout = new QVBoxLayout(taskPanel);
    taskLayout->setContentsMargins(12, 6, 12, 8);
    m_overviewTasks = table({QStringLiteral("任务"), QStringLiteral("数量"), QStringLiteral("状态")},
                            QStringLiteral("overviewTasksTable"), RowStyle::Lines);
    auto* taskColumns = m_overviewTasks->horizontalHeader();
    taskColumns->setStretchLastSection(false);
    taskColumns->setSectionResizeMode(0, QHeaderView::Stretch);
    taskColumns->setSectionResizeMode(1, QHeaderView::Fixed);
    taskColumns->setSectionResizeMode(2, QHeaderView::Fixed);
    m_overviewTasks->setColumnWidth(1, 60);
    m_overviewTasks->setColumnWidth(2, 124);
    m_overviewTasks->setMinimumHeight(212);
    taskLayout->addWidget(m_overviewTasks, 1);
    taskColumn->addWidget(taskPanel, 1);
    middle->addLayout(taskColumn, 2);
    layout->addLayout(middle, 1);
    layout->addSpacing(28);

    auto* logsLink = link(QStringLiteral("全部日志"), QStringLiteral("overviewLogsLink"));
    sectionHeader(layout, QStringLiteral("最近活动"))->addWidget(logsLink);
    auto* logPanel = card(QStringLiteral("overviewLogPanel"));
    auto* logLayout = new QVBoxLayout(logPanel);
    logLayout->setContentsMargins(12, 4, 12, 4);
    m_overviewLogs = table({QStringLiteral("时间"), QStringLiteral("级别"), QStringLiteral("事件")},
                           QStringLiteral("overviewLogsTable"), RowStyle::Lines);
    m_overviewLogs->horizontalHeader()->hide();
    auto* logColumns = m_overviewLogs->horizontalHeader();
    logColumns->setStretchLastSection(false);
    logColumns->setSectionResizeMode(0, QHeaderView::Fixed);
    logColumns->setSectionResizeMode(1, QHeaderView::Fixed);
    logColumns->setSectionResizeMode(2, QHeaderView::Stretch);
    m_overviewLogs->setColumnWidth(0, 92);
    m_overviewLogs->setColumnWidth(1, 116);
    m_overviewLogs->setMinimumHeight(44);
    logLayout->addWidget(m_overviewLogs);
    layout->addWidget(logPanel);
    connect(pricesLink, &QPushButton::clicked, this, [this] { setPage(3); });
    connect(tasksLink, &QPushButton::clicked, this, [this] { setPage(2); });
    connect(logsLink, &QPushButton::clicked, this, [this] { setPage(5); });
    return page;
}

QWidget* MainWindow::buildFavorites()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("favoritesPage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(32, 24, 32, 24);
    layout->setSpacing(0);
    pageHeader(layout, titles[1], &m_favoritesCount);
    layout->addSpacing(16);

    auto* filterBar = new QFrame;
    filterBar->setObjectName(QStringLiteral("catalogFilterBar"));
    auto* filters = new QHBoxLayout(filterBar);
    filters->setContentsMargins(0, 0, 0, 0);
    filters->setSpacing(6);
    const auto addFilter = [&](const QString& text, const QString& key) {
        auto* pill = new PillButton(text);
        pill->setObjectName(QStringLiteral("catalogFilter_") + key);
        pill->setProperty("filterKey", key);
        pill->setChecked(key == QStringLiteral("all"));
        filters->addWidget(pill);
        m_catalogFilters.append(pill);
        connect(pill, &QPushButton::clicked, this, [this, key] { m_catalogFilter = key; refreshFavorites(); });
    };
    addFilter(QStringLiteral("全部"), QStringLiteral("all"));
    addFilter(QStringLiteral("已关注"), QStringLiteral("followed"));
    addFilter(QStringLiteral("未关注"), QStringLiteral("unfollowed"));
    filters->addWidget(rule(QStringLiteral("filterDivider")));
    addFilter(QStringLiteral("步枪"), QStringLiteral("rifle"));
    addFilter(QStringLiteral("冲锋枪"), QStringLiteral("smg"));
    addFilter(QStringLiteral("狙击枪"), QStringLiteral("sniper"));
    addFilter(QStringLiteral("轻机枪"), QStringLiteral("lmg"));
    filters->addStretch();
    m_rarityFilter = new FluentComboBox;
    m_rarityFilter->setObjectName(QStringLiteral("favoriteRarity"));
    m_rarityFilter->addItem(QStringLiteral("全部品级"));
    m_rarityFilter->setFixedWidth(132);
    m_favoriteStateFilter = new FluentComboBox;
    m_favoriteStateFilter->setObjectName(QStringLiteral("favoriteTaskFilter"));
    m_favoriteStateFilter->addItems({QStringLiteral("全部任务"), QStringLiteral("已关联"), QStringLiteral("未关联")});
    m_favoriteStateFilter->setFixedWidth(132);
    filters->addWidget(m_rarityFilter);
    filters->addWidget(m_favoriteStateFilter);
    layout->addWidget(filterBar);
    layout->addSpacing(16);

    auto* body = new QHBoxLayout;
    body->setSpacing(16);
    auto* workspace = new QWidget;
    workspace->setObjectName(QStringLiteral("watchlistWorkspace"));
    auto* work = new QVBoxLayout(workspace);
    work->setContentsMargins(0, 0, 0, 0);
    work->setSpacing(12);
    m_favoritesTable = table({QStringLiteral("关注"), QStringLiteral("物品"), QStringLiteral("磨损"), QStringLiteral("演示报价"),
                              QStringLiteral("样本趋势"), QStringLiteral("目标价格"), QStringLiteral("任务状态")},
                             QStringLiteral("favoriteTable"), RowStyle::Cards);
    auto* columns = m_favoritesTable->horizontalHeader();
    columns->setStretchLastSection(false);
    columns->setSectionResizeMode(QHeaderView::Fixed);
    columns->setSectionResizeMode(1, QHeaderView::Stretch);
    m_favoritesTable->setColumnWidth(0, 44);
    m_favoritesTable->setMinimumHeight(220);
    work->addWidget(m_favoritesTable, 1);

    auto* chartPanel = card(QStringLiteral("watchlistChartPanel"));
    chartPanel->setFixedHeight(236);
    auto* chartLayout = new QVBoxLayout(chartPanel);
    chartLayout->setContentsMargins(20, 14, 20, 10);
    chartLayout->setSpacing(4);
    auto* chartHeader = new QHBoxLayout;
    chartHeader->setSpacing(8);
    chartHeader->addWidget(label(QStringLiteral("价格走势"), QStringLiteral("cardTitle")));
    m_favoriteChartTitle = label(QString(), QStringLiteral("cardCaption"));
    chartHeader->addWidget(m_favoriteChartTitle);
    chartHeader->addStretch();
    chartHeader->addWidget(label(QStringLiteral("模拟数据"), QStringLiteral("tertiaryLabel")));
    chartLayout->addLayout(chartHeader);
    m_favoriteChart = new PriceChart;
    m_favoriteChart->setObjectName(QStringLiteral("favoritePriceChart"));
    m_favoriteChart->setHeaderVisible(false);
    chartLayout->addWidget(m_favoriteChart, 1);
    work->addWidget(chartPanel);
    body->addWidget(workspace, 1);

    m_inspector = card(QStringLiteral("terminalInspector"));
    m_inspector->setFixedWidth(320);
    auto* inspectorOuter = new QVBoxLayout(m_inspector);
    inspectorOuter->setContentsMargins(1, 1, 1, 1);
    auto* inspectorScroll = new QScrollArea;
    inspectorScroll->setObjectName(QStringLiteral("inspectorScrollArea"));
    inspectorScroll->setWidgetResizable(true);
    inspectorScroll->setFrameShape(QFrame::NoFrame);
    inspectorScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    inspectorScroll->viewport()->setAutoFillBackground(false);
    auto* inspectorContent = new QWidget;
    inspectorContent->setObjectName(QStringLiteral("inspectorContent"));
    auto* detail = new QVBoxLayout(inspectorContent);
    detail->setContentsMargins(20, 20, 20, 20);
    detail->setSpacing(0);
    inspectorScroll->setWidget(inspectorContent);
    inspectorOuter->addWidget(inspectorScroll);

    m_favoriteArt = new ArtworkView;
    m_favoriteArt->setObjectName(QStringLiteral("skinPreview"));
    m_favoriteArt->setFixedHeight(148);
    m_favoriteArt->setToolTip(QStringLiteral("生成的演示插图，不代表游戏中的真实皮肤外观。"));
    detail->addWidget(m_favoriteArt);
    detail->addSpacing(16);
    m_favoriteTitle = label(QString(), QStringLiteral("detailTitle"));
    m_favoriteTitle->setWordWrap(true);
    detail->addWidget(m_favoriteTitle);
    detail->addSpacing(2);
    m_favoriteMetadata = label(QString(), QStringLiteral("cardCaption"));
    detail->addWidget(m_favoriteMetadata);
    detail->addSpacing(12);
    auto* quote = new QHBoxLayout;
    quote->setSpacing(10);
    m_favoritePrice = label(QString(), QStringLiteral("inspectorPrice"));
    m_favoriteChange = label(QString(), QStringLiteral("favoriteChangeLabel"));
    quote->addWidget(m_favoritePrice, 0, Qt::AlignBottom);
    quote->addWidget(m_favoriteChange, 0, Qt::AlignBottom);
    quote->addStretch();
    detail->addLayout(quote);
    detail->addWidget(label(QStringLiteral("演示报价 · 非实时行情"), QStringLiteral("tertiaryLabel")));
    detail->addSpacing(16);
    auto* facts = new QHBoxLayout;
    facts->setSpacing(12);
    const auto addFact = [&](const QString& caption, QLabel*& value) {
        if (facts->count() > 0) facts->addWidget(rule(QStringLiteral("factDivider")));
        auto* block = new QVBoxLayout;
        block->setSpacing(2);
        block->addWidget(label(caption, QStringLiteral("factCaption")));
        value = label(QStringLiteral("—"), QStringLiteral("factValue"));
        block->addWidget(value);
        facts->addLayout(block, 1);
    };
    addFact(QStringLiteral("品级"), m_factRarity);
    addFact(QStringLiteral("成色"), m_factCondition);
    addFact(QStringLiteral("磨损"), m_factWear);
    detail->addLayout(facts);
    detail->addSpacing(16);
    detail->addWidget(rule(QStringLiteral("inspectorRule")));
    detail->addSpacing(16);
    detail->addWidget(label(QStringLiteral("任务条件"), QStringLiteral("cardTitle")));
    detail->addSpacing(10);
    m_inspectorPrice = new FluentDoubleSpinBox;
    m_inspectorPrice->setObjectName(QStringLiteral("inspectorMaxPrice"));
    m_inspectorPrice->setRange(0, 999999999);
    m_inspectorPrice->setDecimals(2);
    m_inspectorCondition = new FluentComboBox;
    m_inspectorCondition->setObjectName(QStringLiteral("inspectorCondition"));
    m_inspectorCondition->addItems(conditionOptions(m_state));
    m_inspectorWear = new FluentDoubleSpinBox;
    m_inspectorWear->setObjectName(QStringLiteral("inspectorMaxWear"));
    m_inspectorWear->setRange(0, 100);
    m_inspectorWear->setDecimals(6);
    m_inspectorQuantity = new FluentSpinBox;
    m_inspectorQuantity->setObjectName(QStringLiteral("inspectorQuantity"));
    m_inspectorQuantity->setRange(1, 9999);
    for (QWidget* field : {static_cast<QWidget*>(m_inspectorPrice), static_cast<QWidget*>(m_inspectorCondition),
                           static_cast<QWidget*>(m_inspectorWear), static_cast<QWidget*>(m_inspectorQuantity)})
        field->setMinimumWidth(64);
    auto* fields = new QGridLayout;
    fields->setHorizontalSpacing(12);
    fields->setVerticalSpacing(4);
    fields->addWidget(label(QStringLiteral("最高价格"), QStringLiteral("fieldLabel")), 0, 0);
    fields->addWidget(label(QStringLiteral("成色"), QStringLiteral("fieldLabel")), 0, 1);
    fields->addWidget(m_inspectorPrice, 1, 0);
    fields->addWidget(m_inspectorCondition, 1, 1);
    fields->setRowMinimumHeight(2, 8);
    fields->addWidget(label(QStringLiteral("最大磨损"), QStringLiteral("fieldLabel")), 3, 0);
    fields->addWidget(label(QStringLiteral("数量上限"), QStringLiteral("fieldLabel")), 3, 1);
    fields->addWidget(m_inspectorWear, 4, 0);
    fields->addWidget(m_inspectorQuantity, 4, 1);
    fields->setColumnStretch(0, 3);
    fields->setColumnStretch(1, 2);
    detail->addLayout(fields);
    detail->addSpacing(16);
    m_favoriteCreate = button(QStringLiteral("创建任务"), QStringLiteral("favoriteCreateTaskButton"), ButtonKind::Accent, Glyph::Add);
    m_favoriteCreate->setFixedHeight(36);
    detail->addWidget(m_favoriteCreate);
    detail->addSpacing(8);
    auto* taskRow = new QHBoxLayout;
    m_favoriteTaskInfo = label(QString(), QStringLiteral("inspectorTaskInfo"));
    taskRow->addWidget(m_favoriteTaskInfo);
    taskRow->addStretch();
    auto* manage = link(QStringLiteral("查看任务"), QStringLiteral("favoritesTasksButton"));
    taskRow->addWidget(manage);
    detail->addLayout(taskRow);
    detail->addStretch();
    detail->addSpacing(12);
    auto* note = label(QStringLiteral("本地模拟模式：插图与价格均为演示素材，创建任务只保存筛选条件。"), QStringLiteral("inspectorNote"));
    note->setWordWrap(true);
    detail->addWidget(note);
    body->addWidget(m_inspector);
    layout->addLayout(body, 1);

    connect(m_favoriteSearch, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_searchClear->setVisible(!text.isEmpty());
        if (m_refreshing) return;
        if (!text.isEmpty() && m_pages->currentIndex() != 1) setPage(1);
        refreshFavorites();
    });
    connect(m_rarityFilter, &QComboBox::currentTextChanged, this, [this] { if (!m_refreshing) refreshFavorites(); });
    connect(m_favoriteStateFilter, &QComboBox::currentIndexChanged, this, [this] { if (!m_refreshing) refreshFavorites(); });
    connect(m_favoritesTable, &QTableWidget::itemSelectionChanged, this, [this] { if (!m_refreshing) refreshFavoriteDetail(); });
    connect(m_favoriteCreate, &QPushButton::clicked, this, &MainWindow::createInspectorTask);
    connect(manage, &QPushButton::clicked, this, [this] { setPage(2); });
    return page;
}

QWidget* MainWindow::buildTasks()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("tasksPage"));
    auto* layout = pageLayout(page);
    pageHeader(layout, titles[2], &m_taskCount);
    layout->addSpacing(16);
    auto* commands = new QHBoxLayout;
    commands->setSpacing(4);
    auto* add = button(QStringLiteral("新增任务"), QStringLiteral("taskNewButton"), ButtonKind::Accent, Glyph::Add);
    auto* edit = button(QStringLiteral("编辑"), QStringLiteral("taskEditButton"), ButtonKind::Subtle, Glyph::Edit);
    auto* remove = button(QStringLiteral("删除"), QStringLiteral("taskDeleteButton"), ButtonKind::Subtle, Glyph::Delete);
    auto* enable = button(QStringLiteral("启用选中"), QStringLiteral("enableSelectedTasksButton"), ButtonKind::Subtle, Glyph::CheckMark);
    auto* disable = button(QStringLiteral("暂停选中"), QStringLiteral("pauseSelectedTasksButton"), ButtonKind::Subtle, Glyph::Pause);
    m_taskStart = button(QStringLiteral("运行模拟"), QStringLiteral("tasksSimulationButton"), ButtonKind::Standard, Glyph::Play);
    commands->addWidget(add);
    commands->addSpacing(8);
    commands->addWidget(edit);
    commands->addWidget(remove);
    commands->addWidget(rule(QStringLiteral("filterDivider")));
    commands->addWidget(enable);
    commands->addWidget(disable);
    commands->addStretch();
    commands->addWidget(m_taskStart);
    layout->addLayout(commands);
    layout->addSpacing(12);
    m_tasksTable = table({QStringLiteral("任务名称"), QStringLiteral("目标皮肤"), QStringLiteral("成色"), QStringLiteral("价格区间"),
                          QStringLiteral("最大磨损"), QStringLiteral("数量"), QStringLiteral("启用"), QStringLiteral("状态")},
                         QStringLiteral("taskTable"), RowStyle::Cards);
    auto* columns = m_tasksTable->horizontalHeader();
    columns->setStretchLastSection(false);
    columns->setSectionResizeMode(QHeaderView::Fixed);
    columns->setSectionResizeMode(0, QHeaderView::Stretch);
    columns->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tasksTable->setColumnWidth(2, 76);
    m_tasksTable->setColumnWidth(3, 156);
    m_tasksTable->setColumnWidth(4, 92);
    m_tasksTable->setColumnWidth(5, 64);
    m_tasksTable->setColumnWidth(6, 72);
    m_tasksTable->setColumnWidth(7, 124);
    m_tasksTable->horizontalHeaderItem(6)->setTextAlignment(Qt::AlignCenter);
    m_tasksTable->verticalHeader()->setDefaultSectionSize(56);
    m_tasksTable->setMinimumHeight(330);
    layout->addWidget(m_tasksTable, 1);
    layout->addSpacing(8);
    auto* note = label(QStringLiteral("Ctrl / Shift 多选 · 双击编辑 · 当前仅模拟运行，不执行购买"), QStringLiteral("tertiaryLabel"));
    note->setWordWrap(true);
    layout->addWidget(note);
    connect(add, &QPushButton::clicked, this, [this] { editTask(); });
    connect(edit, &QPushButton::clicked, this, [this] { if (!selectedTaskId().isEmpty()) editTask(selectedTaskId()); });
    connect(m_tasksTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem*) {
        if (!selectedTaskId().isEmpty()) editTask(selectedTaskId());
    });
    connect(remove, &QPushButton::clicked, this, &MainWindow::deleteSelectedTasks);
    connect(enable, &QPushButton::clicked, this, [this] { setSelectedTasksEnabled(true); });
    connect(disable, &QPushButton::clicked, this, [this] { setSelectedTasksEnabled(false); });
    connect(m_taskStart, &QPushButton::clicked, m_state, &AppState::startSimulation);
    return page;
}

QWidget* MainWindow::buildPrices()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("pricesPage"));
    auto* layout = pageLayout(page);
    auto* header = pageHeader(layout, titles[3]);
    header->addWidget(label(QStringLiteral("关注目标"), QStringLiteral("cardCaption")), 0, Qt::AlignVCenter);
    m_priceSkin = new FluentComboBox;
    m_priceSkin->setObjectName(QStringLiteral("pricesSkinCombo"));
    m_priceSkin->setMinimumWidth(260);
    header->addWidget(m_priceSkin, 0, Qt::AlignVCenter);
    auto* exportButton = button(QStringLiteral("导出 CSV"), QStringLiteral("exportPricesButton"), ButtonKind::Standard, Glyph::Download);
    header->addWidget(exportButton, 0, Qt::AlignVCenter);
    layout->addSpacing(20);

    auto* strip = card(QStringLiteral("metricStrip"));
    auto* metrics = new QHBoxLayout(strip);
    metrics->setContentsMargins(0, 8, 0, 8);
    metrics->setSpacing(0);
    const QStringList names = {QStringLiteral("当前演示报价"), QStringLiteral("样本最低报价"), QStringLiteral("样本最高报价")};
    for (int i = 0; i < names.size(); ++i) {
        auto* metric = new MetricCard(names[i], QStringLiteral("—"), QStringLiteral("演示价格 · 非实时市场数据"));
        metric->setObjectName(QStringLiteral("priceMetric%1").arg(i));
        metric->setProperty("separator", i < names.size() - 1);
        m_priceCards.append(metric);
        metrics->addWidget(metric, 1);
    }
    layout->addWidget(strip);
    layout->addSpacing(28);

    sectionHeader(layout, QStringLiteral("价格走势"))->addWidget(label(QStringLiteral("24 个样本 · 演示数据"), QStringLiteral("tertiaryLabel")));
    auto* chartPanel = card(QStringLiteral("priceChartPanel"));
    auto* chartLayout = new QVBoxLayout(chartPanel);
    chartLayout->setContentsMargins(20, 16, 20, 12);
    m_priceChart = new PriceChart;
    m_priceChart->setObjectName(QStringLiteral("priceHistoryChart"));
    m_priceChart->setMinimumHeight(220);
    chartLayout->addWidget(m_priceChart, 1);
    layout->addWidget(chartPanel, 3);
    layout->addSpacing(28);

    sectionHeader(layout, QStringLiteral("样本记录"))->addWidget(label(QStringLiteral("本地合成数据"), QStringLiteral("tertiaryLabel")));
    auto* historyPanel = card(QStringLiteral("priceHistoryPanel"));
    auto* historyLayout = new QVBoxLayout(historyPanel);
    historyLayout->setContentsMargins(12, 6, 12, 8);
    m_priceHistory = table({QStringLiteral("样本"), QStringLiteral("商品"), QStringLiteral("演示价格"), QStringLiteral("类型"), QStringLiteral("数据来源")},
                           QStringLiteral("priceHistoryTable"), RowStyle::Lines);
    auto* historyColumns = m_priceHistory->horizontalHeader();
    historyColumns->setStretchLastSection(false);
    historyColumns->setSectionResizeMode(QHeaderView::Fixed);
    historyColumns->setSectionResizeMode(1, QHeaderView::Stretch);
    m_priceHistory->setColumnWidth(0, 96);
    m_priceHistory->setColumnWidth(2, 120);
    m_priceHistory->setColumnWidth(3, 112);
    m_priceHistory->setColumnWidth(4, 220);
    m_priceHistory->setMinimumHeight(300);
    historyLayout->addWidget(m_priceHistory, 1);
    layout->addWidget(historyPanel, 3);
    connect(m_priceSkin, &QComboBox::currentIndexChanged, this, [this] { if (!m_refreshing) refreshPrices(); });
    connect(exportButton, &QPushButton::clicked, this, &MainWindow::exportPrices);
    return page;
}

QWidget* MainWindow::buildStats()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("statsPage"));
    auto* layout = pageLayout(page);
    pageHeader(layout, titles[4]);
    layout->addSpacing(20);
    auto* strip = card(QStringLiteral("metricStrip"));
    auto* metrics = new QHBoxLayout(strip);
    metrics->setContentsMargins(0, 8, 0, 8);
    metrics->setSpacing(0);
    const QStringList names = {QStringLiteral("模拟扫描"), QStringLiteral("模拟条件命中"), QStringLiteral("模拟确认成功"), QStringLiteral("实际成交 / 实际支出")};
    const QStringList details = {QStringLiteral("本次演示扫描条目"), QStringLiteral("模拟筛选符合条件"), QStringLiteral("只计入模拟确认结果"), QStringLiteral("未连接执行模块")};
    for (int i = 0; i < names.size(); ++i) {
        auto* metric = new MetricCard(names[i], i == 3 ? QStringLiteral("—") : QStringLiteral("0"), details[i]);
        metric->setObjectName(QStringLiteral("statsMetric%1").arg(i));
        metric->setProperty("separator", i < names.size() - 1);
        m_statsCards.append(metric);
        metrics->addWidget(metric, 1);
    }
    layout->addWidget(strip);
    layout->addSpacing(28);

    auto* content = new QHBoxLayout;
    content->setSpacing(16);
    auto* left = new QVBoxLayout;
    left->setSpacing(0);
    sectionHeader(left, QStringLiteral("模拟结果分布"))->addWidget(label(QStringLiteral("基于本次演示计数"), QStringLiteral("tertiaryLabel")));
    auto* distribution = card(QStringLiteral("distributionPanel"));
    auto* distributionLayout = new QVBoxLayout(distribution);
    distributionLayout->setContentsMargins(24, 22, 24, 20);
    distributionLayout->setSpacing(0);
    const QStringList reasons = {QStringLiteral("未命中筛选条件"), QStringLiteral("已命中 · 尚未模拟确认"), QStringLiteral("模拟确认成功")};
    for (const auto& reason : reasons) {
        auto* row = new QHBoxLayout;
        row->addWidget(label(reason));
        row->addStretch();
        auto* value = label(QStringLiteral("0"), QStringLiteral("cardCaption"));
        m_reasonLabels.append(value);
        row->addWidget(value);
        distributionLayout->addLayout(row);
        distributionLayout->addSpacing(10);
        auto* bar = new QProgressBar;
        bar->setRange(0, 100);
        bar->setValue(0);
        bar->setTextVisible(false);
        m_reasonBars.append(bar);
        distributionLayout->addWidget(bar);
        distributionLayout->addSpacing(22);
    }
    distributionLayout->addStretch();
    m_statsNote = label(QString(), QStringLiteral("tertiaryLabel"));
    m_statsNote->setWordWrap(true);
    distributionLayout->addWidget(m_statsNote);
    left->addWidget(distribution, 1);
    content->addLayout(left, 3);

    auto* right = new QVBoxLayout;
    right->setSpacing(0);
    sectionHeader(right, QStringLiteral("统计口径"));
    auto* semantics = card(QStringLiteral("semanticsPanel"));
    auto* semanticsLayout = new QVBoxLayout(semantics);
    semanticsLayout->setContentsMargins(24, 22, 24, 20);
    semanticsLayout->setSpacing(0);
    const QVector<QPair<QString, QString>> explanations = {
        {QStringLiteral("扫描 ≠ 命中"), QStringLiteral("扫描记录观察次数；命中记录符合任务条件的次数。")},
        {QStringLiteral("尝试 ≠ 成交"), QStringLiteral("当前只有本地模拟，不产生实际交易和实际支出。")},
        {QStringLiteral("未决结果单独记"), QStringLiteral("接入执行模块后，结果未明的动作不计入成功。当前演示未决为 0。")}};
    for (const auto& entry : explanations) {
        semanticsLayout->addWidget(label(entry.first, QStringLiteral("cardTitle")));
        semanticsLayout->addSpacing(4);
        auto* description = label(entry.second, QStringLiteral("cardCaption"));
        description->setWordWrap(true);
        semanticsLayout->addWidget(description);
        semanticsLayout->addSpacing(18);
    }
    semanticsLayout->addStretch();
    right->addWidget(semantics, 1);
    content->addLayout(right, 2);
    layout->addLayout(content);
    layout->addStretch(1);
    return page;
}

QWidget* MainWindow::buildLogs()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("logsPage"));
    auto* layout = pageLayout(page);
    auto* header = pageHeader(layout, titles[5], &m_logCount);
    m_logSearch = new QLineEdit;
    m_logSearch->setObjectName(QStringLiteral("logSearch"));
    m_logSearch->setPlaceholderText(QStringLiteral("搜索事件内容"));
    m_logSearch->setFixedWidth(280);
    m_logSearch->addAction(FluentTheme::glyphIcon(Glyph::Search, FluentTheme::secondary, FluentTheme::text), QLineEdit::TrailingPosition);
    auto* clearSearch = m_logSearch->addAction(FluentTheme::glyphIcon(Glyph::Clear, FluentTheme::secondary, FluentTheme::text), QLineEdit::TrailingPosition);
    clearSearch->setVisible(false);
    connect(clearSearch, &QAction::triggered, m_logSearch, &QLineEdit::clear);
    connect(m_logSearch, &QLineEdit::textChanged, clearSearch, [clearSearch](const QString& text) { clearSearch->setVisible(!text.isEmpty()); });
    header->addWidget(m_logSearch, 0, Qt::AlignVCenter);
    m_logLevel = new FluentComboBox;
    m_logLevel->setObjectName(QStringLiteral("logLevel"));
    m_logLevel->addItems({QStringLiteral("全部级别"), "INFO", "DEMO", "SUCCESS", "WARN", "ERROR"});
    m_logLevel->setFixedWidth(140);
    header->addWidget(m_logLevel, 0, Qt::AlignVCenter);
    auto* clear = button(QStringLiteral("清空日志"), QStringLiteral("clearLogsButton"), ButtonKind::Standard, Glyph::Delete);
    header->addWidget(clear, 0, Qt::AlignVCenter);
    layout->addSpacing(20);
    auto* box = card(QStringLiteral("logPanel"));
    auto* boxLayout = new QVBoxLayout(box);
    boxLayout->setContentsMargins(12, 6, 12, 8);
    m_logsTable = table({QStringLiteral("时间"), QStringLiteral("级别"), QStringLiteral("内容")}, QStringLiteral("logsTable"), RowStyle::Lines);
    auto* columns = m_logsTable->horizontalHeader();
    columns->setStretchLastSection(false);
    columns->setSectionResizeMode(QHeaderView::Fixed);
    columns->setSectionResizeMode(2, QHeaderView::Stretch);
    m_logsTable->setColumnWidth(0, 96);
    m_logsTable->setColumnWidth(1, 124);
    m_logsTable->setMinimumHeight(360);
    boxLayout->addWidget(m_logsTable, 1);
    layout->addWidget(box, 1);
    connect(m_logSearch, &QLineEdit::textChanged, this, [this] { if (!m_refreshing) refreshLogs(); });
    connect(m_logLevel, &QComboBox::currentTextChanged, this, [this] { if (!m_refreshing) refreshLogs(); });
    connect(clear, &QPushButton::clicked, this, [this] {
        if (confirm(this, QStringLiteral("清空本地日志"), QStringLiteral("只清空当前本地演示日志，任务与配置不变。"), QStringLiteral("清空"))) {
            m_state->logs.clear();
            m_state->notifyChanged();
        }
    });
    return page;
}

QWidget* MainWindow::buildSettings()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("settingsPage"));
    auto* layout = pageLayout(page);
    pageHeader(layout, titles[6]);
    const auto group = [&](const QString& text, bool first = false) {
        layout->addSpacing(first ? 20 : 28);
        layout->addWidget(label(text, QStringLiteral("groupLabel")));
        layout->addSpacing(8);
    };
    const auto row = [&](QWidget* item) { layout->addWidget(item); layout->addSpacing(4); };
    group(QStringLiteral("显示"), true);
    auto* compact = new ToggleSwitch;
    compact->setObjectName(QStringLiteral("compactTablesCheck"));
    compact->setStateTextVisible(true);
    m_compactTables = compact;
    row(settingsCard(glyphLabel(Glyph::List), QStringLiteral("紧凑列表"), QStringLiteral("降低列表行高，一屏显示更多条目"), compact));
    auto* autoScroll = new ToggleSwitch;
    autoScroll->setObjectName(QStringLiteral("autoScrollLogsCheck"));
    autoScroll->setStateTextVisible(true);
    m_autoScrollLogs = autoScroll;
    row(settingsCard(glyphLabel(Glyph::History), QStringLiteral("日志自动定位"), QStringLiteral("新事件出现时，日志列表定位到最新一条"), autoScroll));

    group(QStringLiteral("本地配置"));
    m_configDirectory = new QLineEdit;
    m_configDirectory->setObjectName(QStringLiteral("configDirectoryEdit"));
    m_configDirectory->setReadOnly(true);
    m_configDirectory->setFixedWidth(460);
    row(settingsCard(glyphLabel(Glyph::Folder), QStringLiteral("配置文件"), QStringLiteral("关注、任务与运行参数保存在此文件；退出程序时自动保存"), m_configDirectory));
    layout->addSpacing(8);
    auto* actionRow = new QHBoxLayout;
    m_settingsMessage = label(QString(), QStringLiteral("settingsMessage"));
    m_settingsMessage->setWordWrap(true);
    actionRow->addWidget(m_settingsMessage, 1);
    auto* save = button(QStringLiteral("保存本地设置"), QStringLiteral("saveSettingsButton"), ButtonKind::Accent, Glyph::Save);
    actionRow->addWidget(save);
    layout->addLayout(actionRow);

    group(QStringLiteral("运行模式"));
    row(settingsCard(glyphLabel(Glyph::Pulse), QStringLiteral("前端独立演示"),
                     QStringLiteral("使用本地演示数据验证关注管理、任务配置、价格展示与统计交互；没有连接游戏进程，不读取实时市场，也不执行实际购买。"),
                     label(QStringLiteral("本地预览"), QStringLiteral("settingsBadge")), QStringLiteral("runModeCard")));
    group(QStringLiteral("数据连接"));
    row(settingsCard(glyphLabel(Glyph::Link), QStringLiteral("游戏页面采集 · 价格识别与校验 · 自动操作与结果确认"),
                     QStringLiteral("界面层与这些模块保持分离；前端完成后可以逐个接入。"),
                     label(QStringLiteral("尚未接入"), QStringLiteral("settingsBadge"))));
    group(QStringLiteral("关于"));
    row(settingsCard(new AppMark(24), QStringLiteral("Relink Studio"),
                     QStringLiteral("版本 %1 · 本地演示前端 · C++17 / Qt %2 Widgets").arg(QCoreApplication::applicationVersion(), QStringLiteral(QT_VERSION_STR)),
                     nullptr, QStringLiteral("aboutCard")));
    layout->addStretch();
    connect(save, &QPushButton::clicked, this, &MainWindow::saveSettings);
    connect(m_compactTables, &QCheckBox::toggled, this, &MainWindow::applyDensity);
    return page;
}

// Every entry of the original assistant's main screen, as Windows 11 settings cards.
QWidget* MainWindow::buildRunSettings()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("runSettingsPage"));
    auto* layout = pageLayout(page);
    QLabel* profileCaption = nullptr;
    auto* header = pageHeader(layout, titles[RunPage], &profileCaption);
    auto* runMessage = label(QString(), QStringLiteral("settingsMessage"));
    auto* save = button(QStringLiteral("保存参数"), QStringLiteral("saveRunSettingsButton"), ButtonKind::Accent, Glyph::Save);
    header->addWidget(runMessage, 0, Qt::AlignVCenter);
    header->addSpacing(8);
    header->addWidget(save, 0, Qt::AlignVCenter);
    m_runBinders.append([this, profileCaption] { profileCaption->setText(QStringLiteral("方案 %1").arg(m_state->run.profile)); });
    connect(save, &QPushButton::clicked, this, [this, runMessage] { saveSettings(); runMessage->setText(m_settingsMessage->text()); });

    const auto group = [&](const QString& text, bool first = false) {
        layout->addSpacing(first ? 20 : 28);
        layout->addWidget(label(text, QStringLiteral("groupLabel")));
        layout->addSpacing(8);
    };
    const auto row = [&](QWidget* item) { layout->addWidget(item); layout->addSpacing(4); };
    // PR07: import is intentionally a preview-only operation.  The button is
    // deterministic and uses an in-memory fixture so the self-test can verify
    // the dialog without opening a file chooser or touching the workspace.
    m_importPreviewButton = button(QStringLiteral("打开只读预览"), QStringLiteral("runImportPreviewButton"), ButtonKind::Standard, Glyph::Import);
    connect(m_importPreviewButton, &QPushButton::clicked, this, &MainWindow::previewImportDemo);
    row(settingsCard(glyphLabel(Glyph::Import), QStringLiteral("导入预览"),
                     QStringLiteral("先检查 schema、行状态和诊断；当前阶段不会写入配置，也不会启用任何任务。"),
                     m_importPreviewButton));
    const auto text = [](const QString& value) { return label(value); };
    const auto strip = [](std::initializer_list<QWidget*> parts) {
        auto* host = new QWidget;
        auto* line = new QHBoxLayout(host);
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(8);
        for (auto* part : parts) line->addWidget(part);
        return host;
    };
    // Each control writes its field immediately; binders push the model back
    // into the controls after an import, a reset or a save.
    const auto toggle = [&](const QString& name, bool RunSettings::* field) {
        auto* item = new ToggleSwitch;
        item->setObjectName(name);
        item->setStateTextVisible(true);
        connect(item, &QCheckBox::toggled, this, [this, field](bool on) { m_state->run.*field = on; });
        m_runBinders.append([this, item, field] { QSignalBlocker guard(item); item->setChecked(m_state->run.*field); });
        return item;
    };
    const auto integer = [&](const QString& name, int RunSettings::* field, int low, int high, const QString& suffix, int width) {
        auto* item = new FluentSpinBox;
        item->setObjectName(name);
        item->setRange(low, high);
        item->setSuffix(suffix);
        item->setFixedWidth(width);
        connect(item, &QSpinBox::valueChanged, this, [this, field](int value) { m_state->run.*field = value; });
        m_runBinders.append([this, item, field] {
            if (item->hasFocus()) return;
            QSignalBlocker guard(item);
            item->setValue(m_state->run.*field);
        });
        return item;
    };
    const auto step = [&](const QString& name, double RunSettings::* field) {
        auto* item = new FluentDoubleSpinBox;
        item->setObjectName(name);
        item->setRange(0, 1000);
        item->setDecimals(1);
        item->setSingleStep(0.5);
        item->setSuffix(QStringLiteral(" ms"));
        item->setFixedWidth(116);
        connect(item, &QDoubleSpinBox::valueChanged, this, [this, field](double value) { m_state->run.*field = value; });
        m_runBinders.append([this, item, field] {
            if (item->hasFocus()) return;
            QSignalBlocker guard(item);
            item->setValue(m_state->run.*field);
        });
        return item;
    };

    group(QStringLiteral("运行"), true);
    auto* profile = new QLineEdit;
    profile->setObjectName(QStringLiteral("runProfileEdit"));
    profile->setMaxLength(40);
    profile->setFixedWidth(240);
    connect(profile, &QLineEdit::editingFinished, this, [this, profile] {
        const QString value = profile->text().trimmed();
        if (value.isEmpty()) {
            profile->setText(m_state->run.profile);
            return;
        }
        m_state->run.profile = value;
        refreshRunSettings();
    });
    m_runBinders.append([this, profile] { if (!profile->hasFocus()) profile->setText(m_state->run.profile); });
    row(settingsCard(glyphLabel(Glyph::Tag), QStringLiteral("配置方案"), QStringLiteral("对应原程序顶部的方案标签；导出、导入时随参数一起保存"), profile));
    auto* hotkey = new FluentComboBox;
    hotkey->setObjectName(QStringLiteral("runHotkeyCombo"));
    for (int key = 1; key <= 12; ++key) hotkey->addItem(QStringLiteral("F%1").arg(key));
    hotkey->setFixedWidth(104);
    connect(hotkey, &QComboBox::currentTextChanged, this, [this](const QString& key) { if (!key.isEmpty()) m_state->run.hotkey = key; });
    m_runBinders.append([this, hotkey] { QSignalBlocker guard(hotkey); hotkey->setCurrentText(m_state->run.hotkey); });
    row(settingsCard(glyphLabel(Glyph::Keyboard), QStringLiteral("运行快捷键"), QStringLiteral("按下后开始或停止运行；接入执行模块后生效"), hotkey));
    auto* scheduleStart = new FluentTimeEdit;
    scheduleStart->setObjectName(QStringLiteral("runScheduleStart"));
    auto* scheduleStop = new FluentTimeEdit;
    scheduleStop->setObjectName(QStringLiteral("runScheduleStop"));
    for (auto* edit : {scheduleStart, scheduleStop}) {
        edit->setDisplayFormat(QStringLiteral("HH:mm"));
        edit->setFixedWidth(100);
    }
    connect(scheduleStart, &QTimeEdit::timeChanged, this, [this](const QTime& time) { m_state->run.scheduleStart = time.toString(QStringLiteral("HH:mm")); });
    connect(scheduleStop, &QTimeEdit::timeChanged, this, [this](const QTime& time) { m_state->run.scheduleStop = time.toString(QStringLiteral("HH:mm")); });
    auto* schedule = toggle(QStringLiteral("runScheduleToggle"), &RunSettings::scheduleEnabled);
    connect(schedule, &QCheckBox::toggled, scheduleStart, &QWidget::setEnabled);
    connect(schedule, &QCheckBox::toggled, scheduleStop, &QWidget::setEnabled);
    m_runBinders.append([this, scheduleStart, scheduleStop] {
        for (auto* edit : {scheduleStart, scheduleStop}) {
            edit->setEnabled(m_state->run.scheduleEnabled);
            if (edit->hasFocus()) continue;
            QSignalBlocker guard(edit);
            edit->setTime(QTime::fromString(edit == scheduleStart ? m_state->run.scheduleStart : m_state->run.scheduleStop, QStringLiteral("HH:mm")));
        }
    });
    row(settingsCard(glyphLabel(Glyph::Clock), QStringLiteral("定时运行"), QStringLiteral("在设定时间自动开始和停止；接入执行模块后生效"),
                     strip({text(QStringLiteral("开始")), scheduleStart, text(QStringLiteral("结束")), scheduleStop, schedule})));

    group(QStringLiteral("购买延迟"));
    row(settingsCard(glyphLabel(Glyph::Stopwatch), QStringLiteral("购买延迟"), QStringLiteral("每次购买操作之间的等待时间"),
                     integer(QStringLiteral("runPurchaseDelay"), &RunSettings::purchaseDelayMs, 0, 60000, QStringLiteral(" ms"), 140)));
    row(settingsCard(nullptr, QStringLiteral("动态延迟"), QStringLiteral("开启动态延迟调整（跳过抽奖页：开启）。原程序标注为不建议"),
                     toggle(QStringLiteral("runDynamicDelay"), &RunSettings::dynamicDelay), QString(), true));
    row(settingsCard(nullptr, QStringLiteral("队列已满时减延迟"), QStringLiteral("每触发设定次数，购买延迟减少一次"),
                     strip({text(QStringLiteral("触发")), integer(QStringLiteral("runQueueTrigger"), &RunSettings::queueFullTrigger, 1, 9999, QStringLiteral(" 次"), 104),
                            text(QStringLiteral("减少")), step(QStringLiteral("runQueueStep"), &RunSettings::queueFullStepMs)}), QString(), true));
    row(settingsCard(nullptr, QStringLiteral("公示期加延迟"), QStringLiteral("处于公示期时，每触发设定次数，购买延迟增加一次"),
                     strip({text(QStringLiteral("触发")), integer(QStringLiteral("runPublicityTrigger"), &RunSettings::publicityTrigger, 1, 9999, QStringLiteral(" 次"), 104),
                            text(QStringLiteral("增加")), step(QStringLiteral("runPublicityStep"), &RunSettings::publicityStepMs)}), QString(), true));

    group(QStringLiteral("连点模式"));
    auto* burst = toggle(QStringLiteral("runBurstClick"), &RunSettings::burstClick);
    auto* interval = integer(QStringLiteral("runClickInterval"), &RunSettings::clickIntervalMs, 1, 10000, QStringLiteral(" ms"), 140);
    connect(burst, &QCheckBox::toggled, interval, &QWidget::setEnabled);
    m_runBinders.append([this, interval] { interval->setEnabled(m_state->run.burstClick); });
    row(settingsCard(glyphLabel(Glyph::Mouse), QStringLiteral("连续点击"), QStringLiteral("开启连点模式，按设定间隔连续点击购买"), burst));
    row(settingsCard(nullptr, QStringLiteral("点击间隔"), QStringLiteral("两次点击之间的间隔"), interval, QString(), true));

    group(QStringLiteral("购买量限制"));
    row(settingsCard(glyphLabel(Glyph::Filter), QStringLiteral("按品级限制购买数量"), QStringLiteral("橙色、紫色、蓝色品级分别计数"),
                     strip({text(QStringLiteral("橙色")), integer(QStringLiteral("runLimitOrange"), &RunSettings::limitOrange, 0, 9999, QString(), 96),
                            text(QStringLiteral("紫色")), integer(QStringLiteral("runLimitPurple"), &RunSettings::limitPurple, 0, 9999, QString(), 96),
                            text(QStringLiteral("蓝色")), integer(QStringLiteral("runLimitBlue"), &RunSettings::limitBlue, 0, 9999, QString(), 96)})));

    group(QStringLiteral("挂机设置"));
    row(settingsCard(glyphLabel(Glyph::Sync), QStringLiteral("刷新界面"), QStringLiteral("挂机时定时刷新市场界面。原程序标注为不推荐"),
                     toggle(QStringLiteral("runRefreshPage"), &RunSettings::refreshPage)));
    row(settingsCard(glyphLabel(Glyph::SkipTo), QStringLiteral("跳过抽奖页"), QStringLiteral("挂机时自动跳过抽奖页"),
                     toggle(QStringLiteral("runSkipLottery"), &RunSettings::skipLotteryPage)));
    row(settingsCard(glyphLabel(Glyph::SkipTo), QStringLiteral("跳过成功页"), QStringLiteral("购买成功后自动跳过结果页"),
                     toggle(QStringLiteral("runSkipSuccess"), &RunSettings::skipSuccessPage)));

    group(QStringLiteral("自动收藏"));
    row(settingsCard(glyphLabel(Glyph::Star), QStringLiteral("自动收藏模式"), QStringLiteral("按收藏任务配置自动收藏符合条件的条目"),
                     toggle(QStringLiteral("runAutoCollect"), &RunSettings::autoCollect)));
    row(settingsCard(glyphLabel(Glyph::Display), QStringLiteral("收藏状态 OSD"), QStringLiteral("在游戏画面上显示收藏状态浮层"),
                     toggle(QStringLiteral("runCollectOsd"), &RunSettings::collectOsd)));
    m_runTaskSummary = label(QString(), QStringLiteral("cardCaption"));
    auto* manageTasks = link(QStringLiteral("管理任务"), QStringLiteral("runManageTasksLink"));
    connect(manageTasks, &QPushButton::clicked, this, [this] { setPage(2); });
    row(settingsCard(glyphLabel(Glyph::Tasks), QStringLiteral("收藏任务配置"), QStringLiteral("皮肤、成色、最大磨损、价格区间与限制量"),
                     strip({m_runTaskSummary, manageTasks})));

    group(QStringLiteral("优化与记录"));
    row(settingsCard(glyphLabel(Glyph::Speed), QStringLiteral("优化"), QStringLiteral("原程序顶部的「优化」入口；具体优化项目待确认后接入"),
                     label(QStringLiteral("待确认"), QStringLiteral("settingsBadge"))));
    auto* records = link(QStringLiteral("查看日志"), QStringLiteral("runRecordsLink"));
    connect(records, &QPushButton::clicked, this, [this] { setPage(5); });
    row(settingsCard(glyphLabel(Glyph::History), QStringLiteral("运行记录"), QStringLiteral("日志与统计页记录每次运行的事件和结果"), records));
    auto* help = link(QStringLiteral("打开帮助"), QStringLiteral("runHelpLink"));
    connect(help, &QPushButton::clicked, this, &MainWindow::showHelp);
    row(settingsCard(glyphLabel(Glyph::Help), QStringLiteral("帮助"), QStringLiteral("使用流程、快捷键与当前版本说明"), help));
    layout->addSpacing(16);
    auto* note = label(QStringLiteral("以上参数随配置一起保存、导出和导入。当前版本不连接游戏，不会执行点击或购买。"), QStringLiteral("tertiaryLabel"));
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addStretch();
    return page;
}

void MainWindow::setPage(int index)
{
    if (index < 0 || index >= m_pages->count()) return;
    m_pages->setCurrentIndex(index);
    for (int i = 0; i < m_nav.size(); ++i) {
        if (!m_nav[i]) continue;
        QSignalBlocker guard(m_nav[i]);
        m_nav[i]->setChecked(i == index);
    }
    QTimer::singleShot(0, this, &MainWindow::fitWatchlistColumns);
}

QString MainWindow::currentPageName() const { return titles.value(m_pages->currentIndex()); }

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (m_favoriteSearch) {
        const int searchWidth = qBound(300, qRound(width() * 0.25), 520);
        m_favoriteSearch->setFixedWidth(searchWidth);
        m_favoriteSearch->move((width() - searchWidth) / 2, 8);
        m_favoriteSearch->raise();
    }
    if (m_inspector) m_inspector->setFixedWidth(width() < 1360 ? 300 : 320);
    QTimer::singleShot(0, this, &MainWindow::fitWatchlistColumns);
}

void MainWindow::fitWatchlistColumns()
{
    if (!m_favoritesTable) return;
    const int w = m_favoritesTable->viewport()->width();
    m_favoritesTable->setColumnWidth(0, 44);
    m_favoritesTable->setColumnWidth(2, qBound(64, qRound(w * .08), 92));
    m_favoritesTable->setColumnWidth(3, qBound(84, qRound(w * .11), 120));
    m_favoritesTable->setColumnWidth(4, qBound(100, qRound(w * .16), 180));
    m_favoritesTable->setColumnWidth(5, qBound(84, qRound(w * .11), 120));
    m_favoritesTable->setColumnWidth(6, qBound(92, qRound(w * .11), 124));
}

void MainWindow::refreshReplayProjection()
{
    if (!m_replayController || !m_replaySourceLabel) return;
    const auto view = relink::runtime::UiProjection::replay(m_replayController->snapshot());
    m_replaySourceLabel->setText(view.source);
    m_replayStateLabel->setText(view.runStateText);
    m_replayReasonLabel->setText(view.matchReason);
    m_replayProgressLabel->setText(view.progress);
    m_replayStartButton->setEnabled(view.canStart);
    m_replayPauseButton->setEnabled(view.canPause);
    m_replayResumeButton->setEnabled(view.canResume);
    m_replayStopButton->setEnabled(view.canStop);
    m_replayStateLabel->setProperty("status", view.runState);
    m_replayStateLabel->style()->unpolish(m_replayStateLabel);
    m_replayStateLabel->style()->polish(m_replayStateLabel);
}

void MainWindow::refreshAll()
{
    if (m_refreshing) return;
    m_refreshing = true;
    refreshOverview();
    refreshFavorites();
    refreshTasks();
    refreshPrices();
    refreshStats();
    refreshLogs();
    refreshRunSettings();
    refreshReplayProjection();
    const bool running = m_state->simulationRunning;
    m_startButton->setEnabled(!running);
    m_pauseButton->setEnabled(running);
    m_taskStart->setEnabled(!running);
    m_statusBadge->setText(QStringLiteral("<span style=\"color:%1;\">●</span>&nbsp;&nbsp;%2")
                               .arg((running ? FluentTheme::positive : FluentTheme::muted).name(),
                                    running ? QStringLiteral("模拟运行中") : QStringLiteral("本地演示")));
    m_statusBadge->setToolTip(running ? QStringLiteral("本地模拟正在推进；不连接游戏，不产生订单。")
                                      : QStringLiteral("演示数据 · 未连接游戏"));
    QString path = m_state->configPath;
    if (path.isEmpty()) path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/config.json";
    m_configDirectory->setText(QDir::toNativeSeparators(path));
    m_configDirectory->setCursorPosition(0);
    m_refreshing = false;
}

void MainWindow::refreshRunSettings()
{
    for (const auto& bind : m_runBinders) bind();
    int enabled = 0;
    for (const auto& task : m_state->tasks) if (task.enabled) ++enabled;
    if (m_runTaskSummary)
        m_runTaskSummary->setText(QStringLiteral("%1 条任务 · %2 条启用").arg(m_state->tasks.size()).arg(enabled));
}

void MainWindow::refreshOverview()
{
    int followed = 0, enabled = 0;
    for (const auto& skin : m_state->skins) if (skin.followed) ++followed;
    for (const auto& task : m_state->tasks) if (task.enabled) ++enabled;
    m_overviewCards[0]->setValue(QString::number(followed), QStringLiteral("本地关注条目"));
    m_overviewCards[1]->setValue(QString::number(enabled), QStringLiteral("共 %1 条任务").arg(m_state->tasks.size()));
    m_overviewCards[2]->setValue(QString::number(m_state->simulatedScans), QStringLiteral("本次模拟累计"));
    m_overviewCards[3]->setValue(QString::number(m_state->simulatedSuccess), QStringLiteral("仅模拟结果"));
    const double reference = m_state->skins.isEmpty() ? 100 : m_state->skins.first().price;
    m_overviewChart->setSeries(series(reference));
    m_overviewChart->setCaption(m_state->skins.isEmpty() ? QStringLiteral("演示价格") : m_state->skins.first().name);
    m_overviewTaskCount->setText(QStringLiteral("%1 条任务").arg(m_state->tasks.size()));
    m_overviewTasks->setRowCount(qMin(4, int(m_state->tasks.size())));
    for (int i = 0; i < m_overviewTasks->rowCount(); ++i) {
        const auto& task = m_state->tasks[i];
        put(m_overviewTasks, i, 0, task.name);
        put(m_overviewTasks, i, 1, QString::number(task.quantity));
        putStatus(m_overviewTasks, i, 2, task.status, taskTone(task));
    }
    const int count = qMin(3, int(m_state->logs.size()));
    m_overviewLogs->setRowCount(count);
    for (int r = 0; r < count; ++r) {
        const auto& log = m_state->logs[r];
        put(m_overviewLogs, r, 0, log.time, FluentTheme::secondary);
        putStatus(m_overviewLogs, r, 1, log.level, levelTone(log.level));
        put(m_overviewLogs, r, 2, log.message)->setToolTip(log.message);
    }
    m_overviewLogs->setFixedHeight(qMax(1, count) * m_overviewLogs->verticalHeader()->defaultSectionSize() + 2);
}

QString MainWindow::selectedFavoriteId() const { return selectedId(m_favoritesTable); }

void MainWindow::refreshFavorites()
{
    const QString previous = selectedFavoriteId();
    {
        QSignalBlocker guard(m_rarityFilter);
        const QString rarity = m_rarityFilter->currentText();
        QStringList values;
        for (const auto& skin : m_state->skins) if (!values.contains(skin.rarity)) values.append(skin.rarity);
        m_rarityFilter->clear();
        m_rarityFilter->addItem(QStringLiteral("全部品级"));
        m_rarityFilter->addItems(values);
        m_rarityFilter->setCurrentIndex(qMax(0, m_rarityFilter->findText(rarity)));
    }
    auto matches = [&](const Skin& skin, const QString& key) {
        bool linked = false;
        for (const auto& task : m_state->tasks) if (task.skinId == skin.id) { linked = true; break; }
        return key == "all" || (key == "followed" && skin.followed) || (key == "unfollowed" && !skin.followed)
            || (key == "linked" && linked) || (key == "unlinked" && !linked) || key == skinCategory(skin.name);
    };
    for (auto* filter : m_catalogFilters) {
        const QString key = filter->property("filterKey").toString();
        int count = 0;
        for (const auto& skin : m_state->skins) if (matches(skin, key)) ++count;
        QSignalBlocker guard(filter);
        filter->setCount(count);
        filter->setChecked(key == m_catalogFilter);
    }
    QSignalBlocker guard(m_favoritesTable);
    m_favoritesTable->setRowCount(0);
    int selected = -1;
    const QString query = m_favoriteSearch->text().trimmed();
    for (const auto& skin : m_state->skins) {
        if (!matches(skin, m_catalogFilter)) continue;
        if (!query.isEmpty() && !skin.name.contains(query, Qt::CaseInsensitive) && !skin.series.contains(query, Qt::CaseInsensitive)) continue;
        if (m_rarityFilter->currentIndex() > 0 && skin.rarity != m_rarityFilter->currentText()) continue;
        const Task* linked = nullptr;
        bool enabled = false;
        for (const auto& task : m_state->tasks) {
            if (task.skinId != skin.id) continue;
            if (!linked) linked = &task;
            enabled |= task.enabled;
        }
        if (m_favoriteStateFilter->currentIndex() == 1 && !linked) continue;
        if (m_favoriteStateFilter->currentIndex() == 2 && linked) continue;
        const int row = m_favoritesTable->rowCount();
        m_favoritesTable->insertRow(row);
        if (skin.id == previous) selected = row;
        auto* star = new StarToggle;
        star->setObjectName("follow_" + skin.id);
        star->setChecked(skin.followed);
        star->setToolTip(skin.followed ? QStringLiteral("取消关注") : QStringLiteral("关注"));
        m_favoritesTable->setCellWidget(row, 0, centered(star));
        connect(star, &QCheckBox::toggled, this, [this, id = skin.id](bool checked) {
            if (m_refreshing) return;
            for (auto& value : m_state->skins) if (value.id == id) { value.followed = checked; break; }
            m_state->notifyChanged();
        });
        auto* identity = put(m_favoritesTable, row, 1, skin.name, QColor(), skin.id);
        identity->setData(KindRole, ItemCell);
        identity->setData(SubtitleRole, skin.series);
        identity->setData(ArtRole, skinArtIndex(skin));
        identity->setToolTip(skin.name + "\n" + skin.series + QStringLiteral(" · 演示插图"));
        put(m_favoritesTable, row, 2, QString::number(skin.wear, 'f', 3), FluentTheme::secondary);
        put(m_favoritesTable, row, 3, amount(skin.price));
        QVariantList samples;
        for (double value : series(skin.price)) samples.append(value);
        auto* trend = put(m_favoritesTable, row, 4, QString());
        trend->setData(KindRole, SparkCell);
        trend->setData(SamplesRole, samples);
        put(m_favoritesTable, row, 5, linked ? amount(linked->maxPrice) : QStringLiteral("—"), linked ? FluentTheme::text : FluentTheme::muted);
        putStatus(m_favoritesTable, row, 6,
                  linked ? (enabled ? QStringLiteral("已启用") : QStringLiteral("已暂停")) : QStringLiteral("未配置"),
                  linked ? (enabled ? ToneSuccess : ToneCaution) : ToneInactive);
    }
    if (m_favoritesTable->rowCount() > 0) m_favoritesTable->selectRow(selected < 0 ? 0 : selected);
    m_favoritesCount->setText(QStringLiteral("%1 个条目").arg(m_favoritesTable->rowCount()));
    refreshFavoriteDetail();
}

void MainWindow::refreshFavoriteDetail()
{
    const auto* skin = findSkin(m_state, selectedFavoriteId());
    const bool valid = skin != nullptr;
    for (QWidget* control : {static_cast<QWidget*>(m_favoriteCreate), static_cast<QWidget*>(m_inspectorPrice),
                             static_cast<QWidget*>(m_inspectorWear), static_cast<QWidget*>(m_inspectorQuantity),
                             static_cast<QWidget*>(m_inspectorCondition)})
        control->setEnabled(valid);
    if (!skin) {
        m_inspectorSkinId.clear();
        m_favoriteTitle->setText(QStringLiteral("没有匹配条目"));
        m_favoriteMetadata->setText(QStringLiteral("调整搜索或筛选条件"));
        m_favoritePrice->setText(QStringLiteral("—"));
        m_favoriteChange->clear();
        m_favoriteArt->setArtwork(QPixmap());
        for (auto* fact : {m_factRarity, m_factCondition, m_factWear}) fact->setText(QStringLiteral("—"));
        m_favoriteTaskInfo->clear();
        m_favoriteChart->setSeries({});
        m_favoriteChartTitle->clear();
        return;
    }
    m_favoriteTitle->setText(skin->name);
    m_favoriteMetadata->setText(skin->series + QStringLiteral(" · 演示插图"));
    m_favoriteArt->setArtwork(skinArtwork(skinArtIndex(*skin)));
    m_favoritePrice->setText(amount(skin->price));
    m_favoriteChange->setText(changeText(skin->change));
    // Falling prices read as favourable for a buyer, rising prices as unfavourable.
    const QColor changeColor = skin->change < 0 ? FluentTheme::positive : (skin->change > 0 ? FluentTheme::negative : FluentTheme::secondary);
    m_favoriteChange->setStyleSheet(QStringLiteral("color:%1;").arg(changeColor.name()));
    m_factRarity->setText(skin->rarity);
    m_factCondition->setText(skin->condition);
    m_factWear->setText(QString::number(skin->wear, 'f', 3));
    m_favoriteChart->setSeries(series(skin->price));
    m_favoriteChartTitle->setText(skin->name);
    int tasks = 0;
    const Task* existing = nullptr;
    for (const auto& task : m_state->tasks) if (task.skinId == skin->id) { ++tasks; if (!existing) existing = &task; }
    if (m_inspectorSkinId != skin->id) {
        m_inspectorSkinId = skin->id;
        m_inspectorPrice->setValue(existing ? existing->maxPrice : skin->price);
        m_inspectorWear->setValue(existing ? existing->maxWear : 5);
        m_inspectorQuantity->setValue(existing ? existing->quantity : 1);
        const QString condition = existing ? existing->condition : skin->condition;
        if (m_inspectorCondition->findText(condition) < 0) m_inspectorCondition->addItem(condition);
        m_inspectorCondition->setCurrentText(condition);
    }
    m_favoriteTaskInfo->setText(QStringLiteral("已关联 %1 条任务").arg(tasks));
}

void MainWindow::createInspectorTask()
{
    const auto* skin = findSkin(m_state, selectedFavoriteId());
    if (!skin) return;
    m_inspectorPrice->interpretText();
    m_inspectorWear->interpretText();
    m_inspectorQuantity->interpretText();
    Task task;
    task.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    task.name = skin->name.left(45) + QStringLiteral(" · 价格关注");
    task.skinId = skin->id;
    task.minPrice = 0;
    task.maxPrice = m_inspectorPrice->value();
    task.maxWear = m_inspectorWear->value();
    task.quantity = m_inspectorQuantity->value();
    task.condition = m_inspectorCondition->currentText();
    task.enabled = true;
    task.status = QStringLiteral("待启动");
    m_state->pauseSimulation();
    m_state->tasks.append(task);
    m_state->addLog("INFO", QStringLiteral("从关注工作区创建本地任务：") + task.name);
    m_state->notifyChanged();
    m_favoriteTaskInfo->setText(QStringLiteral("已创建 · 可在任务页编辑"));
}

QString MainWindow::selectedTaskId() const { return selectedId(m_tasksTable); }

void MainWindow::refreshTasks()
{
    const QString previous = selectedTaskId();
    QStringList selected;
    for (const auto& row : m_tasksTable->selectionModel()->selectedRows())
        if (auto* item = m_tasksTable->item(row.row(), 0)) selected.append(item->data(Qt::UserRole).toString());
    QSignalBlocker guard(m_tasksTable);
    m_tasksTable->setRowCount(m_state->tasks.size());
    int active = 0;
    for (int row = 0; row < m_state->tasks.size(); ++row) {
        const auto& task = m_state->tasks[row];
        const auto* skin = findSkin(m_state, task.skinId);
        if (task.enabled) ++active;
        put(m_tasksTable, row, 0, task.name, QColor(), task.id);
        put(m_tasksTable, row, 1, skin ? skinChoice(*skin) : QStringLiteral("目标已移除"), skin ? QColor() : FluentTheme::muted);
        put(m_tasksTable, row, 2, task.condition, FluentTheme::secondary);
        put(m_tasksTable, row, 3, QStringLiteral("%1 — %2").arg(amount(task.minPrice), amount(task.maxPrice)));
        put(m_tasksTable, row, 4, QString::number(task.maxWear, 'f', 3), FluentTheme::secondary);
        put(m_tasksTable, row, 5, QString::number(task.quantity));
        auto* toggle = new ToggleSwitch;
        toggle->setObjectName("taskEnabled_" + task.id);
        toggle->setChecked(task.enabled);
        toggle->setToolTip(task.enabled ? QStringLiteral("暂停任务") : QStringLiteral("启用任务"));
        m_tasksTable->setCellWidget(row, 6, centered(toggle));
        connect(toggle, &QCheckBox::toggled, this, [this, id = task.id](bool checked) {
            if (m_refreshing) return;
            for (auto& value : m_state->tasks) {
                if (value.id != id) continue;
                value.enabled = checked;
                value.status = checked ? QStringLiteral("待启动") : QStringLiteral("演示已暂停");
                break;
            }
            m_state->notifyChanged();
        });
        putStatus(m_tasksTable, row, 7, task.status, taskTone(task));
    }
    m_tasksTable->clearSelection();
    for (int row = 0; row < m_tasksTable->rowCount(); ++row) {
        const QString id = m_tasksTable->item(row, 0)->data(Qt::UserRole).toString();
        if (selected.contains(id))
            m_tasksTable->selectionModel()->select(m_tasksTable->model()->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        if (id == previous) m_tasksTable->setCurrentCell(row, 0, QItemSelectionModel::NoUpdate);
    }
    m_taskCount->setText(QStringLiteral("%1 条任务 · %2 条启用").arg(m_state->tasks.size()).arg(active));
}

void MainWindow::refreshPrices()
{
    const QString selected = m_priceSkin->currentData().toString();
    {
        QSignalBlocker guard(m_priceSkin);
        m_priceSkin->clear();
        for (const auto& skin : m_state->skins) m_priceSkin->addItem(skin.name, skin.id);
        m_priceSkin->setCurrentIndex(qMax(0, m_priceSkin->findData(selected)));
    }
    const auto* skin = findSkin(m_state, m_priceSkin->currentData().toString());
    if (!skin) {
        for (auto* metric : m_priceCards) metric->setValue(QStringLiteral("—"), QStringLiteral("暂无皮肤样本"));
        m_priceChart->setSeries({});
        m_priceHistory->setRowCount(0);
        return;
    }
    const auto values = series(skin->price);
    m_priceCards[0]->setValue(amount(skin->price), QStringLiteral("演示报价 · 非实际成交价"));
    m_priceCards[1]->setValue(amount(*std::min_element(values.begin(), values.end())), QStringLiteral("合成样本范围"));
    m_priceCards[2]->setValue(amount(*std::max_element(values.begin(), values.end())), QStringLiteral("合成样本范围"));
    m_priceChart->setSeries(values);
    m_priceChart->setCaption(skin->name + QStringLiteral(" · 演示数据"));
    m_priceHistory->setRowCount(6);
    for (int row = 0; row < 6; ++row) {
        const int index = values.size() - 1 - row;
        put(m_priceHistory, row, 0, QStringLiteral("样本 %1").arg(index + 1), FluentTheme::secondary);
        put(m_priceHistory, row, 1, skin->name);
        put(m_priceHistory, row, 2, amount(values[index]));
        put(m_priceHistory, row, 3, QStringLiteral("演示报价"), FluentTheme::secondary);
        put(m_priceHistory, row, 4, QStringLiteral("本地合成 · 非游戏采集"), FluentTheme::secondary);
    }
}

void MainWindow::refreshStats()
{
    const int scans = m_state->simulatedScans, matches = m_state->simulatedMatches, success = m_state->simulatedSuccess;
    m_statsCards[0]->setValue(QString::number(scans), QStringLiteral("本次演示扫描条目"));
    m_statsCards[1]->setValue(QString::number(matches), QStringLiteral("符合模拟筛选条件"));
    m_statsCards[2]->setValue(QString::number(success), QStringLiteral("不是实际成交"));
    m_statsCards[3]->setValue(QStringLiteral("—"), QStringLiteral("未连接执行模块"));
    const QVector<int> values = {qMax(0, scans - matches), qMax(0, matches - success), success};
    for (int i = 0; i < values.size(); ++i) {
        const double fraction = scans > 0 ? values[i] * 100.0 / scans : 0;
        m_reasonLabels[i]->setText(QStringLiteral("%1 · %2%").arg(values[i]).arg(fraction, 0, 'f', 1));
        m_reasonBars[i]->setValue(qRound(fraction));
    }
    m_statsNote->setText(QStringLiteral("模拟未决：0。统计只描述本地模拟引擎，不代表实际游戏结果。"));
}

void MainWindow::refreshLogs()
{
    m_logsTable->setRowCount(0);
    const QString query = m_logSearch->text().trimmed();
    for (const auto& log : m_state->logs) {
        if (m_logLevel->currentIndex() > 0 && log.level != m_logLevel->currentText()) continue;
        if (!query.isEmpty() && !log.message.contains(query, Qt::CaseInsensitive) && !log.level.contains(query, Qt::CaseInsensitive)) continue;
        const int row = m_logsTable->rowCount();
        m_logsTable->insertRow(row);
        put(m_logsTable, row, 0, log.time, FluentTheme::secondary);
        putStatus(m_logsTable, row, 1, log.level, levelTone(log.level));
        put(m_logsTable, row, 2, log.message)->setToolTip(log.message);
    }
    m_logCount->setText(QStringLiteral("显示 %1 / %2 条").arg(m_logsTable->rowCount()).arg(m_state->logs.size()));
    if (m_autoScrollLogs && m_autoScrollLogs->isChecked()) m_logsTable->scrollToTop();
}

void MainWindow::showHelp()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("helpDialog"));
    dialog.setWindowTitle(QStringLiteral("帮助"));
    auto* outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* content = new QFrame;
    content->setObjectName(QStringLiteral("dialogContent"));
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(24, 22, 24, 22);
    body->setSpacing(0);
    body->addWidget(label(QStringLiteral("使用说明"), QStringLiteral("dialogTitle")));
    body->addSpacing(16);
    const QVector<QPair<QString, QString>> steps = {
        {QStringLiteral("关注"), QStringLiteral("筛选或搜索皮肤，在右侧填写最高价格、成色、最大磨损和数量后创建任务。")},
        {QStringLiteral("任务"), QStringLiteral("管理收藏任务：目标皮肤、成色、价格区间、最大磨损、限制量与启用开关。")},
        {QStringLiteral("运行"), QStringLiteral("设置购买延迟、连点模式、品级限购、挂机与自动收藏参数，以及运行快捷键和定时。")},
        {QStringLiteral("记录"), QStringLiteral("在日志与统计页查看运行事件和结果；导入、导出位于窗口右上角。")}};
    for (const auto& step : steps) {
        body->addWidget(label(step.first, QStringLiteral("cardTitle")));
        body->addSpacing(2);
        auto* text = label(step.second, QStringLiteral("cardCaption"));
        text->setWordWrap(true);
        body->addWidget(text);
        body->addSpacing(14);
    }
    body->addWidget(label(QStringLiteral("运行快捷键：%1（接入执行模块后生效）").arg(m_state->run.hotkey), QStringLiteral("cardCaption")));
    body->addSpacing(4);
    auto* demo = label(QStringLiteral("当前版本是本地演示前端：不连接游戏，不读取实时市场，也不执行点击或购买。"), QStringLiteral("tertiaryLabel"));
    demo->setWordWrap(true);
    body->addWidget(demo);
    outer->addWidget(content, 1);
    auto* commands = new QFrame;
    commands->setObjectName(QStringLiteral("dialogCommands"));
    commands->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* commandRow = new QHBoxLayout(commands);
    commandRow->setContentsMargins(24, 20, 24, 20);
    commandRow->addStretch();
    auto* close = button(QStringLiteral("知道了"), QStringLiteral("helpCloseButton"), ButtonKind::Accent);
    close->setDefault(true);
    close->setMinimumWidth(120);
    commandRow->addWidget(close);
    outer->addWidget(commands);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    fitDialog(dialog, 560);
    dialog.exec();
}

void MainWindow::editTask(const QString& id, const QString& skinId)
{
    if (m_state->simulationRunning) m_state->pauseSimulation();
    Task existing;
    bool editing = false;
    for (const auto& task : m_state->tasks) if (task.id == id) { existing = task; editing = true; break; }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("taskEditorDialog"));
    dialog.setWindowTitle(editing ? QStringLiteral("编辑模拟任务") : QStringLiteral("新增模拟任务"));
    dialog.setMinimumWidth(540);
    auto* outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    // ContentDialog layout: white content area above a Mica command bar.
    auto* content = new QFrame;
    content->setObjectName(QStringLiteral("dialogContent"));
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(24, 22, 24, 20);
    body->setSpacing(0);
    body->addWidget(label(editing ? QStringLiteral("调整任务条件") : QStringLiteral("创建一条模拟任务"), QStringLiteral("dialogTitle")));
    body->addSpacing(6);
    body->addWidget(label(QStringLiteral("这些规则只用于本地演示，不会触发实际购买。"), QStringLiteral("cardCaption")));
    body->addSpacing(20);
    auto* name = new QLineEdit;
    name->setObjectName(QStringLiteral("taskName"));
    name->setMaxLength(60);
    name->setPlaceholderText(QStringLiteral("例如：天命低价关注"));
    if (editing) name->setText(existing.name);
    auto* skin = new FluentComboBox;
    skin->setObjectName(QStringLiteral("taskSkin"));
    for (const auto& item : m_state->skins) skin->addItem(skinChoice(item), item.id);
    const int index = skin->findData(editing ? existing.skinId : skinId);
    if (index >= 0) skin->setCurrentIndex(index);
    const auto* defaultSkin = findSkin(m_state, skin->currentData().toString());
    auto* condition = new FluentComboBox;
    condition->setObjectName(QStringLiteral("taskCondition"));
    condition->addItems(conditionOptions(m_state));
    const QString conditionValue = editing ? existing.condition : (defaultSkin ? defaultSkin->condition : QStringLiteral("不限"));
    if (condition->findText(conditionValue) < 0) condition->addItem(conditionValue);
    condition->setCurrentText(conditionValue);
    auto* min = new FluentDoubleSpinBox;
    min->setObjectName(QStringLiteral("taskMinPrice"));
    min->setRange(0, 999999999);
    min->setDecimals(2);
    min->setValue(editing ? existing.minPrice : 0);
    auto* max = new FluentDoubleSpinBox;
    max->setObjectName(QStringLiteral("taskMaxPrice"));
    max->setRange(0, 999999999);
    max->setDecimals(2);
    max->setValue(editing ? existing.maxPrice : (defaultSkin ? defaultSkin->price : 800));
    auto* wear = new FluentDoubleSpinBox;
    wear->setObjectName(QStringLiteral("taskWear"));
    wear->setRange(0, 100);
    wear->setDecimals(6);
    wear->setValue(editing ? existing.maxWear : 5);
    auto* quantity = new FluentSpinBox;
    quantity->setObjectName(QStringLiteral("taskQuantity"));
    quantity->setRange(1, 9999);
    quantity->setValue(editing ? existing.quantity : 1);
    auto* enabled = new ToggleSwitch;
    enabled->setObjectName(QStringLiteral("taskEnabledCheck"));
    enabled->setStateTextVisible(true);
    enabled->setChecked(editing ? existing.enabled : true);
    auto* form = new QGridLayout;
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(4);
    const auto field = [&](const QString& title, QWidget* control, int row, int column, int span = 1) {
        form->addWidget(label(title, QStringLiteral("fieldLabel")), row, column, 1, span);
        form->addWidget(control, row + 1, column, 1, span);
    };
    field(QStringLiteral("任务名称"), name, 0, 0, 2);
    form->setRowMinimumHeight(2, 12);
    field(QStringLiteral("目标皮肤"), skin, 3, 0);
    field(QStringLiteral("成色"), condition, 3, 1);
    form->setRowMinimumHeight(5, 12);
    field(QStringLiteral("最低价格"), min, 6, 0);
    field(QStringLiteral("最高价格"), max, 6, 1);
    form->setRowMinimumHeight(8, 12);
    field(QStringLiteral("最大磨损"), wear, 9, 0);
    field(QStringLiteral("数量上限"), quantity, 9, 1);
    form->setColumnStretch(0, 1);
    form->setColumnStretch(1, 1);
    body->addLayout(form);
    body->addSpacing(16);
    auto* enableRow = new QHBoxLayout;
    enableRow->addWidget(label(QStringLiteral("启用此任务")));
    enableRow->addStretch();
    enableRow->addWidget(enabled);
    body->addLayout(enableRow);
    body->addSpacing(8);
    auto* validation = label(QString(), QStringLiteral("taskValidationLabel"));
    validation->setWordWrap(true);
    body->addWidget(validation);
    outer->addWidget(content);
    auto* commands = new QFrame;
    commands->setObjectName(QStringLiteral("dialogCommands"));
    auto* commandRow = new QHBoxLayout(commands);
    commandRow->setContentsMargins(24, 20, 24, 20);
    commandRow->setSpacing(8);
    commands->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* save = button(QStringLiteral("保存任务"), QStringLiteral("taskSaveButton"), ButtonKind::Accent);
    save->setDefault(true);
    auto* cancel = button(QStringLiteral("取消"), QStringLiteral("taskCancelButton"));
    commandRow->addWidget(save, 1);
    commandRow->addWidget(cancel, 1);
    outer->addWidget(commands);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(save, &QPushButton::clicked, &dialog, [&] {
        if (name->text().trimmed().isEmpty()) { validation->setText(QStringLiteral("请输入任务名称。")); name->setFocus(); return; }
        if (skin->currentIndex() < 0) { validation->setText(QStringLiteral("请选择目标皮肤。")); return; }
        if (min->value() > max->value()) { validation->setText(QStringLiteral("最低价格需要小于或等于最高价格。")); return; }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) return;
    Task task;
    task.id = editing ? existing.id : QUuid::createUuid().toString(QUuid::WithoutBraces);
    task.name = name->text().trimmed();
    task.skinId = skin->currentData().toString();
    task.minPrice = min->value();
    task.maxPrice = max->value();
    task.maxWear = wear->value();
    task.quantity = quantity->value();
    task.condition = condition->currentText();
    task.enabled = enabled->isChecked();
    task.status = task.enabled ? QStringLiteral("待启动") : QStringLiteral("演示已暂停");
    if (editing) {
        for (auto& target : m_state->tasks) {
            if (target.id == id) { target = task; break; }
        }
        m_state->addLog("INFO", QStringLiteral("更新本地任务：") + task.name);
        m_state->resetTaskSimulation(task.id);
    } else {
        m_state->tasks.append(task);
        m_state->addLog("INFO", QStringLiteral("新增本地任务：") + task.name);
        m_state->notifyChanged();
    }
}

void MainWindow::deleteSelectedTasks()
{
    QStringList ids;
    for (const auto& index : m_tasksTable->selectionModel()->selectedRows())
        if (auto* item = m_tasksTable->item(index.row(), 0)) ids.append(item->data(Qt::UserRole).toString());
    if (ids.isEmpty()) return;
    if (!confirm(this, QStringLiteral("删除任务"), QStringLiteral("删除选中的 %1 条本地任务？关注列表保持不变。").arg(ids.size()), QStringLiteral("删除")))
        return;
    for (int i = m_state->tasks.size() - 1; i >= 0; --i) {
        if (ids.contains(m_state->tasks[i].id)) m_state->tasks.removeAt(i);
    }
    m_state->addLog("INFO", QStringLiteral("删除 %1 条本地任务").arg(ids.size()));
    m_state->notifyChanged();
}

void MainWindow::setSelectedTasksEnabled(bool enabled)
{
    QStringList ids;
    for (const auto& index : m_tasksTable->selectionModel()->selectedRows())
        if (auto* item = m_tasksTable->item(index.row(), 0)) ids.append(item->data(Qt::UserRole).toString());
    if (ids.isEmpty()) return;
    for (auto& task : m_state->tasks) {
        if (ids.contains(task.id)) {
            task.enabled = enabled;
            task.status = enabled ? QStringLiteral("待启动") : QStringLiteral("演示已暂停");
        }
    }
    m_state->addLog("INFO", QStringLiteral("%1 %2 条选中任务").arg(enabled ? QStringLiteral("启用") : QStringLiteral("暂停")).arg(ids.size()));
    m_state->notifyChanged();
}

void MainWindow::importConfiguration()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入只读预览"), QString(),
                                                      QStringLiteral("配置或 13 列文本 (*.json *.txt *.ini);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), file.errorString());
        return;
    }
    previewImportBytes(file.readAll(), QFileInfo(path).fileName());
}

void MainWindow::previewImportBytes(const QByteArray& bytes, const QString& sourceName)
{
    if (bytes.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("导入预览"), QStringLiteral("文件为空，无法生成预览。"));
        return;
    }
    const QByteArray trimmed = bytes.trimmed();
    QJsonObject preview;
    const bool jsonBom = bytes.startsWith("\xEF\xBB\xBF") || bytes.startsWith("\xFF\xFE");
    if (jsonBom || trimmed.startsWith('{')) {
        preview = relink::config::previewV1(bytes);
    } else {
        relink::config::ImportOptions options;
        options.container = sourceName.endsWith(QStringLiteral(".ini"), Qt::CaseInsensitive)
            ? QStringLiteral("ini") : QStringLiteral("text");
        preview = relink::config::preview13Columns(bytes, options);
    }
    showImportPreview(preview, sourceName);
}

void MainWindow::previewImportDemo()
{
    const QJsonObject sample{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("demo"), true},
        {QStringLiteral("source"), QStringLiteral("in-memory PR07 fixture")},
        {QStringLiteral("skins"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("demo-01")},
                        {QStringLiteral("name"), QStringLiteral("AUG 示例")},
                        {QStringLiteral("series"), QStringLiteral("S11")},
                        {QStringLiteral("condition"), QStringLiteral("S")},
                        {QStringLiteral("price"), 650.0}}
        }},
        {QStringLiteral("tasks"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("task-demo-01")},
                        {QStringLiteral("name"), QStringLiteral("示例关注任务")},
                        {QStringLiteral("skinId"), QStringLiteral("demo-01")},
                        {QStringLiteral("minPrice"), 100.0},
                        {QStringLiteral("maxPrice"), 680.0},
                        {QStringLiteral("maxWear"), 5.0},
                        {QStringLiteral("quantity"), 1},
                        {QStringLiteral("enabled"), true},
                        {QStringLiteral("condition"), QStringLiteral("S")}}
        }},
        {QStringLiteral("run_settings"), QJsonObject{{QStringLiteral("purchaseDelayMs"), 830}}}
    };
    previewImportBytes(QJsonDocument(sample).toJson(QJsonDocument::Compact), QStringLiteral("pr07-demo.json"));
}

void MainWindow::showImportPreview(const QJsonObject& preview, const QString& sourceName)
{
    using relink::runtime::UiProjection;
    const auto view = UiProjection::importPreview(preview);
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("importPreviewDialog"));
    dialog->setWindowTitle(QStringLiteral("导入预览 · 只读"));
    dialog->setModal(true);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMinimumSize(760, 520);

    auto* outer = new QVBoxLayout(dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* content = new QFrame;
    content->setObjectName(QStringLiteral("dialogContent"));
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(24, 22, 24, 18);
    body->setSpacing(8);
    body->addWidget(label(QStringLiteral("导入预览"), QStringLiteral("dialogTitle")));
    auto* source = label(QStringLiteral("%1 · %2 · %3").arg(sourceName, view.format, view.encoding), QStringLiteral("importPreviewSource"));
    source->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->addWidget(source);
    auto* hash = label(QStringLiteral("SHA-256  %1").arg(view.sourceHash), QStringLiteral("importPreviewHash"));
    hash->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->addWidget(hash);
    auto* status = label(view.statusText, QStringLiteral("importPreviewStatus"));
    status->setProperty("status", view.errorCount > 0 ? "negative" : (view.reviewRows > 0 ? "caution" : "positive"));
    body->addWidget(status);
    auto* policy = label(QStringLiteral("候选策略：enabled=false · review_required=true · 仅供审阅"), QStringLiteral("importPreviewPolicy"));
    policy->setWordWrap(true);
    body->addWidget(policy);

    auto* table = new QTableWidget;
    table->setObjectName(QStringLiteral("importPreviewTable"));
    table->setColumnCount(5);
    table->setHorizontalHeaderLabels({QStringLiteral("行"), QStringLiteral("状态"), QStringLiteral("候选"), QStringLiteral("数量"), QStringLiteral("诊断")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(false);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    const auto rows = preview.value(QStringLiteral("rows")).toArray();
    table->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const auto row = rows.at(i).toObject();
        const auto candidate = row.value(QStringLiteral("candidate")).toObject();
        const auto extensions = candidate.value(QStringLiteral("extensions")).toObject();
        const auto diagnostics = row.value(QStringLiteral("diagnostics")).toArray();
        QStringList codes;
        for (const auto& item : diagnostics) {
            const QString code = item.toObject().value(QStringLiteral("code")).toString();
            if (!code.isEmpty()) codes.append(code);
        }
        table->setItem(i, 0, new QTableWidgetItem(QString::number(row.value(QStringLiteral("line")).toInt(i + 1))));
        table->setItem(i, 1, new QTableWidgetItem(row.value(QStringLiteral("status")).toString()));
        QString candidateName = candidate.value(QStringLiteral("name")).toString();
        if (candidateName.isEmpty()) candidateName = extensions.value(QStringLiteral("x-product_name")).toString();
        table->setItem(i, 2, new QTableWidgetItem(candidateName));
        const auto quantity = candidate.value(QStringLiteral("quantity_candidate"));
        table->setItem(i, 3, new QTableWidgetItem(quantity.isNull() ? QStringLiteral("—") : quantity.toVariant().toString()));
        auto* diag = new QTableWidgetItem(codes.join(QStringLiteral(" · ")));
        diag->setToolTip(codes.join(QStringLiteral("\n")));
        table->setItem(i, 4, diag);
    }
    body->addWidget(table, 1);
    auto* note = label(view.readOnlyNote, QStringLiteral("importPreviewReadOnlyNote"));
    note->setWordWrap(true);
    body->addWidget(note);
    outer->addWidget(content, 1);

    auto* commands = new QFrame;
    commands->setObjectName(QStringLiteral("dialogCommands"));
    auto* commandRow = new QHBoxLayout(commands);
    commandRow->setContentsMargins(24, 16, 24, 16);
    commandRow->setSpacing(8);
    commandRow->addStretch();
    auto* apply = button(QStringLiteral("应用（暂不可用）"), QStringLiteral("importPreviewApplyButton"));
    apply->setEnabled(false);
    apply->setToolTip(QStringLiteral("PR08 才会提供审定提交；本阶段始终只读"));
    auto* close = button(QStringLiteral("关闭"), QStringLiteral("importPreviewCloseButton"), ButtonKind::Accent);
    close->setDefault(true);
    close->setMinimumWidth(112);
    commandRow->addWidget(apply);
    commandRow->addWidget(close);
    outer->addWidget(commands);
    connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
    dialog->open();
}
void MainWindow::exportConfiguration()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出本地配置"), "relink-config.json", QStringLiteral("JSON 配置 (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    if (!m_state->saveTo(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    m_state->addLog("INFO", QStringLiteral("已导出本地配置"));
    m_state->notifyChanged();
}

void MainWindow::exportPrices()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出演示报价"), "relink-demo-prices.csv", QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty()) return;
    QString error;
    if (!m_state->exportPricesCsv(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    m_state->addLog("INFO", QStringLiteral("已导出演示报价 CSV（非游戏采集数据）"));
    m_state->notifyChanged();
}

void MainWindow::saveSettings()
{
    QSettings preferences;
    preferences.setValue("ui/autoScrollLogs", m_autoScrollLogs->isChecked());
    preferences.setValue("ui/compactTables", m_compactTables->isChecked());
    preferences.sync();
    QString path = m_state->configPath;
    if (path.isEmpty()) path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/config.json";
    QDir().mkpath(QFileInfo(path).absolutePath());
    QString error;
    if (m_state->saveTo(path, &error)) {
        m_settingsMessage->setText(QStringLiteral("设置与本地配置已保存"));
        m_state->configPath = path;
        m_state->addLog("INFO", QStringLiteral("已保存界面设置与本地配置"));
        m_state->notifyChanged();
    } else {
        m_settingsMessage->setText(QStringLiteral("配置保存失败：") + error);
    }
}

void MainWindow::applyDensity()
{
    const bool compact = m_compactTables && m_compactTables->isChecked();
    for (auto* item : findChildren<QTableWidget*>()) {
        int size = compact ? 36 : 44;
        if (item == m_favoritesTable) size = compact ? 52 : 64;
        else if (item == m_tasksTable) size = compact ? 46 : 56;
        item->verticalHeader()->setDefaultSectionSize(size);
    }
    if (m_overviewLogs)
        m_overviewLogs->setFixedHeight(qMax(1, m_overviewLogs->rowCount()) * m_overviewLogs->verticalHeader()->defaultSectionSize() + 2);
}

