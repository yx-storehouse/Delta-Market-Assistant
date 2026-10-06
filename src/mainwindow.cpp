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
const QStringList titles = {QStringLiteral("宸ヤ綔鍙?"), QStringLiteral("鎴戠殑鍏虫敞"), QStringLiteral("鑷姩浠诲姟"),
                            QStringLiteral("浠锋牸涓績"), QStringLiteral("杩愯缁熻"), QStringLiteral("杩愯鏃ュ織"),
                            QStringLiteral("璁剧疆"), QStringLiteral("杩愯璁剧疆")};
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
    if (task.status == QStringLiteral("婕旂ず宸插畬鎴?")) return ToneSuccess;
    if (task.status == QStringLiteral("婕旂ず閰嶇疆鏃犳晥")) return ToneCritical;
    if (task.status == QStringLiteral("婕旂ず宸叉殏鍋?") || task.status == QStringLiteral("婕旂ず绛夊緟鏉′欢")) return ToneCaution;
    if (task.status == QStringLiteral("婕旂ず杩涜涓?")) return ToneAccent;
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

// Store-style "鏌ョ湅鍏ㄩ儴 鈥? link: text followed by a small chevron.
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

// Store-style section title; links such as "鏌ョ湅鍏ㄩ儴" are added on the right.
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
    return title.contains(QStringLiteral("鎶ヤ环")) || title.contains(QStringLiteral("浠锋牸"))
        || title == QStringLiteral("鏁伴噺") || title.contains(QStringLiteral("纾ㄦ崯"));
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
// Mirrors the original "S6|AUG 绐佸嚮姝ユ灙 - 澶╁懡" skin selector: season, then name.
QString skinChoice(const Skin& skin) { return skin.series.section(' ', 0, 0) + QStringLiteral(" | ") + skin.name; }
QStringList conditionOptions(const AppState* state)
{
    QStringList options = {QStringLiteral("涓嶉檺"), QStringLiteral("成色S"), QStringLiteral("成色A"), QStringLiteral("成色B")};
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
    auto* no = button(QStringLiteral("鍙栨秷"), QStringLiteral("confirmCancelButton"));
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
    m_favoriteSearch->setPlaceholderText(QStringLiteral("鎼滅储鐨偆銆佺郴鍒?"));
    m_favoriteSearch->setFixedSize(520, 34);
    auto* searchAction = m_favoriteSearch->addAction(
        FluentTheme::glyphIcon(Glyph::Search, FluentTheme::secondary, FluentTheme::text), QLineEdit::TrailingPosition);
    searchAction->setToolTip(QStringLiteral("鎼滅储鍏虫敞鏉＄洰"));
    m_searchClear = m_favoriteSearch->addAction(
        FluentTheme::glyphIcon(Glyph::Clear, FluentTheme::secondary, FluentTheme::text), QLineEdit::TrailingPosition);
    m_searchClear->setToolTip(QStringLiteral("娓呴櫎"));
    m_searchClear->setVisible(false);
    connect(searchAction, &QAction::triggered, this, [this] { setPage(1); m_favoriteSearch->setFocus(); });
    connect(m_searchClear, &QAction::triggered, m_favoriteSearch, &QLineEdit::clear);
    auto* importButton = button(QStringLiteral("瀵煎叆"), QStringLiteral("importConfigButton"), ButtonKind::Subtle, Glyph::Import);
    auto* exportButton = button(QStringLiteral("瀵煎嚭"), QStringLiteral("exportConfigButton"), ButtonKind::Subtle, Glyph::Export);
    auto* helpButton = button(QString(), QStringLiteral("helpButton"), ButtonKind::Subtle, Glyph::Help);
    helpButton->setToolTip(QStringLiteral("甯姪"));
    helpButton->setAccessibleName(QStringLiteral("甯姪"));
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
        {1, "navFavorites", "鍏虫敞", Glyph::Star, Glyph::StarFill},
        {2, "navTasks", "浠诲姟", Glyph::Tasks, 0},
        {RunPage, "navRun", "杩愯", Glyph::Lightning, 0},
        {3, "navPrices", "浠锋牸", Glyph::Trend, 0},
        {0, "navOverview", "宸ヤ綔鍙?", Glyph::Home, Glyph::HomeFill},
        {4, "navStats", "缁熻", Glyph::Pie, 0},
        {5, "navLogs", "鏃ュ織", Glyph::History, 0},
        {6, "navSettings", "璁剧疆", Glyph::Settings, Glyph::SettingsFill},
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
    m_pauseButton = button(QStringLiteral("鏆傚仠"), QStringLiteral("pauseSimulationButton"), ButtonKind::Standard, Glyph::Pause);
    m_startButton = button(QStringLiteral("寮€濮嬫ā鎷?"), QStringLiteral("startSimulationButton"), ButtonKind::Accent, Glyph::Play);
    header->addWidget(m_pauseButton, 0, Qt::AlignVCenter);
    header->addWidget(m_startButton, 0, Qt::AlignVCenter);
    connect(m_startButton, &QPushButton::clicked, m_state, &AppState::startSimulation);
    connect(m_pauseButton, &QPushButton::clicked, m_state, &AppState::pauseSimulation);
    layout->addSpacing(20);

    auto* strip = card(QStringLiteral("metricStrip"));
    auto* metrics = new QHBoxLayout(strip);
    metrics->setContentsMargins(0, 8, 0, 8);
    metrics->setSpacing(0);
    const QStringList names = {QStringLiteral("鎴戠殑鍏虫敞"), QStringLiteral("鍚敤浠诲姟"), QStringLiteral("鎵弿鏉＄洰"), QStringLiteral("妯℃嫙纭")};
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
    auto* pricesLink = link(QStringLiteral("鏌ョ湅鍏ㄩ儴浠锋牸"), QStringLiteral("overviewPricesLink"));
    sectionHeader(chartColumn, QStringLiteral("浠锋牸瑙傚療"))->addWidget(pricesLink);
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
    auto* taskHeader = sectionHeader(taskColumn, QStringLiteral("浠诲姟闃熷垪"));
    m_overviewTaskCount = label(QString(), QStringLiteral("tertiaryLabel"));
    taskHeader->insertWidget(1, m_overviewTaskCount, 0, Qt::AlignVCenter);
    auto* tasksLink = link(QStringLiteral("绠＄悊浠诲姟"), QStringLiteral("overviewTasksLink"));
    taskHeader->addWidget(tasksLink);
    auto* taskPanel = card(QStringLiteral("overviewTaskPanel"));
    auto* taskLayout = new QVBoxLayout(taskPanel);
    taskLayout->setContentsMargins(12, 6, 12, 8);
    m_overviewTasks = table({QStringLiteral("浠诲姟"), QStringLiteral("鏁伴噺"), QStringLiteral("鐘舵€?")},
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

    auto* logsLink = link(QStringLiteral("鍏ㄩ儴鏃ュ織"), QStringLiteral("overviewLogsLink"));
    sectionHeader(layout, QStringLiteral("鏈€杩戞椿鍔?"))->addWidget(logsLink);
    auto* logPanel = card(QStringLiteral("overviewLogPanel"));
    auto* logLayout = new QVBoxLayout(logPanel);
    logLayout->setContentsMargins(12, 4, 12, 4);
    m_overviewLogs = table({QStringLiteral("鏃堕棿"), QStringLiteral("绾у埆"), QStringLiteral("浜嬩欢")},
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
    addFilter(QStringLiteral("鍏ㄩ儴"), QStringLiteral("all"));
    addFilter(QStringLiteral("宸插叧娉?"), QStringLiteral("followed"));
    addFilter(QStringLiteral("鏈叧娉?"), QStringLiteral("unfollowed"));
    filters->addWidget(rule(QStringLiteral("filterDivider")));
    addFilter(QStringLiteral("姝ユ灙"), QStringLiteral("rifle"));
    addFilter(QStringLiteral("鍐查攱鏋?"), QStringLiteral("smg"));
    addFilter(QStringLiteral("鐙欏嚮鏋?"), QStringLiteral("sniper"));
    addFilter(QStringLiteral("杞绘満鏋?"), QStringLiteral("lmg"));
    filters->addStretch();
    m_rarityFilter = new FluentComboBox;
    m_rarityFilter->setObjectName(QStringLiteral("favoriteRarity"));
    m_rarityFilter->addItem(QStringLiteral("鍏ㄩ儴鍝佺骇"));
    m_rarityFilter->setFixedWidth(132);
    m_favoriteStateFilter = new FluentComboBox;
    m_favoriteStateFilter->setObjectName(QStringLiteral("favoriteTaskFilter"));
    m_favoriteStateFilter->addItems({QStringLiteral("鍏ㄩ儴浠诲姟"), QStringLiteral("宸插叧鑱?"), QStringLiteral("鏈叧鑱?")});
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
    m_favoritesTable = table({QStringLiteral("鍏虫敞"), QStringLiteral("鐗╁搧"), QStringLiteral("纾ㄦ崯"), QStringLiteral("婕旂ず鎶ヤ环"),
                              QStringLiteral("鏍锋湰瓒嬪娍"), QStringLiteral("鐩爣浠锋牸"), QStringLiteral("浠诲姟鐘舵€?")},
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
    chartHeader->addWidget(label(QStringLiteral("浠锋牸璧板娍"), QStringLiteral("cardTitle")));
    m_favoriteChartTitle = label(QString(), QStringLiteral("cardCaption"));
    chartHeader->addWidget(m_favoriteChartTitle);
    chartHeader->addStretch();
    chartHeader->addWidget(label(QStringLiteral("妯℃嫙鏁版嵁"), QStringLiteral("tertiaryLabel")));
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
    m_favoriteArt->setToolTip(QStringLiteral("鐢熸垚鐨勬紨绀烘彃鍥撅紝涓嶄唬琛ㄦ父鎴忎腑鐨勭湡瀹炵毊鑲ゅ瑙傘€?"));
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
    detail->addWidget(label(QStringLiteral("婕旂ず鎶ヤ环 路 闈炲疄鏃惰鎯?"), QStringLiteral("tertiaryLabel")));
    detail->addSpacing(16);
    auto* facts = new QHBoxLayout;
    facts->setSpacing(12);
    const auto addFact = [&](const QString& caption, QLabel*& value) {
        if (facts->count() > 0) facts->addWidget(rule(QStringLiteral("factDivider")));
        auto* block = new QVBoxLayout;
        block->setSpacing(2);
        block->addWidget(label(caption, QStringLiteral("factCaption")));
        value = label(QStringLiteral("鈥?"), QStringLiteral("factValue"));
        block->addWidget(value);
        facts->addLayout(block, 1);
    };
    addFact(QStringLiteral("鍝佺骇"), m_factRarity);
    addFact(QStringLiteral("成色"), m_factCondition);
    addFact(QStringLiteral("纾ㄦ崯"), m_factWear);
    detail->addLayout(facts);
    detail->addSpacing(16);
    detail->addWidget(rule(QStringLiteral("inspectorRule")));
    detail->addSpacing(16);
    detail->addWidget(label(QStringLiteral("浠诲姟鏉′欢"), QStringLiteral("cardTitle")));
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
    fields->addWidget(label(QStringLiteral("鏈€楂樹环鏍?"), QStringLiteral("fieldLabel")), 0, 0);
    fields->addWidget(label(QStringLiteral("成色"), QStringLiteral("fieldLabel")), 0, 1);
    fields->addWidget(m_inspectorPrice, 1, 0);
    fields->addWidget(m_inspectorCondition, 1, 1);
    fields->setRowMinimumHeight(2, 8);
    fields->addWidget(label(QStringLiteral("鏈€澶х（鎹?"), QStringLiteral("fieldLabel")), 3, 0);
    fields->addWidget(label(QStringLiteral("鏁伴噺涓婇檺"), QStringLiteral("fieldLabel")), 3, 1);
    fields->addWidget(m_inspectorWear, 4, 0);
    fields->addWidget(m_inspectorQuantity, 4, 1);
    fields->setColumnStretch(0, 3);
    fields->setColumnStretch(1, 2);
    detail->addLayout(fields);
    detail->addSpacing(16);
    m_favoriteCreate = button(QStringLiteral("鍒涘缓浠诲姟"), QStringLiteral("favoriteCreateTaskButton"), ButtonKind::Accent, Glyph::Add);
    m_favoriteCreate->setFixedHeight(36);
    detail->addWidget(m_favoriteCreate);
    detail->addSpacing(8);
    auto* taskRow = new QHBoxLayout;
    m_favoriteTaskInfo = label(QString(), QStringLiteral("inspectorTaskInfo"));
    taskRow->addWidget(m_favoriteTaskInfo);
    taskRow->addStretch();
    auto* manage = link(QStringLiteral("鏌ョ湅浠诲姟"), QStringLiteral("favoritesTasksButton"));
    taskRow->addWidget(manage);
    detail->addLayout(taskRow);
    detail->addStretch();
    detail->addSpacing(12);
    auto* note = label(QStringLiteral("鏈湴妯℃嫙妯″紡锛氭彃鍥句笌浠锋牸鍧囦负婕旂ず绱犳潗锛屽垱寤轰换鍔″彧淇濆瓨绛涢€夋潯浠躲€?"), QStringLiteral("inspectorNote"));
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
    auto* add = button(QStringLiteral("鏂板浠诲姟"), QStringLiteral("taskNewButton"), ButtonKind::Accent, Glyph::Add);
    auto* edit = button(QStringLiteral("缂栬緫"), QStringLiteral("taskEditButton"), ButtonKind::Subtle, Glyph::Edit);
    auto* remove = button(QStringLiteral("鍒犻櫎"), QStringLiteral("taskDeleteButton"), ButtonKind::Subtle, Glyph::Delete);
    auto* enable = button(QStringLiteral("鍚敤閫変腑"), QStringLiteral("enableSelectedTasksButton"), ButtonKind::Subtle, Glyph::CheckMark);
    auto* disable = button(QStringLiteral("鏆傚仠閫変腑"), QStringLiteral("pauseSelectedTasksButton"), ButtonKind::Subtle, Glyph::Pause);
    m_taskStart = button(QStringLiteral("杩愯妯℃嫙"), QStringLiteral("tasksSimulationButton"), ButtonKind::Standard, Glyph::Play);
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
    auto* note = label(QStringLiteral("Ctrl / Shift 澶氶€?路 鍙屽嚮缂栬緫 路 褰撳墠浠呮ā鎷熻繍琛岋紝涓嶆墽琛岃喘涔?"), QStringLiteral("tertiaryLabel"));
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
    header->addWidget(label(QStringLiteral("鍏虫敞鐩爣"), QStringLiteral("cardCaption")), 0, Qt::AlignVCenter);
    m_priceSkin = new FluentComboBox;
    m_priceSkin->setObjectName(QStringLiteral("pricesSkinCombo"));
    m_priceSkin->setMinimumWidth(260);
    header->addWidget(m_priceSkin, 0, Qt::AlignVCenter);
    auto* exportButton = button(QStringLiteral("瀵煎嚭 CSV"), QStringLiteral("exportPricesButton"), ButtonKind::Standard, Glyph::Download);
    header->addWidget(exportButton, 0, Qt::AlignVCenter);
    layout->addSpacing(20);

    auto* strip = card(QStringLiteral("metricStrip"));
    auto* metrics = new QHBoxLayout(strip);
    metrics->setContentsMargins(0, 8, 0, 8);
    metrics->setSpacing(0);
    const QStringList names = {QStringLiteral("褰撳墠婕旂ず鎶ヤ环"), QStringLiteral("鏍锋湰鏈€浣庢姤浠?"), QStringLiteral("鏍锋湰鏈€楂樻姤浠?")};
    for (int i = 0; i < names.size(); ++i) {
        auto* metric = new MetricCard(names[i], QStringLiteral("鈥?"), QStringLiteral("婕旂ず浠锋牸 路 闈炲疄鏃跺競鍦烘暟鎹?"));
        metric->setObjectName(QStringLiteral("priceMetric%1").arg(i));
        metric->setProperty("separator", i < names.size() - 1);
        m_priceCards.append(metric);
        metrics->addWidget(metric, 1);
    }
    layout->addWidget(strip);
    layout->addSpacing(28);

    sectionHeader(layout, QStringLiteral("浠锋牸璧板娍"))->addWidget(label(QStringLiteral("24 涓牱鏈?路 婕旂ず鏁版嵁"), QStringLiteral("tertiaryLabel")));
    auto* chartPanel = card(QStringLiteral("priceChartPanel"));
    auto* chartLayout = new QVBoxLayout(chartPanel);
    chartLayout->setContentsMargins(20, 16, 20, 12);
    m_priceChart = new PriceChart;
    m_priceChart->setObjectName(QStringLiteral("priceHistoryChart"));
    m_priceChart->setMinimumHeight(220);
    chartLayout->addWidget(m_priceChart, 1);
    layout->addWidget(chartPanel, 3);
    layout->addSpacing(28);

    sectionHeader(layout, QStringLiteral("鏍锋湰璁板綍"))->addWidget(label(QStringLiteral("鏈湴鍚堟垚鏁版嵁"), QStringLiteral("tertiaryLabel")));
    auto* historyPanel = card(QStringLiteral("priceHistoryPanel"));
    auto* historyLayout = new QVBoxLayout(historyPanel);
    historyLayout->setContentsMargins(12, 6, 12, 8);
    m_priceHistory = table({QStringLiteral("鏍锋湰"), QStringLiteral("鍟嗗搧"), QStringLiteral("婕旂ず浠锋牸"), QStringLiteral("绫诲瀷"), QStringLiteral("鏁版嵁鏉ユ簮")},
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
    const QStringList names = {QStringLiteral("妯℃嫙鎵弿"), QStringLiteral("妯℃嫙鏉′欢鍛戒腑"), QStringLiteral("妯℃嫙纭鎴愬姛"), QStringLiteral("瀹為檯鎴愪氦 / 瀹為檯鏀嚭")};
    const QStringList details = {QStringLiteral("鏈婕旂ず鎵弿鏉＄洰"), QStringLiteral("妯℃嫙绛涢€夌鍚堟潯浠?"), QStringLiteral("鍙鍏ユā鎷熺‘璁ょ粨鏋?"), QStringLiteral("鏈繛鎺ユ墽琛屾ā鍧?")};
    for (int i = 0; i < names.size(); ++i) {
        auto* metric = new MetricCard(names[i], i == 3 ? QStringLiteral("鈥?") : QStringLiteral("0"), details[i]);
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
    sectionHeader(left, QStringLiteral("妯℃嫙缁撴灉鍒嗗竷"))->addWidget(label(QStringLiteral("鍩轰簬鏈婕旂ず璁℃暟"), QStringLiteral("tertiaryLabel")));
    auto* distribution = card(QStringLiteral("distributionPanel"));
    auto* distributionLayout = new QVBoxLayout(distribution);
    distributionLayout->setContentsMargins(24, 22, 24, 20);
    distributionLayout->setSpacing(0);
    const QStringList reasons = {QStringLiteral("鏈懡涓瓫閫夋潯浠?"), QStringLiteral("宸插懡涓?路 灏氭湭妯℃嫙纭"), QStringLiteral("妯℃嫙纭鎴愬姛")};
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
    sectionHeader(right, QStringLiteral("缁熻鍙ｅ緞"));
    auto* semantics = card(QStringLiteral("semanticsPanel"));
    auto* semanticsLayout = new QVBoxLayout(semantics);
    semanticsLayout->setContentsMargins(24, 22, 24, 20);
    semanticsLayout->setSpacing(0);
    const QVector<QPair<QString, QString>> explanations = {
        {QStringLiteral("鎵弿 鈮?鍛戒腑"), QStringLiteral("鎵弿璁板綍瑙傚療娆℃暟锛涘懡涓褰曠鍚堜换鍔℃潯浠剁殑娆℃暟銆?")},
        {QStringLiteral("灏濊瘯 鈮?鎴愪氦"), QStringLiteral("褰撳墠鍙湁鏈湴妯℃嫙锛屼笉浜х敓瀹為檯浜ゆ槗鍜屽疄闄呮敮鍑恒€?")},
        {QStringLiteral("鏈喅缁撴灉鍗曠嫭璁?"), QStringLiteral("鎺ュ叆鎵ц妯″潡鍚庯紝缁撴灉鏈槑鐨勫姩浣滀笉璁″叆鎴愬姛銆傚綋鍓嶆紨绀烘湭鍐充负 0銆?")}};
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
    m_logSearch->setPlaceholderText(QStringLiteral("鎼滅储浜嬩欢鍐呭"));
    m_logSearch->setFixedWidth(280);
    m_logSearch->addAction(FluentTheme::glyphIcon(Glyph::Search, FluentTheme::secondary, FluentTheme::text), QLineEdit::TrailingPosition);
    auto* clearSearch = m_logSearch->addAction(FluentTheme::glyphIcon(Glyph::Clear, FluentTheme::secondary, FluentTheme::text), QLineEdit::TrailingPosition);
    clearSearch->setVisible(false);
    connect(clearSearch, &QAction::triggered, m_logSearch, &QLineEdit::clear);
    connect(m_logSearch, &QLineEdit::textChanged, clearSearch, [clearSearch](const QString& text) { clearSearch->setVisible(!text.isEmpty()); });
    header->addWidget(m_logSearch, 0, Qt::AlignVCenter);
    m_logLevel = new FluentComboBox;
    m_logLevel->setObjectName(QStringLiteral("logLevel"));
    m_logLevel->addItems({QStringLiteral("鍏ㄩ儴绾у埆"), "INFO", "DEMO", "SUCCESS", "WARN", "ERROR"});
    m_logLevel->setFixedWidth(140);
    header->addWidget(m_logLevel, 0, Qt::AlignVCenter);
    auto* clear = button(QStringLiteral("娓呯┖鏃ュ織"), QStringLiteral("clearLogsButton"), ButtonKind::Standard, Glyph::Delete);
    header->addWidget(clear, 0, Qt::AlignVCenter);
    layout->addSpacing(20);
    auto* box = card(QStringLiteral("logPanel"));
    auto* boxLayout = new QVBoxLayout(box);
    boxLayout->setContentsMargins(12, 6, 12, 8);
    m_logsTable = table({QStringLiteral("鏃堕棿"), QStringLiteral("绾у埆"), QStringLiteral("鍐呭")}, QStringLiteral("logsTable"), RowStyle::Lines);
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
        if (confirm(this, QStringLiteral("娓呯┖鏈湴鏃ュ織"), QStringLiteral("鍙竻绌哄綋鍓嶆湰鍦版紨绀烘棩蹇楋紝浠诲姟涓庨厤缃笉鍙樸€?"), QStringLiteral("娓呯┖"))) {
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
    group(QStringLiteral("鏄剧ず"), true);
    auto* compact = new ToggleSwitch;
    compact->setObjectName(QStringLiteral("compactTablesCheck"));
    compact->setStateTextVisible(true);
    m_compactTables = compact;
    row(settingsCard(glyphLabel(Glyph::List), QStringLiteral("绱у噾鍒楄〃"), QStringLiteral("闄嶄綆鍒楄〃琛岄珮锛屼竴灞忔樉绀烘洿澶氭潯鐩?"), compact));
    auto* autoScroll = new ToggleSwitch;
    autoScroll->setObjectName(QStringLiteral("autoScrollLogsCheck"));
    autoScroll->setStateTextVisible(true);
    m_autoScrollLogs = autoScroll;
    row(settingsCard(glyphLabel(Glyph::History), QStringLiteral("鏃ュ織鑷姩瀹氫綅"), QStringLiteral("鏂颁簨浠跺嚭鐜版椂锛屾棩蹇楀垪琛ㄥ畾浣嶅埌鏈€鏂颁竴鏉?"), autoScroll));

    group(QStringLiteral("鏈湴閰嶇疆"));
    m_configDirectory = new QLineEdit;
    m_configDirectory->setObjectName(QStringLiteral("configDirectoryEdit"));
    m_configDirectory->setReadOnly(true);
    m_configDirectory->setFixedWidth(460);
    row(settingsCard(glyphLabel(Glyph::Folder), QStringLiteral("閰嶇疆鏂囦欢"), QStringLiteral("鍏虫敞銆佷换鍔′笌杩愯鍙傛暟淇濆瓨鍦ㄦ鏂囦欢锛涢€€鍑虹▼搴忔椂鑷姩淇濆瓨"), m_configDirectory));
    layout->addSpacing(8);
    auto* actionRow = new QHBoxLayout;
    m_settingsMessage = label(QString(), QStringLiteral("settingsMessage"));
    m_settingsMessage->setWordWrap(true);
    actionRow->addWidget(m_settingsMessage, 1);
    auto* save = button(QStringLiteral("淇濆瓨鏈湴璁剧疆"), QStringLiteral("saveSettingsButton"), ButtonKind::Accent, Glyph::Save);
    actionRow->addWidget(save);
    layout->addLayout(actionRow);

    group(QStringLiteral("杩愯妯″紡"));
    row(settingsCard(glyphLabel(Glyph::Pulse), QStringLiteral("鍓嶇鐙珛婕旂ず"),
                     QStringLiteral("浣跨敤鏈湴婕旂ず鏁版嵁楠岃瘉鍏虫敞绠＄悊銆佷换鍔￠厤缃€佷环鏍煎睍绀轰笌缁熻浜や簰锛涙病鏈夎繛鎺ユ父鎴忚繘绋嬶紝涓嶈鍙栧疄鏃跺競鍦猴紝涔熶笉鎵ц瀹為檯璐拱銆?"),
                     label(QStringLiteral("鏈湴棰勮"), QStringLiteral("settingsBadge")), QStringLiteral("runModeCard")));
    group(QStringLiteral("鏁版嵁杩炴帴"));
    row(settingsCard(glyphLabel(Glyph::Link), QStringLiteral("娓告垙椤甸潰閲囬泦 路 浠锋牸璇嗗埆涓庢牎楠?路 鑷姩鎿嶄綔涓庣粨鏋滅‘璁?"),
                     QStringLiteral("鐣岄潰灞備笌杩欎簺妯″潡淇濇寔鍒嗙锛涘墠绔畬鎴愬悗鍙互閫愪釜鎺ュ叆銆?"),
                     label(QStringLiteral("灏氭湭鎺ュ叆"), QStringLiteral("settingsBadge"))));
    group(QStringLiteral("鍏充簬"));
    row(settingsCard(new AppMark(24), QStringLiteral("Relink Studio"),
                     QStringLiteral("鐗堟湰 %1 路 鏈湴婕旂ず鍓嶇 路 C++17 / Qt %2 Widgets").arg(QCoreApplication::applicationVersion(), QStringLiteral(QT_VERSION_STR)),
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
    auto* save = button(QStringLiteral("淇濆瓨鍙傛暟"), QStringLiteral("saveRunSettingsButton"), ButtonKind::Accent, Glyph::Save);
    header->addWidget(runMessage, 0, Qt::AlignVCenter);
    header->addSpacing(8);
    header->addWidget(save, 0, Qt::AlignVCenter);
    m_runBinders.append([this, profileCaption] { profileCaption->setText(QStringLiteral("鏂规 %1").arg(m_state->run.profile)); });
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

    group(QStringLiteral("杩愯"), true);
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
    row(settingsCard(glyphLabel(Glyph::Tag), QStringLiteral("閰嶇疆鏂规"), QStringLiteral("瀵瑰簲鍘熺▼搴忛《閮ㄧ殑鏂规鏍囩锛涘鍑恒€佸鍏ユ椂闅忓弬鏁颁竴璧蜂繚瀛?"), profile));
    auto* hotkey = new FluentComboBox;
    hotkey->setObjectName(QStringLiteral("runHotkeyCombo"));
    for (int key = 1; key <= 12; ++key) hotkey->addItem(QStringLiteral("F%1").arg(key));
    hotkey->setFixedWidth(104);
    connect(hotkey, &QComboBox::currentTextChanged, this, [this](const QString& key) { if (!key.isEmpty()) m_state->run.hotkey = key; });
    m_runBinders.append([this, hotkey] { QSignalBlocker guard(hotkey); hotkey->setCurrentText(m_state->run.hotkey); });
    row(settingsCard(glyphLabel(Glyph::Keyboard), QStringLiteral("杩愯蹇嵎閿?"), QStringLiteral("鎸変笅鍚庡紑濮嬫垨鍋滄杩愯锛涙帴鍏ユ墽琛屾ā鍧楀悗鐢熸晥"), hotkey));
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
    row(settingsCard(glyphLabel(Glyph::Clock), QStringLiteral("瀹氭椂杩愯"), QStringLiteral("鍦ㄨ瀹氭椂闂磋嚜鍔ㄥ紑濮嬪拰鍋滄锛涙帴鍏ユ墽琛屾ā鍧楀悗鐢熸晥"),
                     strip({text(QStringLiteral("寮€濮?")), scheduleStart, text(QStringLiteral("缁撴潫")), scheduleStop, schedule})));

    group(QStringLiteral("璐拱寤惰繜"));
    row(settingsCard(glyphLabel(Glyph::Stopwatch), QStringLiteral("璐拱寤惰繜"), QStringLiteral("姣忔璐拱鎿嶄綔涔嬮棿鐨勭瓑寰呮椂闂?"),
                     integer(QStringLiteral("runPurchaseDelay"), &RunSettings::purchaseDelayMs, 0, 60000, QStringLiteral(" ms"), 140)));
    row(settingsCard(nullptr, QStringLiteral("鍔ㄦ€佸欢杩?"), QStringLiteral("寮€鍚姩鎬佸欢杩熻皟鏁达紙璺宠繃鎶藉椤碉細寮€鍚級銆傚師绋嬪簭鏍囨敞涓轰笉寤鸿"),
                     toggle(QStringLiteral("runDynamicDelay"), &RunSettings::dynamicDelay), QString(), true));
    row(settingsCard(nullptr, QStringLiteral("闃熷垪宸叉弧鏃跺噺寤惰繜"), QStringLiteral("姣忚Е鍙戣瀹氭鏁帮紝璐拱寤惰繜鍑忓皯涓€娆?"),
                     strip({text(QStringLiteral("瑙﹀彂")), integer(QStringLiteral("runQueueTrigger"), &RunSettings::queueFullTrigger, 1, 9999, QStringLiteral(" 娆?"), 104),
                            text(QStringLiteral("鍑忓皯")), step(QStringLiteral("runQueueStep"), &RunSettings::queueFullStepMs)}), QString(), true));
    row(settingsCard(nullptr, QStringLiteral("鍏ず鏈熷姞寤惰繜"), QStringLiteral("澶勪簬鍏ず鏈熸椂锛屾瘡瑙﹀彂璁惧畾娆℃暟锛岃喘涔板欢杩熷鍔犱竴娆?"),
                     strip({text(QStringLiteral("瑙﹀彂")), integer(QStringLiteral("runPublicityTrigger"), &RunSettings::publicityTrigger, 1, 9999, QStringLiteral(" 娆?"), 104),
                            text(QStringLiteral("澧炲姞")), step(QStringLiteral("runPublicityStep"), &RunSettings::publicityStepMs)}), QString(), true));

    group(QStringLiteral("杩炵偣妯″紡"));
    auto* burst = toggle(QStringLiteral("runBurstClick"), &RunSettings::burstClick);
    auto* interval = integer(QStringLiteral("runClickInterval"), &RunSettings::clickIntervalMs, 1, 10000, QStringLiteral(" ms"), 140);
    connect(burst, &QCheckBox::toggled, interval, &QWidget::setEnabled);
    m_runBinders.append([this, interval] { interval->setEnabled(m_state->run.burstClick); });
    row(settingsCard(glyphLabel(Glyph::Mouse), QStringLiteral("杩炵画鐐瑰嚮"), QStringLiteral("寮€鍚繛鐐规ā寮忥紝鎸夎瀹氶棿闅旇繛缁偣鍑昏喘涔?"), burst));
    row(settingsCard(nullptr, QStringLiteral("鐐瑰嚮闂撮殧"), QStringLiteral("涓ゆ鐐瑰嚮涔嬮棿鐨勯棿闅?"), interval, QString(), true));

    group(QStringLiteral("璐拱閲忛檺鍒?"));
    row(settingsCard(glyphLabel(Glyph::Filter), QStringLiteral("鎸夊搧绾ч檺鍒惰喘涔版暟閲?"), QStringLiteral("姗欒壊銆佺传鑹层€佽摑鑹插搧绾у垎鍒鏁?"),
                     strip({text(QStringLiteral("姗欒壊")), integer(QStringLiteral("runLimitOrange"), &RunSettings::limitOrange, 0, 9999, QString(), 96),
                            text(QStringLiteral("绱壊")), integer(QStringLiteral("runLimitPurple"), &RunSettings::limitPurple, 0, 9999, QString(), 96),
                            text(QStringLiteral("钃濊壊")), integer(QStringLiteral("runLimitBlue"), &RunSettings::limitBlue, 0, 9999, QString(), 96)})));

    group(QStringLiteral("鎸傛満璁剧疆"));
    row(settingsCard(glyphLabel(Glyph::Sync), QStringLiteral("鍒锋柊鐣岄潰"), QStringLiteral("鎸傛満鏃跺畾鏃跺埛鏂板競鍦虹晫闈€傚師绋嬪簭鏍囨敞涓轰笉鎺ㄨ崘"),
                     toggle(QStringLiteral("runRefreshPage"), &RunSettings::refreshPage)));
    row(settingsCard(glyphLabel(Glyph::SkipTo), QStringLiteral("璺宠繃鎶藉椤?"), QStringLiteral("鎸傛満鏃惰嚜鍔ㄨ烦杩囨娊濂栭〉"),
                     toggle(QStringLiteral("runSkipLottery"), &RunSettings::skipLotteryPage)));
    row(settingsCard(glyphLabel(Glyph::SkipTo), QStringLiteral("璺宠繃鎴愬姛椤?"), QStringLiteral("璐拱鎴愬姛鍚庤嚜鍔ㄨ烦杩囩粨鏋滈〉"),
                     toggle(QStringLiteral("runSkipSuccess"), &RunSettings::skipSuccessPage)));

    group(QStringLiteral("鑷姩鏀惰棌"));
    row(settingsCard(glyphLabel(Glyph::Star), QStringLiteral("鑷姩鏀惰棌妯″紡"), QStringLiteral("鎸夋敹钘忎换鍔￠厤缃嚜鍔ㄦ敹钘忕鍚堟潯浠剁殑鏉＄洰"),
                     toggle(QStringLiteral("runAutoCollect"), &RunSettings::autoCollect)));
    row(settingsCard(glyphLabel(Glyph::Display), QStringLiteral("鏀惰棌鐘舵€?OSD"), QStringLiteral("鍦ㄦ父鎴忕敾闈笂鏄剧ず鏀惰棌鐘舵€佹诞灞?"),
                     toggle(QStringLiteral("runCollectOsd"), &RunSettings::collectOsd)));
    m_runTaskSummary = label(QString(), QStringLiteral("cardCaption"));
    auto* manageTasks = link(QStringLiteral("绠＄悊浠诲姟"), QStringLiteral("runManageTasksLink"));
    connect(manageTasks, &QPushButton::clicked, this, [this] { setPage(2); });
    row(settingsCard(glyphLabel(Glyph::Tasks), QStringLiteral("鏀惰棌浠诲姟閰嶇疆"), QStringLiteral("鐨偆銆佹垚鑹层€佹渶澶х（鎹熴€佷环鏍煎尯闂翠笌闄愬埗閲?"),
                     strip({m_runTaskSummary, manageTasks})));

    group(QStringLiteral("浼樺寲涓庤褰?"));
    row(settingsCard(glyphLabel(Glyph::Speed), QStringLiteral("浼樺寲"), QStringLiteral("鍘熺▼搴忛《閮ㄧ殑銆屼紭鍖栥€嶅叆鍙ｏ紱鍏蜂綋浼樺寲椤圭洰寰呯‘璁ゅ悗鎺ュ叆"),
                     label(QStringLiteral("寰呯‘璁?"), QStringLiteral("settingsBadge"))));
    auto* records = link(QStringLiteral("鏌ョ湅鏃ュ織"), QStringLiteral("runRecordsLink"));
    connect(records, &QPushButton::clicked, this, [this] { setPage(5); });
    row(settingsCard(glyphLabel(Glyph::History), QStringLiteral("杩愯璁板綍"), QStringLiteral("鏃ュ織涓庣粺璁￠〉璁板綍姣忔杩愯鐨勪簨浠跺拰缁撴灉"), records));
    auto* help = link(QStringLiteral("鎵撳紑甯姪"), QStringLiteral("runHelpLink"));
    connect(help, &QPushButton::clicked, this, &MainWindow::showHelp);
    row(settingsCard(glyphLabel(Glyph::Help), QStringLiteral("甯姪"), QStringLiteral("浣跨敤娴佺▼銆佸揩鎹烽敭涓庡綋鍓嶇増鏈鏄?"), help));
    layout->addSpacing(16);
    auto* note = label(QStringLiteral("浠ヤ笂鍙傛暟闅忛厤缃竴璧蜂繚瀛樸€佸鍑哄拰瀵煎叆銆傚綋鍓嶇増鏈笉杩炴帴娓告垙锛屼笉浼氭墽琛岀偣鍑绘垨璐拱銆?"), QStringLiteral("tertiaryLabel"));
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
    m_statusBadge->setText(QStringLiteral("<span style=\"color:%1;\">鈼?/span>&nbsp;&nbsp;%2")
                               .arg((running ? FluentTheme::positive : FluentTheme::muted).name(),
                                    running ? QStringLiteral("妯℃嫙杩愯涓?") : QStringLiteral("鏈湴婕旂ず")));
    m_statusBadge->setToolTip(running ? QStringLiteral("鏈湴妯℃嫙姝ｅ湪鎺ㄨ繘锛涗笉杩炴帴娓告垙锛屼笉浜х敓璁㈠崟銆?")
                                      : QStringLiteral("婕旂ず鏁版嵁 路 鏈繛鎺ユ父鎴?"));
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
        m_runTaskSummary->setText(QStringLiteral("%1 鏉′换鍔?路 %2 鏉″惎鐢?").arg(m_state->tasks.size()).arg(enabled));
}

void MainWindow::refreshOverview()
{
    int followed = 0, enabled = 0;
    for (const auto& skin : m_state->skins) if (skin.followed) ++followed;
    for (const auto& task : m_state->tasks) if (task.enabled) ++enabled;
    m_overviewCards[0]->setValue(QString::number(followed), QStringLiteral("鏈湴鍏虫敞鏉＄洰"));
    m_overviewCards[1]->setValue(QString::number(enabled), QStringLiteral("鍏?%1 鏉′换鍔?").arg(m_state->tasks.size()));
    m_overviewCards[2]->setValue(QString::number(m_state->simulatedScans), QStringLiteral("鏈妯℃嫙绱"));
    m_overviewCards[3]->setValue(QString::number(m_state->simulatedSuccess), QStringLiteral("浠呮ā鎷熺粨鏋?"));
    const double reference = m_state->skins.isEmpty() ? 100 : m_state->skins.first().price;
    m_overviewChart->setSeries(series(reference));
    m_overviewChart->setCaption(m_state->skins.isEmpty() ? QStringLiteral("婕旂ず浠锋牸") : m_state->skins.first().name);
    m_overviewTaskCount->setText(QStringLiteral("%1 鏉′换鍔?").arg(m_state->tasks.size()));
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
        m_rarityFilter->addItem(QStringLiteral("鍏ㄩ儴鍝佺骇"));
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
        star->setToolTip(skin.followed ? QStringLiteral("鍙栨秷鍏虫敞") : QStringLiteral("鍏虫敞"));
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
        identity->setToolTip(skin.name + "\n" + skin.series + QStringLiteral(" 路 婕旂ず鎻掑浘"));
        put(m_favoritesTable, row, 2, QString::number(skin.wear, 'f', 3), FluentTheme::secondary);
        put(m_favoritesTable, row, 3, amount(skin.price));
        QVariantList samples;
        for (double value : series(skin.price)) samples.append(value);
        auto* trend = put(m_favoritesTable, row, 4, QString());
        trend->setData(KindRole, SparkCell);
        trend->setData(SamplesRole, samples);
        put(m_favoritesTable, row, 5, linked ? amount(linked->maxPrice) : QStringLiteral("鈥?"), linked ? FluentTheme::text : FluentTheme::muted);
        putStatus(m_favoritesTable, row, 6,
                  linked ? (enabled ? QStringLiteral("宸插惎鐢?") : QStringLiteral("宸叉殏鍋?")) : QStringLiteral("鏈厤缃?"),
                  linked ? (enabled ? ToneSuccess : ToneCaution) : ToneInactive);
    }
    if (m_favoritesTable->rowCount() > 0) m_favoritesTable->selectRow(selected < 0 ? 0 : selected);
    m_favoritesCount->setText(QStringLiteral("%1 涓潯鐩?").arg(m_favoritesTable->rowCount()));
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
        m_favoriteTitle->setText(QStringLiteral("娌℃湁鍖归厤鏉＄洰"));
        m_favoriteMetadata->setText(QStringLiteral("璋冩暣鎼滅储鎴栫瓫閫夋潯浠?"));
        m_favoritePrice->setText(QStringLiteral("鈥?"));
        m_favoriteChange->clear();
        m_favoriteArt->setArtwork(QPixmap());
        for (auto* fact : {m_factRarity, m_factCondition, m_factWear}) fact->setText(QStringLiteral("鈥?"));
        m_favoriteTaskInfo->clear();
        m_favoriteChart->setSeries({});
        m_favoriteChartTitle->clear();
        return;
    }
    m_favoriteTitle->setText(skin->name);
    m_favoriteMetadata->setText(skin->series + QStringLiteral(" 路 婕旂ず鎻掑浘"));
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
    m_favoriteTaskInfo->setText(QStringLiteral("宸插叧鑱?%1 鏉′换鍔?").arg(tasks));
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
    task.name = skin->name.left(45) + QStringLiteral(" 路 浠锋牸鍏虫敞");
    task.skinId = skin->id;
    task.minPrice = 0;
    task.maxPrice = m_inspectorPrice->value();
    task.maxWear = m_inspectorWear->value();
    task.quantity = m_inspectorQuantity->value();
    task.condition = m_inspectorCondition->currentText();
    task.enabled = true;
    task.status = QStringLiteral("寰呭惎鍔?");
    m_state->pauseSimulation();
    m_state->tasks.append(task);
    m_state->addLog("INFO", QStringLiteral("浠庡叧娉ㄥ伐浣滃尯鍒涘缓鏈湴浠诲姟锛?") + task.name);
    m_state->notifyChanged();
    m_favoriteTaskInfo->setText(QStringLiteral("宸插垱寤?路 鍙湪浠诲姟椤电紪杈?"));
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
        put(m_tasksTable, row, 1, skin ? skinChoice(*skin) : QStringLiteral("鐩爣宸茬Щ闄?"), skin ? QColor() : FluentTheme::muted);
        put(m_tasksTable, row, 2, task.condition, FluentTheme::secondary);
        put(m_tasksTable, row, 3, QStringLiteral("%1 鈥?%2").arg(amount(task.minPrice), amount(task.maxPrice)));
        put(m_tasksTable, row, 4, QString::number(task.maxWear, 'f', 3), FluentTheme::secondary);
        put(m_tasksTable, row, 5, QString::number(task.quantity));
        auto* toggle = new ToggleSwitch;
        toggle->setObjectName("taskEnabled_" + task.id);
        toggle->setChecked(task.enabled);
        toggle->setToolTip(task.enabled ? QStringLiteral("鏆傚仠浠诲姟") : QStringLiteral("鍚敤浠诲姟"));
        m_tasksTable->setCellWidget(row, 6, centered(toggle));
        connect(toggle, &QCheckBox::toggled, this, [this, id = task.id](bool checked) {
            if (m_refreshing) return;
            for (auto& value : m_state->tasks) {
                if (value.id != id) continue;
                value.enabled = checked;
                value.status = checked ? QStringLiteral("寰呭惎鍔?") : QStringLiteral("婕旂ず宸叉殏鍋?");
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
    m_taskCount->setText(QStringLiteral("%1 鏉′换鍔?路 %2 鏉″惎鐢?").arg(m_state->tasks.size()).arg(active));
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
        for (auto* metric : m_priceCards) metric->setValue(QStringLiteral("鈥?"), QStringLiteral("鏆傛棤鐨偆鏍锋湰"));
        m_priceChart->setSeries({});
        m_priceHistory->setRowCount(0);
        return;
    }
    const auto values = series(skin->price);
    m_priceCards[0]->setValue(amount(skin->price), QStringLiteral("婕旂ず鎶ヤ环 路 闈炲疄闄呮垚浜や环"));
    m_priceCards[1]->setValue(amount(*std::min_element(values.begin(), values.end())), QStringLiteral("鍚堟垚鏍锋湰鑼冨洿"));
    m_priceCards[2]->setValue(amount(*std::max_element(values.begin(), values.end())), QStringLiteral("鍚堟垚鏍锋湰鑼冨洿"));
    m_priceChart->setSeries(values);
    m_priceChart->setCaption(skin->name + QStringLiteral(" 路 婕旂ず鏁版嵁"));
    m_priceHistory->setRowCount(6);
    for (int row = 0; row < 6; ++row) {
        const int index = values.size() - 1 - row;
        put(m_priceHistory, row, 0, QStringLiteral("鏍锋湰 %1").arg(index + 1), FluentTheme::secondary);
        put(m_priceHistory, row, 1, skin->name);
        put(m_priceHistory, row, 2, amount(values[index]));
        put(m_priceHistory, row, 3, QStringLiteral("婕旂ず鎶ヤ环"), FluentTheme::secondary);
        put(m_priceHistory, row, 4, QStringLiteral("鏈湴鍚堟垚 路 闈炴父鎴忛噰闆?"), FluentTheme::secondary);
    }
}

void MainWindow::refreshStats()
{
    const int scans = m_state->simulatedScans, matches = m_state->simulatedMatches, success = m_state->simulatedSuccess;
    m_statsCards[0]->setValue(QString::number(scans), QStringLiteral("鏈婕旂ず鎵弿鏉＄洰"));
    m_statsCards[1]->setValue(QString::number(matches), QStringLiteral("绗﹀悎妯℃嫙绛涢€夋潯浠?"));
    m_statsCards[2]->setValue(QString::number(success), QStringLiteral("涓嶆槸瀹為檯鎴愪氦"));
    m_statsCards[3]->setValue(QStringLiteral("鈥?"), QStringLiteral("鏈繛鎺ユ墽琛屾ā鍧?"));
    const QVector<int> values = {qMax(0, scans - matches), qMax(0, matches - success), success};
    for (int i = 0; i < values.size(); ++i) {
        const double fraction = scans > 0 ? values[i] * 100.0 / scans : 0;
        m_reasonLabels[i]->setText(QStringLiteral("%1 路 %2%").arg(values[i]).arg(fraction, 0, 'f', 1));
        m_reasonBars[i]->setValue(qRound(fraction));
    }
    m_statsNote->setText(QStringLiteral("妯℃嫙鏈喅锛?銆傜粺璁″彧鎻忚堪鏈湴妯℃嫙寮曟搸锛屼笉浠ｈ〃瀹為檯娓告垙缁撴灉銆?"));
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
    m_logCount->setText(QStringLiteral("鏄剧ず %1 / %2 鏉?").arg(m_logsTable->rowCount()).arg(m_state->logs.size()));
    if (m_autoScrollLogs && m_autoScrollLogs->isChecked()) m_logsTable->scrollToTop();
}

void MainWindow::showHelp()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("helpDialog"));
    dialog.setWindowTitle(QStringLiteral("甯姪"));
    auto* outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* content = new QFrame;
    content->setObjectName(QStringLiteral("dialogContent"));
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(24, 22, 24, 22);
    body->setSpacing(0);
    body->addWidget(label(QStringLiteral("浣跨敤璇存槑"), QStringLiteral("dialogTitle")));
    body->addSpacing(16);
    const QVector<QPair<QString, QString>> steps = {
        {QStringLiteral("鍏虫敞"), QStringLiteral("绛涢€夋垨鎼滅储鐨偆锛屽湪鍙充晶濉啓鏈€楂樹环鏍笺€佹垚鑹层€佹渶澶х（鎹熷拰鏁伴噺鍚庡垱寤轰换鍔°€?")},
        {QStringLiteral("浠诲姟"), QStringLiteral("绠＄悊鏀惰棌浠诲姟锛氱洰鏍囩毊鑲ゃ€佹垚鑹层€佷环鏍煎尯闂淬€佹渶澶х（鎹熴€侀檺鍒堕噺涓庡惎鐢ㄥ紑鍏炽€?")},
        {QStringLiteral("杩愯"), QStringLiteral("璁剧疆璐拱寤惰繜銆佽繛鐐规ā寮忋€佸搧绾ч檺璐€佹寕鏈轰笌鑷姩鏀惰棌鍙傛暟锛屼互鍙婅繍琛屽揩鎹烽敭鍜屽畾鏃躲€?")},
        {QStringLiteral("璁板綍"), QStringLiteral("鍦ㄦ棩蹇椾笌缁熻椤垫煡鐪嬭繍琛屼簨浠跺拰缁撴灉锛涘鍏ャ€佸鍑轰綅浜庣獥鍙ｅ彸涓婅銆?")}};
    for (const auto& step : steps) {
        body->addWidget(label(step.first, QStringLiteral("cardTitle")));
        body->addSpacing(2);
        auto* text = label(step.second, QStringLiteral("cardCaption"));
        text->setWordWrap(true);
        body->addWidget(text);
        body->addSpacing(14);
    }
    body->addWidget(label(QStringLiteral("杩愯蹇嵎閿細%1锛堟帴鍏ユ墽琛屾ā鍧楀悗鐢熸晥锛?").arg(m_state->run.hotkey), QStringLiteral("cardCaption")));
    body->addSpacing(4);
    auto* demo = label(QStringLiteral("褰撳墠鐗堟湰鏄湰鍦版紨绀哄墠绔細涓嶈繛鎺ユ父鎴忥紝涓嶈鍙栧疄鏃跺競鍦猴紝涔熶笉鎵ц鐐瑰嚮鎴栬喘涔般€?"), QStringLiteral("tertiaryLabel"));
    demo->setWordWrap(true);
    body->addWidget(demo);
    outer->addWidget(content, 1);
    auto* commands = new QFrame;
    commands->setObjectName(QStringLiteral("dialogCommands"));
    commands->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* commandRow = new QHBoxLayout(commands);
    commandRow->setContentsMargins(24, 20, 24, 20);
    commandRow->addStretch();
    auto* close = button(QStringLiteral("鐭ラ亾浜?"), QStringLiteral("helpCloseButton"), ButtonKind::Accent);
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
    dialog.setWindowTitle(editing ? QStringLiteral("缂栬緫妯℃嫙浠诲姟") : QStringLiteral("鏂板妯℃嫙浠诲姟"));
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
    body->addWidget(label(editing ? QStringLiteral("璋冩暣浠诲姟鏉′欢") : QStringLiteral("鍒涘缓涓€鏉℃ā鎷熶换鍔?"), QStringLiteral("dialogTitle")));
    body->addSpacing(6);
    body->addWidget(label(QStringLiteral("杩欎簺瑙勫垯鍙敤浜庢湰鍦版紨绀猴紝涓嶄細瑙﹀彂瀹為檯璐拱銆?"), QStringLiteral("cardCaption")));
    body->addSpacing(20);
    auto* name = new QLineEdit;
    name->setObjectName(QStringLiteral("taskName"));
    name->setMaxLength(60);
    name->setPlaceholderText(QStringLiteral("渚嬪锛氬ぉ鍛戒綆浠峰叧娉?"));
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
    const QString conditionValue = editing ? existing.condition : (defaultSkin ? defaultSkin->condition : QStringLiteral("涓嶉檺"));
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
    field(QStringLiteral("浠诲姟鍚嶇О"), name, 0, 0, 2);
    form->setRowMinimumHeight(2, 12);
    field(QStringLiteral("鐩爣鐨偆"), skin, 3, 0);
    field(QStringLiteral("成色"), condition, 3, 1);
    form->setRowMinimumHeight(5, 12);
    field(QStringLiteral("鏈€浣庝环鏍?"), min, 6, 0);
    field(QStringLiteral("鏈€楂樹环鏍?"), max, 6, 1);
    form->setRowMinimumHeight(8, 12);
    field(QStringLiteral("鏈€澶х（鎹?"), wear, 9, 0);
    field(QStringLiteral("鏁伴噺涓婇檺"), quantity, 9, 1);
    form->setColumnStretch(0, 1);
    form->setColumnStretch(1, 1);
    body->addLayout(form);
    body->addSpacing(16);
    auto* enableRow = new QHBoxLayout;
    enableRow->addWidget(label(QStringLiteral("鍚敤姝や换鍔?")));
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
    auto* save = button(QStringLiteral("淇濆瓨浠诲姟"), QStringLiteral("taskSaveButton"), ButtonKind::Accent);
    save->setDefault(true);
    auto* cancel = button(QStringLiteral("鍙栨秷"), QStringLiteral("taskCancelButton"));
    commandRow->addWidget(save, 1);
    commandRow->addWidget(cancel, 1);
    outer->addWidget(commands);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(save, &QPushButton::clicked, &dialog, [&] {
        if (name->text().trimmed().isEmpty()) { validation->setText(QStringLiteral("璇疯緭鍏ヤ换鍔″悕绉般€?")); name->setFocus(); return; }
        if (skin->currentIndex() < 0) { validation->setText(QStringLiteral("璇烽€夋嫨鐩爣鐨偆銆?")); return; }
        if (min->value() > max->value()) { validation->setText(QStringLiteral("鏈€浣庝环鏍奸渶瑕佸皬浜庢垨绛変簬鏈€楂樹环鏍笺€?")); return; }
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
    task.status = task.enabled ? QStringLiteral("寰呭惎鍔?") : QStringLiteral("婕旂ず宸叉殏鍋?");
    if (editing) {
        for (auto& target : m_state->tasks) {
            if (target.id == id) { target = task; break; }
        }
        m_state->addLog("INFO", QStringLiteral("鏇存柊鏈湴浠诲姟锛?") + task.name);
        m_state->resetTaskSimulation(task.id);
    } else {
        m_state->tasks.append(task);
        m_state->addLog("INFO", QStringLiteral("鏂板鏈湴浠诲姟锛?") + task.name);
        m_state->notifyChanged();
    }
}

void MainWindow::deleteSelectedTasks()
{
    QStringList ids;
    for (const auto& index : m_tasksTable->selectionModel()->selectedRows())
        if (auto* item = m_tasksTable->item(index.row(), 0)) ids.append(item->data(Qt::UserRole).toString());
    if (ids.isEmpty()) return;
    if (!confirm(this, QStringLiteral("鍒犻櫎浠诲姟"), QStringLiteral("鍒犻櫎閫変腑鐨?%1 鏉℃湰鍦颁换鍔★紵鍏虫敞鍒楄〃淇濇寔涓嶅彉銆?").arg(ids.size()), QStringLiteral("鍒犻櫎")))
        return;
    for (int i = m_state->tasks.size() - 1; i >= 0; --i) {
        if (ids.contains(m_state->tasks[i].id)) m_state->tasks.removeAt(i);
    }
    m_state->addLog("INFO", QStringLiteral("鍒犻櫎 %1 鏉℃湰鍦颁换鍔?").arg(ids.size()));
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
            task.status = enabled ? QStringLiteral("寰呭惎鍔?") : QStringLiteral("婕旂ず宸叉殏鍋?");
        }
    }
    m_state->addLog("INFO", QStringLiteral("%1 %2 鏉￠€変腑浠诲姟").arg(enabled ? QStringLiteral("鍚敤") : QStringLiteral("鏆傚仠")).arg(ids.size()));
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
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("瀵煎嚭鏈湴閰嶇疆"), "relink-config.json", QStringLiteral("JSON 閰嶇疆 (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    if (!m_state->saveTo(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("瀵煎嚭澶辫触"), error);
        return;
    }
    m_state->addLog("INFO", QStringLiteral("宸插鍑烘湰鍦伴厤缃?"));
    m_state->notifyChanged();
}

void MainWindow::exportPrices()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("瀵煎嚭婕旂ず鎶ヤ环"), "relink-demo-prices.csv", QStringLiteral("CSV 鏂囦欢 (*.csv)"));
    if (path.isEmpty()) return;
    QString error;
    if (!m_state->exportPricesCsv(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("瀵煎嚭澶辫触"), error);
        return;
    }
    m_state->addLog("INFO", QStringLiteral("宸插鍑烘紨绀烘姤浠?CSV锛堥潪娓告垙閲囬泦鏁版嵁锛?"));
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
        m_settingsMessage->setText(QStringLiteral("璁剧疆涓庢湰鍦伴厤缃凡淇濆瓨"));
        m_state->configPath = path;
        m_state->addLog("INFO", QStringLiteral("宸蹭繚瀛樼晫闈㈣缃笌鏈湴閰嶇疆"));
        m_state->notifyChanged();
    } else {
        m_settingsMessage->setText(QStringLiteral("閰嶇疆淇濆瓨澶辫触锛?") + error);
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

