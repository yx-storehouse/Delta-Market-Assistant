#pragma once
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QPalette>
#include <QRectF>
class QApplication;
class QPainter;

// Windows 11 (WinUI 3) light theme resources, flattened over the #F3F3F3 Mica
// base so QPainter code and theme.qss share the same values. The user asked
// for the neutral white/gray Windows look without blue, so the accent role is
// a neutral charcoal instead of the system blue.
namespace FluentTheme {
// Surfaces: Mica base -> content layer -> cards.
inline const QColor window("#F3F3F3");    // title bar and navigation rail (Mica)
inline const QColor layer("#F9F9F9");     // NavigationView content layer
inline const QColor surface("#FFFFFF");   // card fill
inline const QColor raised("#FAFAFA");    // card hover
inline const QColor hover("#F7F7F7");
inline const QColor selected("#F0F0F0");  // selected list row
inline const QColor inset("#F3F3F3");     // recessed tracks and tiles
inline const QColor stroke("#E5E5E5");    // CardStrokeColorDefault
inline const QColor divider("#EBEBEB");   // DividerStrokeColorDefault
inline const QColor flyout("#FAFAFA");    // menus, popups, tooltips
inline const QColor grid("#EDEDED");
// Text
inline const QColor text("#1B1B1B");      // TextFillColorPrimary
inline const QColor body("#1B1B1B");
inline const QColor secondary("#5E5E5E"); // TextFillColorSecondary
inline const QColor muted("#8A8A8A");     // TextFillColorTertiary
inline const QColor disabled("#A3A3A3");  // TextFillColorDisabled
// Neutral accent (no system blue)
inline const QColor accent("#1F1F1F");
inline const QColor accentHover("#353535");
inline const QColor accentPressed("#4C4C4C");
inline const QColor accentText("#1B1B1B");
inline const QColor onAccent("#FFFFFF");
// System status colors (light theme)
inline const QColor positive("#0F7B0F");  // SystemFillColorSuccess
inline const QColor negative("#C42B1C");  // SystemFillColorCritical
inline const QColor caution("#9D5D00");   // SystemFillColorCaution

QPalette palette();
QFont font(int pixelSize, int weight = QFont::Normal);
QFont displayFont(int pixelSize, int weight = QFont::DemiBold);
QFont iconFont(int pixelSize);
bool hasIconFont();
void drawGlyph(QPainter* painter, const QRectF& rect, char16_t glyph, const QColor& color,
               int pixelSize, Qt::Alignment alignment = Qt::AlignCenter);
// Glyph icon that stays crisp at fractional scale factors.
QIcon glyphIcon(char16_t glyph, const QColor& normal = secondary, const QColor& active = text);
QIcon appIcon();
void paintAppIcon(QPainter* painter, const QRectF& rect);
void installNativeFrames(QApplication& app);
}

// Segoe Fluent Icons code points (Segoe MDL2 Assets on Windows 10).
namespace Glyph {
inline constexpr char16_t Star = 0xE734, StarFill = 0xE735, Tasks = 0xE9D5, Trend = 0xEAFC,
    Home = 0xE80F, HomeFill = 0xEA8A, Pie = 0xEB05, History = 0xE81C, Settings = 0xE713,
    SettingsFill = 0xF8B0, Search = 0xE721, Clear = 0xE894, ChevronDown = 0xE70D,
    ChevronUp = 0xE70E, ChevronRight = 0xE76C, Add = 0xE710, Edit = 0xE70F, Delete = 0xE74D,
    Play = 0xE768, Pause = 0xE769, Import = 0xE8B5, Export = 0xEDE1, Download = 0xE896,
    Save = 0xE74E, Info = 0xE946, Folder = 0xE8B7, Link = 0xE71B, CheckMark = 0xE73E,
    Pulse = 0xE9D9, List = 0xEA37, Lightning = 0xE945, Stopwatch = 0xE916, Clock = 0xE823,
    Keyboard = 0xE765, Mouse = 0xE962, Tag = 0xE8EC, Display = 0xE7F4, Speed = 0xEC4A,
    Sync = 0xE895, Filter = 0xE71C, SkipTo = 0xE8B5, Help = 0xE9CE;
}
