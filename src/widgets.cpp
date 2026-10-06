#include "widgets.h"
#include "fluenttheme.h"

#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QLinearGradient>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QSizePolicy>
#include <QStyleOptionSpinBox>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>


namespace {
QString priceNumber(double value)
{
    const QLocale locale(QLocale::English, QLocale::UnitedStates);
    return locale.toString(value, 'f', 2);
}

QString axisNumber(double value, double interval)
{
    const double magnitude = std::abs(value);
    if (magnitude >= 1000000.0)
        return QString::number(value / 1000000.0, 'f', interval < 100000.0 ? 2 : 1)
            + QStringLiteral("m");
    if (magnitude >= 10000.0)
        return QString::number(value / 1000.0, 'f', interval < 100.0 ? 2 : 1)
            + QStringLiteral("k");
    const int decimals = interval < 0.1 ? 2 : interval < 1.0 ? 1 : 0;
    const QLocale locale(QLocale::English, QLocale::UnitedStates);
    return locale.toString(value, 'f', decimals);
}

// Keyboard focus visual: 2 px dark outer stroke, as in WinUI light theme.
void drawFocusVisual(QPainter &painter, const QRectF &rect, qreal radius)
{
    painter.save();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0, 0, 0, 228), 2));
    painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), radius, radius);
    painter.restore();
}

// Windows 11 drop-down item: rounded hover plate and a pill on the current item.
class ComboItemDelegate final : public QStyledItemDelegate
{
public:
    explicit ComboItemDelegate(QComboBox *combo) : QStyledItemDelegate(combo), combo_(combo) {}
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRectF plate = QRectF(option.rect).adjusted(4, 2, -4, -2);
        const bool current = index.row() == combo_->currentIndex();
        const bool highlighted = option.state & (QStyle::State_Selected | QStyle::State_MouseOver);
        if (highlighted || current) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(0, 0, 0, highlighted ? 15 : 9));
            painter->drawRoundedRect(plate, 4, 4);
        }
        if (current) {
            painter->setBrush(FluentTheme::accent);
            painter->drawRoundedRect(QRectF(plate.left(), plate.center().y() - 8, 3, 16), 1.5, 1.5);
        }
        painter->setFont(FluentTheme::font(14));
        painter->setPen(option.state & QStyle::State_Enabled ? FluentTheme::text : FluentTheme::disabled);
        const QRectF textRect = plate.adjusted(12, 0, -8, 0);
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                          QFontMetrics(painter->font()).elidedText(index.data().toString(), Qt::ElideRight,
                                                                   qRound(textRect.width())));
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        return QSize(QFontMetrics(FluentTheme::font(14)).horizontalAdvance(index.data().toString()) + 44, 36);
    }

private:
    QComboBox *combo_;
};

void paintSpinChevrons(QPainter &painter, QAbstractSpinBox *box, const QStyleOptionSpinBox &option)
{
    const QRect up = box->style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxUp, box);
    const QRect down = box->style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxDown, box);
    const bool enabled = box->isEnabled();
    const QColor upInk = enabled && (option.stepEnabled & QAbstractSpinBox::StepUpEnabled)
        ? FluentTheme::secondary : FluentTheme::disabled;
    const QColor downInk = enabled && (option.stepEnabled & QAbstractSpinBox::StepDownEnabled)
        ? FluentTheme::secondary : FluentTheme::disabled;
    FluentTheme::drawGlyph(&painter, QRectF(up).translated(0, 1), Glyph::ChevronUp, upInk, 9);
    FluentTheme::drawGlyph(&painter, QRectF(down).translated(0, -1), Glyph::ChevronDown, downInk, 9);
}
}

MetricCard::MetricCard(const QString &title, const QString &value,
                       const QString &detail, const QColor &accent,
                       QWidget *parent)
    : QFrame(parent)
{
    // The accent argument remains source-compatible; metrics share one
    // quiet strip instead of separately colored dashboard cards.
    Q_UNUSED(accent);
    setObjectName(QStringLiteral("metricCard"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMinimumHeight(96);
    setAccessibleName(title);

    auto *body = new QVBoxLayout(this);
    body->setContentsMargins(20, 14, 20, 14);
    body->setSpacing(2);

    auto *titleLabel = new QLabel(title, this);
    titleLabel->setObjectName(QStringLiteral("metricTitle"));
    titleLabel->setWordWrap(true);
    body->addWidget(titleLabel);

    valueLabel_ = new QLabel(value, this);
    valueLabel_->setObjectName(QStringLiteral("metricValue"));
    valueLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->addWidget(valueLabel_);

    detailLabel_ = new QLabel(detail, this);
    detailLabel_->setObjectName(QStringLiteral("metricDetail"));
    detailLabel_->setWordWrap(true);
    body->addWidget(detailLabel_);
}

void MetricCard::setValue(const QString &value, const QString &detail)
{
    valueLabel_->setText(value);
    if (!detail.isNull())
        detailLabel_->setText(detail);
}

PriceChart::PriceChart(QWidget *parent)
    : QWidget(parent), caption_(QStringLiteral("价格走势"))
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(220, 150);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("模拟价格趋势图"));
    setToolTip(QStringLiteral("前端演示样本，不代表游戏市场实时价格；横轴表示样本顺序。"));
}

void PriceChart::setSeries(const QVector<double> &values)
{
    series_.clear();
    series_.reserve(values.size());
    for (double value : values) {
        if (std::isfinite(value))
            series_.append(value);
    }
    hoverIndex_ = -1;
    update();
}

void PriceChart::setCaption(const QString &caption)
{
    caption_ = caption;
    update();
}

void PriceChart::setHeaderVisible(bool visible)
{
    headerVisible_ = visible;
    update();
}

void PriceChart::mouseMoveEvent(QMouseEvent *event)
{
    int index = -1;
    if (series_.size() > 1 && plot_.width() > 0
        && plot_.adjusted(-6, -6, 6, 6).contains(event->position())) {
        const double ratio = (event->position().x() - plot_.left()) / plot_.width();
        index = std::clamp(qRound(ratio * (series_.size() - 1)), 0, int(series_.size()) - 1);
    }
    if (index != hoverIndex_) {
        hoverIndex_ = index;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void PriceChart::leaveEvent(QEvent *event)
{
    hoverIndex_ = -1;
    update();
    QWidget::leaveEvent(event);
}

void PriceChart::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QColor ink = FluentTheme::text;
    const QColor secondary = FluentTheme::secondary;
    const QColor tertiary = FluentTheme::muted;
    const QColor line = FluentTheme::accent;
    const bool compact = width() < 420;
    const int side = compact ? 4 : 6;

    int top = 10;
    if (headerVisible_) {
        // Charts are always labeled as synthetic; the badge is only added when
        // the caption itself does not already say so.
        const bool labeled = caption_.contains(QStringLiteral("模拟"))
            || caption_.contains(QStringLiteral("演示"))
            || caption_.contains(QStringLiteral("合成"));
        const int badgeWidth = labeled ? 0 : 64;
        painter.setFont(FluentTheme::font(12));
        painter.setPen(secondary);
        const int titleWidth = std::max(0, width() - 2 * side - badgeWidth);
        painter.drawText(QRectF(side, 0, titleWidth, 22), Qt::AlignVCenter | Qt::AlignLeft,
                         QFontMetrics(painter.font()).elidedText(caption_, Qt::ElideRight, titleWidth));
        if (!labeled) {
            painter.setPen(tertiary);
            painter.drawText(QRectF(width() - side - badgeWidth, 0, badgeWidth, 22),
                             Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("模拟数据"));
        }
        top = 36;
    }

    double minimum = 0.0;
    double maximum = 100.0;
    if (!series_.isEmpty()) {
        const auto extrema = std::minmax_element(series_.cbegin(), series_.cend());
        minimum = *extrema.first;
        maximum = *extrema.second;
        const double padding = std::max((maximum - minimum) * 0.18,
                                       std::max(1.0, std::abs(maximum) * 0.005));
        minimum = minimum >= 0.0 ? std::max(0.0, minimum - padding) : minimum - padding;
        maximum += padding;
        if (maximum <= minimum)
            maximum = minimum + 1.0;
    }

    const int intervals = height() < 220 ? 3 : 4;
    const double interval = (maximum - minimum) / intervals;
    painter.setFont(FluentTheme::font(11));
    const QFontMetrics axisMetrics(painter.font());
    int labelWidth = 0;
    for (int i = 0; i <= intervals; ++i)
        labelWidth = std::max(labelWidth,
            axisMetrics.horizontalAdvance(axisNumber(maximum - interval * i, interval)));
    labelWidth = std::min(labelWidth, compact ? 48 : 64);

    const QString lastPrice = series_.isEmpty() ? QString() : priceNumber(series_.last());
    const QFont tagFont = FluentTheme::font(12, QFont::DemiBold);
    const int tagWidth = QFontMetrics(tagFont).horizontalAdvance(lastPrice) + 16;
    const bool outerTag = !compact && tagWidth < width() / 4;
    const double rightSpace = outerTag ? tagWidth + 12.0 : 8.0;
    const QRectF plot(side + labelWidth + 12.0, top + 4.0,
                      width() - 2.0 * side - labelWidth - 12.0 - rightSpace,
                      height() - top - 34.0);
    plot_ = plot;
    if (plot.width() <= 30.0 || plot.height() <= 25.0)
        return;

    for (int i = 0; i <= intervals; ++i) {
        const double y = std::round(plot.top() + plot.height() * i / intervals) + 0.5;
        painter.setPen(QPen(FluentTheme::grid, 1));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(tertiary);
        const QString tick = axisMetrics.elidedText(
            axisNumber(maximum - interval * i, interval), Qt::ElideRight, labelWidth);
        painter.drawText(QRectF(side, y - 8.0, labelWidth, 16.0),
                         Qt::AlignRight | Qt::AlignVCenter, tick);
    }

    if (series_.isEmpty()) {
        painter.setPen(secondary);
        painter.setFont(FluentTheme::font(14));
        painter.drawText(plot, Qt::AlignCenter, QStringLiteral("暂无价格样本"));
        return;
    }

    // No timestamps accompany these frontend samples. Index labels make that
    // explicit rather than presenting invented intraday market timestamps.
    const int tickCount = series_.size() == 1 ? 1
        : std::min<int>(series_.size(), plot.width() >= 700.0 ? 24 : (plot.width() >= 400.0 ? 12 : 5));
    painter.setPen(tertiary);
    for (int i = 0; i < tickCount; ++i) {
        const int index = tickCount == 1 ? 0
            : qRound((series_.size() - 1.0) * i / (tickCount - 1.0));
        const double x = series_.size() == 1 ? plot.center().x()
            : plot.left() + plot.width() * index / (series_.size() - 1.0);
        const QString label = QStringLiteral("S%1").arg(index + 1);
        const double textWidth = axisMetrics.horizontalAdvance(label) + 2.0;
        const double left = std::clamp(x - textWidth / 2.0,
                                      plot.left(), std::max(plot.left(), plot.right() - textWidth));
        painter.drawText(QRectF(left, plot.bottom() + 9.0, textWidth, 17.0),
                         Qt::AlignHCenter | Qt::AlignVCenter, label);
    }

    QVector<QPointF> points;
    points.reserve(series_.size());
    for (int i = 0; i < series_.size(); ++i) {
        const double x = series_.size() == 1 ? plot.center().x()
            : plot.left() + plot.width() * i / (series_.size() - 1.0);
        const double y = plot.bottom() - (series_[i] - minimum) / (maximum - minimum) * plot.height();
        points.append(QPointF(x, y));
    }

    QPainterPath path;
    path.moveTo(points.first());
    for (int i = 1; i < points.size(); ++i)
        path.lineTo(points[i]);

    painter.save();
    painter.setClipRect(plot.adjusted(-4.0, -4.0, 4.0, 4.0));
    // A faint neutral wash under the line, as in Task Manager's graphs.
    QPainterPath area(path);
    area.lineTo(points.last().x(), plot.bottom());
    area.lineTo(points.first().x(), plot.bottom());
    area.closeSubpath();
    QLinearGradient wash(0, plot.top(), 0, plot.bottom());
    wash.setColorAt(0, QColor(0, 0, 0, 22));
    wash.setColorAt(1, QColor(0, 0, 0, 0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(wash);
    painter.drawPath(area);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(line, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
    painter.setPen(QPen(FluentTheme::surface, 2));
    painter.setBrush(line);
    painter.drawEllipse(points.last(), 4.0, 4.0);
    painter.restore();

    // Only the last observed price receives a permanent label.
    painter.setFont(tagFont);
    const QPointF last = points.last();
    QRectF tag;
    if (outerTag) {
        tag = QRectF(plot.right() + 10.0, last.y() - 11.0, tagWidth, 22.0);
    } else {
        const double tagLeft = std::max(plot.left(), last.x() - tagWidth);
        const double tagTop = std::clamp(last.y() - 30.0, plot.top() + 1.0,
                                        std::max(plot.top() + 1.0, plot.bottom() - 24.0));
        tag = QRectF(tagLeft, tagTop, tagWidth, 22.0);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(FluentTheme::accent);
    painter.drawRoundedRect(tag, 4, 4);
    painter.setPen(FluentTheme::onAccent);
    painter.drawText(tag, Qt::AlignCenter, lastPrice);

    if (hoverIndex_ >= 0 && hoverIndex_ < points.size()) {
        const QPointF point = points[hoverIndex_];
        painter.setPen(QPen(QColor(0, 0, 0, 60), 1, Qt::DashLine));
        painter.drawLine(QPointF(point.x(), plot.top()), QPointF(point.x(), plot.bottom()));
        painter.setPen(QPen(FluentTheme::surface, 2));
        painter.setBrush(line);
        painter.drawEllipse(point, 4.5, 4.5);
        // Flyout-style readout: sample index and value.
        const QString index = QStringLiteral("S%1").arg(hoverIndex_ + 1);
        const QString value = priceNumber(series_[hoverIndex_]);
        const QFont small = FluentTheme::font(12);
        const int bubbleWidth = QFontMetrics(small).horizontalAdvance(index)
            + QFontMetrics(tagFont).horizontalAdvance(value) + 30;
        QRectF bubble(point.x() + 12, plot.top() + 2, bubbleWidth, 28);
        if (bubble.right() > width() - 2)
            bubble.moveRight(point.x() - 12);
        painter.setPen(QPen(FluentTheme::stroke, 1));
        painter.setBrush(FluentTheme::surface);
        painter.drawRoundedRect(bubble, 6, 6);
        painter.setFont(small);
        painter.setPen(secondary);
        painter.drawText(bubble.adjusted(10, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, index);
        painter.setFont(tagFont);
        painter.setPen(ink);
        painter.drawText(bubble.adjusted(0, 0, -10, 0), Qt::AlignVCenter | Qt::AlignRight, value);
    }
}

NavRailButton::NavRailButton(const QString &text, char16_t glyph, char16_t selectedGlyph, QWidget *parent)
    : QPushButton(text, parent), glyph_(glyph), selectedGlyph_(selectedGlyph ? selectedGlyph : glyph)
{
    setCheckable(true);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setAccessibleName(text);
    setToolTip(text);
}

QSize NavRailButton::sizeHint() const
{
    return QSize(64, 56);
}

void NavRailButton::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool on = isChecked();
    const bool hovered = underMouse() && isEnabled();
    const QRectF plate = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (on) {
        // The selected item reads as a raised white plate on Mica.
        painter.setPen(QPen(QColor(0, 0, 0, 12), 1));
        painter.setBrush(isDown() ? QColor("#FAFAFA") : QColor("#FFFFFF"));
        painter.drawRoundedRect(plate, 6, 6);
        painter.setPen(Qt::NoPen);
        painter.setBrush(FluentTheme::accent);
        painter.drawRoundedRect(QRectF(0, height() / 2.0 - 8, 3, 16), 1.5, 1.5);
        FluentTheme::drawGlyph(&painter, QRectF(rect()), selectedGlyph_,
                               isEnabled() ? FluentTheme::text : FluentTheme::disabled, 22);
    } else {
        if (hovered || isDown()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, isDown() ? 6 : 9));
            painter.drawRoundedRect(plate, 6, 6);
        }
        const QColor ink = !isEnabled() ? FluentTheme::disabled
            : (hovered ? FluentTheme::text : FluentTheme::secondary);
        FluentTheme::drawGlyph(&painter, QRectF(0, 7, width(), 24), glyph_, ink, 20);
        painter.setFont(FluentTheme::font(12));
        painter.setPen(ink);
        painter.drawText(QRectF(2, 32, width() - 4, 17), Qt::AlignHCenter | Qt::AlignVCenter, text());
    }
    if (hasFocus())
        drawFocusVisual(painter, plate, 6);
}

PillButton::PillButton(const QString &text, QWidget *parent)
    : QPushButton(text, parent)
{
    setCheckable(true);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void PillButton::setCount(int count)
{
    if (count_ == count)
        return;
    count_ = count;
    updateGeometry();
    update();
}

QSize PillButton::sizeHint() const
{
    int width = QFontMetrics(FluentTheme::font(14)).horizontalAdvance(text()) + 32;
    if (count_ >= 0)
        width += 6 + QFontMetrics(FluentTheme::font(12)).horizontalAdvance(QString::number(count_));
    return QSize(width, 32);
}

void PillButton::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool on = isChecked();
    const bool hovered = underMouse() && isEnabled();
    const QRectF plate = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = plate.height() / 2.0;
    if (on) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(isDown() ? FluentTheme::accentPressed
                                  : (hovered ? FluentTheme::accentHover : FluentTheme::accent));
    } else {
        painter.setPen(QPen(FluentTheme::stroke, 1));
        painter.setBrush(isDown() ? QColor("#F5F5F5") : (hovered ? QColor("#F6F6F6") : QColor("#FFFFFF")));
    }
    painter.drawRoundedRect(plate, radius, radius);

    const QFont labelFont = FluentTheme::font(14);
    const QFont countFont = FluentTheme::font(12);
    const QString count = count_ >= 0 ? QString::number(count_) : QString();
    const int labelWidth = QFontMetrics(labelFont).horizontalAdvance(text());
    const int countWidth = count.isEmpty() ? 0 : QFontMetrics(countFont).horizontalAdvance(count) + 6;
    const qreal left = (width() - labelWidth - countWidth) / 2.0;
    painter.setFont(labelFont);
    painter.setPen(on ? FluentTheme::onAccent : (isEnabled() ? FluentTheme::text : FluentTheme::disabled));
    painter.drawText(QRectF(left, 0, labelWidth + 1, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
    if (!count.isEmpty()) {
        painter.setFont(countFont);
        painter.setPen(on ? QColor(255, 255, 255, 178) : FluentTheme::secondary);
        painter.drawText(QRectF(left + labelWidth + 6, 0, countWidth, height()),
                         Qt::AlignVCenter | Qt::AlignLeft, count);
    }
    if (hasFocus())
        drawFocusVisual(painter, plate, radius);
}

ToggleSwitch::ToggleSwitch(QWidget *parent)
    : QCheckBox(parent)
{
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void ToggleSwitch::setStateTextVisible(bool visible)
{
    stateText_ = visible;
    updateGeometry();
    update();
}

QSize ToggleSwitch::sizeHint() const
{
    if (!stateText_)
        return QSize(44, 24);
    const QFontMetrics metrics(FluentTheme::font(14));
    const int text = std::max(metrics.horizontalAdvance(QStringLiteral("开")),
                              metrics.horizontalAdvance(QStringLiteral("关")));
    return QSize(text + 12 + 44, 32);
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool on = isChecked();
    const bool hovered = underMouse() && isEnabled();
    const QRectF track(width() - 42.0, (height() - 20.0) / 2.0, 40.0, 20.0);
    if (stateText_) {
        painter.setFont(FluentTheme::font(14));
        painter.setPen(isEnabled() ? FluentTheme::text : FluentTheme::disabled);
        painter.drawText(QRectF(0, 0, track.left() - 12, height()), Qt::AlignVCenter | Qt::AlignRight,
                         on ? QStringLiteral("开") : QStringLiteral("关"));
    }
    const qreal knob = isDown() ? 14.0 : (hovered ? 14.0 : 12.0);
    if (on) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(!isEnabled() ? QColor("#C5C5C5")
                         : (hovered ? FluentTheme::accentHover : FluentTheme::accent));
        painter.drawRoundedRect(track, 10, 10);
        painter.setBrush(Qt::white);
        painter.drawEllipse(QPointF(track.right() - 10, track.center().y()), knob / 2, knob / 2);
    } else {
        painter.setPen(QPen(isEnabled() ? QColor("#8D8D8D") : QColor("#C5C5C5"), 1));
        painter.setBrush(hovered ? QColor(0, 0, 0, 15) : QColor(0, 0, 0, 6));
        painter.drawRoundedRect(track.adjusted(0.5, 0.5, -0.5, -0.5), 9.5, 9.5);
        painter.setPen(Qt::NoPen);
        painter.setBrush(isEnabled() ? FluentTheme::secondary : QColor("#C5C5C5"));
        painter.drawEllipse(QPointF(track.left() + 10, track.center().y()), knob / 2, knob / 2);
    }
    if (hasFocus())
        drawFocusVisual(painter, track.adjusted(-3, -3, 3, 3), 13);
}

StarToggle::StarToggle(QWidget *parent)
    : QCheckBox(parent)
{
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setFixedSize(28, 28);
}

void StarToggle::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool hovered = underMouse() && isEnabled();
    if (hovered) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 9));
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
    }
    if (isChecked())
        FluentTheme::drawGlyph(&painter, QRectF(rect()), Glyph::StarFill, FluentTheme::text, 16);
    else
        FluentTheme::drawGlyph(&painter, QRectF(rect()), Glyph::Star,
                               hovered ? FluentTheme::secondary : FluentTheme::muted, 16);
    if (hasFocus())
        drawFocusVisual(painter, QRectF(rect()), 4);
}

FluentComboBox::FluentComboBox(QWidget *parent)
    : QComboBox(parent)
{
    setItemDelegate(new ComboItemDelegate(this));
}

void FluentComboBox::paintEvent(QPaintEvent *event)
{
    QComboBox::paintEvent(event);
    QPainter painter(this);
    FluentTheme::drawGlyph(&painter, QRectF(width() - 34, 0, 28, height()), Glyph::ChevronDown,
                           isEnabled() ? FluentTheme::secondary : FluentTheme::disabled, 12);
}

void FluentSpinBox::paintEvent(QPaintEvent *event)
{
    QSpinBox::paintEvent(event);
    QStyleOptionSpinBox option;
    initStyleOption(&option);
    QPainter painter(this);
    paintSpinChevrons(painter, this, option);
}

void FluentDoubleSpinBox::paintEvent(QPaintEvent *event)
{
    QDoubleSpinBox::paintEvent(event);
    QStyleOptionSpinBox option;
    initStyleOption(&option);
    QPainter painter(this);
    paintSpinChevrons(painter, this, option);
}

void FluentTimeEdit::paintEvent(QPaintEvent *event)
{
    QTimeEdit::paintEvent(event);
    QStyleOptionSpinBox option;
    initStyleOption(&option);
    QPainter painter(this);
    paintSpinChevrons(painter, this, option);
}

ArtworkView::ArtworkView(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void ArtworkView::setArtwork(const QPixmap &artwork)
{
    artwork_ = artwork;
    update();
}

QRectF ArtworkView::artworkRect() const
{
    if (artwork_.isNull())
        return {};
    const QRectF frame = QRectF(rect()).adjusted(18, 14, -18, -14);
    const QSizeF size = QSizeF(artwork_.size()).scaled(frame.size(), Qt::KeepAspectRatio);
    return QRectF(frame.center().x() - size.width() / 2, frame.center().y() - size.height() / 2,
                  size.width(), size.height());
}

void ArtworkView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF plate = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QLinearGradient backdrop(plate.topLeft(), plate.bottomLeft());
    backdrop.setColorAt(0, QColor("#F8F8F8"));
    backdrop.setColorAt(1, QColor("#EEEEEE"));
    painter.setPen(QPen(FluentTheme::stroke, 1));
    painter.setBrush(backdrop);
    painter.drawRoundedRect(plate, 6, 6);
    if (artwork_.isNull()) {
        painter.setFont(FluentTheme::font(12));
        painter.setPen(FluentTheme::secondary);
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("暂无演示插图"));
        return;
    }
    painter.drawPixmap(artworkRect(), artwork_, QRectF(artwork_.rect()));
}
