#include "fluenttheme.h"
#include <QApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QIconEngine>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QWidget>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

QPalette FluentTheme::palette()
{
    QPalette pal;
    pal.setColor(QPalette::Window, window);
    pal.setColor(QPalette::WindowText, text);
    pal.setColor(QPalette::Base, surface);
    pal.setColor(QPalette::AlternateBase, layer);
    pal.setColor(QPalette::Text, body);
    pal.setColor(QPalette::Button, QColor("#FDFDFD"));
    pal.setColor(QPalette::ButtonText, text);
    pal.setColor(QPalette::Light, QColor("#FFFFFF"));
    pal.setColor(QPalette::Midlight, window);
    pal.setColor(QPalette::Mid, stroke);
    pal.setColor(QPalette::Dark, QColor("#D6D6D6"));
    pal.setColor(QPalette::Shadow, QColor("#C8C8C8"));
    pal.setColor(QPalette::Highlight, QColor("#D6D6D6"));
    pal.setColor(QPalette::HighlightedText, text);
    pal.setColor(QPalette::PlaceholderText, muted);
    pal.setColor(QPalette::ToolTipBase, flyout);
    pal.setColor(QPalette::ToolTipText, text);
    pal.setColor(QPalette::Link, accentText);
    pal.setColor(QPalette::LinkVisited, accentText);
    pal.setColor(QPalette::Accent, accent);
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
        pal.setColor(QPalette::Disabled, role, disabled);
    return pal;
}

QFont FluentTheme::font(int pixelSize, int weight)
{
    QFont font;
    font.setFamilies({QStringLiteral("Segoe UI"),
                      QStringLiteral("Microsoft YaHei UI")});
    font.setPixelSize(pixelSize);
    font.setWeight(static_cast<QFont::Weight>(weight));
    return font;
}

QFont FluentTheme::displayFont(int pixelSize, int weight)
{
    QFont font;
    font.setFamilies({QStringLiteral("Segoe UI"),
                      QStringLiteral("Microsoft YaHei UI")});
    font.setPixelSize(pixelSize);
    font.setWeight(static_cast<QFont::Weight>(weight));
    return font;
}

QFont FluentTheme::iconFont(int pixelSize)
{
    QFont font;
    font.setFamilies({QStringLiteral("Segoe Fluent Icons"), QStringLiteral("Segoe MDL2 Assets")});
    font.setPixelSize(pixelSize);
    font.setStyleStrategy(QFont::NoFontMerging);
    return font;
}

bool FluentTheme::hasIconFont()
{
    const QStringList families = QFontDatabase::families();
    return families.contains(QStringLiteral("Segoe Fluent Icons"))
        || families.contains(QStringLiteral("Segoe MDL2 Assets"));
}

void FluentTheme::drawGlyph(QPainter* painter, const QRectF& rect, char16_t glyph,
                            const QColor& color, int pixelSize, Qt::Alignment alignment)
{
    // The icon fonts use a one-em line box (ascent = em, descent = 0), so a
    // centered text rectangle centers the glyph's design square.
    painter->save();
    painter->setFont(iconFont(pixelSize));
    painter->setPen(color);
    painter->drawText(rect, alignment, QString(QChar(glyph)));
    painter->restore();
}

namespace {
class GlyphIconEngine final : public QIconEngine {
public:
    GlyphIconEngine(char16_t glyph, const QColor& normal, const QColor& active)
        : m_glyph(glyph), m_normal(normal), m_active(active) {}
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        QColor color = m_normal;
        if (mode == QIcon::Disabled) color = FluentTheme::disabled;
        else if (mode == QIcon::Active || mode == QIcon::Selected || state == QIcon::On) color = m_active;
        FluentTheme::drawGlyph(painter, rect, m_glyph, color, qMax(8, qMin(rect.width(), rect.height())));
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap pixels(size * scale);
        pixels.setDevicePixelRatio(scale);
        pixels.fill(Qt::transparent);
        QPainter painter(&pixels);
        painter.setRenderHint(QPainter::TextAntialiasing);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return pixels;
    }
    QIconEngine* clone() const override { return new GlyphIconEngine(*this); }
    QString key() const override { return QStringLiteral("RelinkGlyph"); }
private:
    char16_t m_glyph;
    QColor m_normal, m_active;
};

class AppIconEngine final : public QIconEngine {
public:
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode, QIcon::State) override
    {
        FluentTheme::paintAppIcon(painter, rect);
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode, QIcon::State, qreal scale) override
    {
        QPixmap pixels(size * scale);
        pixels.setDevicePixelRatio(scale);
        pixels.fill(Qt::transparent);
        QPainter painter(&pixels);
        FluentTheme::paintAppIcon(&painter, QRectF(QPointF(), QSizeF(size)));
        return pixels;
    }
    QList<QSize> availableSizes(QIcon::Mode, QIcon::State) override
    {
        return {QSize(16, 16), QSize(24, 24), QSize(32, 32), QSize(48, 48), QSize(64, 64), QSize(256, 256)};
    }
    QIconEngine* clone() const override { return new AppIconEngine; }
    QString key() const override { return QStringLiteral("RelinkAppIcon"); }
};
}

QIcon FluentTheme::glyphIcon(char16_t glyph, const QColor& normal, const QColor& active)
{
    return QIcon(new GlyphIconEngine(glyph, normal, active));
}

QIcon FluentTheme::appIcon()
{
    return QIcon(new AppIconEngine);
}

void FluentTheme::paintAppIcon(QPainter* painter, const QRectF& rect)
{
    // Neutral charcoal tile with a single price trace; no brand colors.
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const qreal s = qMin(rect.width(), rect.height());
    const QRectF tile(rect.center().x() - s * 0.45, rect.center().y() - s * 0.45, s * 0.9, s * 0.9);
    QLinearGradient fill(tile.topLeft(), tile.bottomLeft());
    fill.setColorAt(0, QColor("#4A4A4A"));
    fill.setColorAt(1, QColor("#1C1C1C"));
    painter->setPen(Qt::NoPen);
    painter->setBrush(fill);
    painter->drawRoundedRect(tile, s * 0.2, s * 0.2);
    if (s >= 24) {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(QColor(255, 255, 255, 34), qMax(1.0, s / 64.0)));
        painter->drawRoundedRect(tile.adjusted(s / 128.0, s / 128.0, -s / 128.0, -s / 128.0), s * 0.19, s * 0.19);
    }
    const auto at = [&](qreal x, qreal y) { return QPointF(tile.left() + tile.width() * x, tile.top() + tile.height() * y); };
    QPainterPath trace;
    trace.moveTo(at(0.22, 0.66));
    trace.lineTo(at(0.42, 0.45));
    trace.lineTo(at(0.57, 0.57));
    trace.lineTo(at(0.76, 0.33));
    painter->setPen(QPen(Qt::white, qMax(1.4, s * 0.075), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->drawPath(trace);
    painter->setPen(Qt::NoPen);
    painter->setBrush(Qt::white);
    painter->drawEllipse(at(0.76, 0.33), s * 0.07, s * 0.07);
    painter->restore();
}

namespace {
class NativeFrameTheme final : public QObject {
public:
    explicit NativeFrameTheme(QObject* parent) : QObject(parent) {}
protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
#ifdef Q_OS_WIN
        if (event->type() == QEvent::Show) {
            auto* widget = qobject_cast<QWidget*>(watched);
            if (widget && widget->isWindow()) {
                const Qt::WindowType type = widget->windowType();
                const HWND handle = reinterpret_cast<HWND>(widget->winId());
                // Documented DWM values, declared here for older MinGW SDKs.
                constexpr DWORD immersiveDarkMode = 20;
                constexpr DWORD windowCornerPreference = 33;
                constexpr DWORD captionColorAttribute = 35;
                constexpr DWORD textColorAttribute = 36;
                if (type == Qt::Window || type == Qt::Dialog) {
                    // Light caption that matches the Mica base (or the white
                    // dialog surface); native caption buttons and snap stay.
                    const BOOL dark = FALSE;
                    const DWORD rounded = 2; // DWMWCP_ROUND
                    const COLORREF caption = type == Qt::Dialog ? RGB(255, 255, 255) : RGB(243, 243, 243);
                    const COLORREF captionText = RGB(27, 27, 27);
                    DwmSetWindowAttribute(handle, immersiveDarkMode, &dark, sizeof(dark));
                    DwmSetWindowAttribute(handle, windowCornerPreference, &rounded, sizeof(rounded));
                    DwmSetWindowAttribute(handle, captionColorAttribute, &caption, sizeof(caption));
                    DwmSetWindowAttribute(handle, textColorAttribute, &captionText, sizeof(captionText));
                } else if (type == Qt::Popup || type == Qt::ToolTip) {
                    // Windows 11 rounds menus and drop-downs (8 px) and tooltips (4 px).
                    const DWORD rounded = type == Qt::Popup ? 2 : 3; // DWMWCP_ROUND / ROUNDSMALL
                    DwmSetWindowAttribute(handle, windowCornerPreference, &rounded, sizeof(rounded));
                }
            }
        }
#else
        Q_UNUSED(watched);
        Q_UNUSED(event);
#endif
        return false;
    }
};
}

void FluentTheme::installNativeFrames(QApplication& app)
{
    // No native handles or desktop windows are created during offscreen tests.
    if (app.platformName() == QStringLiteral("windows"))
        app.installEventFilter(new NativeFrameTheme(&app));
}
