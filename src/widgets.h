#pragma once

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QTimeEdit>
#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QPaintEvent;

// Compact, stretchable metric. All text can be updated without recreating its
// layout; a null detail preserves the existing description.
class MetricCard : public QFrame
{
    Q_OBJECT
public:
    MetricCard(const QString &title, const QString &value, const QString &detail,
               const QColor &accent = QColor(), QWidget *parent = nullptr);
    void setValue(const QString &value, const QString &detail = QString());

private:
    QLabel *valueLabel_ = nullptr;
    QLabel *detailLabel_ = nullptr;
};

// Small dependency-free time-series view. This frontend explicitly labels
// its demo axis and values; it does not fetch prices or interact with a game.
class PriceChart : public QWidget
{
    Q_OBJECT
public:
    explicit PriceChart(QWidget *parent = nullptr);
    void setSeries(const QVector<double> &values);
    void setCaption(const QString &caption);
    // Cards that already show a header hide the chart's own caption row.
    void setHeaderVisible(bool visible);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QVector<double> series_;
    QString caption_;
    bool headerVisible_ = true;
    int hoverIndex_ = -1;
    QRectF plot_;
};

// NavigationView rail item in the Microsoft Store layout: outline glyph above
// a short label; the selected item shows only the filled glyph and a pill.
class NavRailButton : public QPushButton
{
    Q_OBJECT
public:
    NavRailButton(const QString &text, char16_t glyph, char16_t selectedGlyph = 0, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    char16_t glyph_;
    char16_t selectedGlyph_;
};

// Rounded filter chip with an optional count.
class PillButton : public QPushButton
{
    Q_OBJECT
public:
    explicit PillButton(const QString &text, QWidget *parent = nullptr);
    void setCount(int count);
    int count() const { return count_; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    int count_ = -1;
};

// Windows 11 ToggleSwitch. Remains a QCheckBox for keyboard handling and tests.
class ToggleSwitch : public QCheckBox
{
    Q_OBJECT
public:
    explicit ToggleSwitch(QWidget *parent = nullptr);
    // Settings cards show "开 / 关" beside the switch.
    void setStateTextVisible(bool visible);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *event) override;
    bool hitButton(const QPoint &pos) const override { return rect().contains(pos); }

private:
    bool stateText_ = false;
};

// Follow toggle drawn as an outline / filled star.
class StarToggle : public QCheckBox
{
    Q_OBJECT
public:
    explicit StarToggle(QWidget *parent = nullptr);
    QSize sizeHint() const override { return QSize(28, 28); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *event) override;
    bool hitButton(const QPoint &pos) const override { return rect().contains(pos); }
};

// ComboBox and NumberBox variants that draw Segoe Fluent chevrons.
class FluentComboBox : public QComboBox
{
    Q_OBJECT
public:
    explicit FluentComboBox(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
};

class FluentSpinBox : public QSpinBox
{
    Q_OBJECT
public:
    using QSpinBox::QSpinBox;

protected:
    void paintEvent(QPaintEvent *event) override;
};

class FluentDoubleSpinBox : public QDoubleSpinBox
{
    Q_OBJECT
public:
    using QDoubleSpinBox::QDoubleSpinBox;

protected:
    void paintEvent(QPaintEvent *event) override;
};

class FluentTimeEdit : public QTimeEdit
{
    Q_OBJECT
public:
    using QTimeEdit::QTimeEdit;

protected:
    void paintEvent(QPaintEvent *event) override;
};

// Product-page style hero: neutral backdrop with the item artwork centered.
class ArtworkView : public QWidget
{
    Q_OBJECT
public:
    explicit ArtworkView(QWidget *parent = nullptr);
    void setArtwork(const QPixmap &artwork);
    bool hasArtwork() const { return !artwork_.isNull(); }
    QRectF artworkRect() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QPixmap artwork_;
};
