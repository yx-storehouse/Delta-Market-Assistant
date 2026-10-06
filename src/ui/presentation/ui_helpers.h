#pragma once

#include <QColor>
#include <QFont>
#include <QPixmap>
#include <QStringList>
#include <QVector>

class AppState;
struct Skin;
struct Task;
class QWidget;
class QLabel;
class QPushButton;
class QFrame;
class QVBoxLayout;
class QHBoxLayout;
class QTableWidget;
class QTableWidgetItem;
class QDialog;
class QScrollArea;

// Shared, stateless presentation primitives. No application/controller ownership
// belongs here; pages own controls and MainWindow only composes those pages.
namespace relink::ui {
constexpr int SubtitleRole = Qt::UserRole + 1;
constexpr int ArtRole = Qt::UserRole + 2;
constexpr int SamplesRole = Qt::UserRole + 3;
constexpr int ToneRole = Qt::UserRole + 4;
constexpr int KindRole = Qt::UserRole + 10;
enum CellKind { TextCell, ItemCell, SparkCell, StatusCell };
enum Tone { ToneNeutral, ToneAccent, ToneSuccess, ToneCaution, ToneCritical, ToneInactive };
enum class RowStyle { Cards, Lines };
enum class ButtonKind { Standard, Accent, Subtle, Link };


QString workspaceText(const QString& value);
QString shortWorkspaceId(const QString& value);
QColor toneColor(int tone);
int taskTone(const Task& task);
int levelTone(const QString& level);
QLabel* label(const QString& text, const QString& name = QString());
QPushButton* button(const QString& text, const QString& name,
                    ButtonKind kind = ButtonKind::Standard, char16_t glyph = 0);
QPushButton* link(const QString& text, const QString& name);
QFrame* card(const QString& name);
QFrame* rule(const QString& name);
QVBoxLayout* pageLayout(QWidget* page);
QHBoxLayout* pageHeader(QVBoxLayout* layout, const QString& title, QLabel** caption = nullptr);
QHBoxLayout* sectionHeader(QVBoxLayout* layout, const QString& title);
bool numericHeading(const QString& title);
QFont numberFont();
QString amount(double value);
QString changeText(double change);
QString selectedId(QTableWidget* view);
const Skin* findSkin(const AppState* state, const QString& id);
QVector<double> series(double price);
QString skinCategory(const QString& name);
int skinArtIndex(const Skin& skin);
QPixmap skinArtwork(int index);
QString skinChoice(const Skin& skin);
QStringList conditionOptions(const AppState* state);
QTableWidget* table(const QStringList& columns, const QString& name, RowStyle style);
QTableWidgetItem* put(QTableWidget* table, int row, int column, const QString& text,
                      const QColor& color = QColor(), const QString& id = QString());
void putStatus(QTableWidget* table, int row, int column, const QString& text, int tone);
QWidget* centered(QWidget* control);
QLabel* glyphLabel(char16_t glyph);
QFrame* settingsCard(QWidget* icon, const QString& title, const QString& description, QWidget* trailing,
                     const QString& name = QString(), bool indented = false);
void fitDialog(QDialog& dialog, int width);
bool confirm(QWidget* parent, const QString& title, const QString& text, const QString& accept);
QScrollArea* scrollablePage(QWidget* page);
} // namespace relink::ui
