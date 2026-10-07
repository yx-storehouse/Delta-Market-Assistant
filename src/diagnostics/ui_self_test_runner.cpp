#include "diagnostics/ui_self_test_runner.h"
#include "application/startup_config.h"
#include "application/workspace/workspace_controller.h"
#include "application/workspace/workspace_ui_self_test.h"
#include "domain.h"
#include "fluenttheme.h"
#include "mainwindow.h"
#include "widgets.h"
#include <QApplication>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFontMetrics>
#include <QFrame>
#include <QHeaderView>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMetaProperty>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyleOptionComboBox>
#include <QTableWidget>
#include <QTimer>
#include <cmath>
#include <cstdio>

namespace {

// These checks use logical widget geometry, so they also work at non-integer
// device-pixel ratios. Vertical scrolling is intentional and is not a failure.
static QJsonArray sizeJson(const QSize& size) {
    return {size.width(), size.height()};
}

static QJsonArray rectJson(const QRect& rect) {
    return {rect.x(), rect.y(), rect.width(), rect.height()};
}

static bool insideItemView(const QWidget* widget) {
    for (auto* parent = widget->parentWidget(); parent; parent = parent->parentWidget())
        if (qobject_cast<const QAbstractItemView*>(parent)) return true;
    return false;
}

static bool isLayoutControl(const QWidget* widget) {
    return qobject_cast<const QPushButton*>(widget) || qobject_cast<const QLineEdit*>(widget)
        || qobject_cast<const QComboBox*>(widget) || qobject_cast<const QCheckBox*>(widget)
        || qobject_cast<const QSpinBox*>(widget) || qobject_cast<const QDoubleSpinBox*>(widget);
}

static QJsonArray overlappingWidgets(const QList<QWidget*>& widgets, QWidget* reference) {
    QJsonArray overlaps;
    for (qsizetype i = 0; i < widgets.size(); ++i) {
        for (qsizetype j = i + 1; j < widgets.size(); ++j) {
            const auto* first = widgets[i];
            const auto* second = widgets[j];
            if (first->isAncestorOf(second) || second->isAncestorOf(first)) continue;
            const QRect firstRect(first->mapTo(reference, QPoint()), first->size());
            const QRect secondRect(second->mapTo(reference, QPoint()), second->size());
            const QRect overlap = firstRect.intersected(secondRect);
            if (overlap.width() > 1 && overlap.height() > 1) {
                overlaps.append(QJsonObject{{"first", first->objectName()}, {"second", second->objectName()},
                                            {"intersection", rectJson(overlap)}});
            }
        }
    }
    return overlaps;
}

static QJsonArray inspectUiText(QWidget* root) {
    QJsonArray issues;
    auto inspect = [&](const QString& object, const QString& field, const QString& text) {
        if (text.contains(QStringLiteral("??")) || text.contains(QChar::ReplacementCharacter))
            issues.append(QJsonObject{{"object", object}, {"field", field}, {"text", text}});
    };
    QList<QObject*> objects = root->findChildren<QObject*>();
    objects.prepend(root);
    for (const auto* object : objects) {
        const QString name = object->objectName().isEmpty()
            ? QString::fromLatin1(object->metaObject()->className()) : object->objectName();
        for (int i = 0; i < object->metaObject()->propertyCount(); ++i) {
            const QMetaProperty property = object->metaObject()->property(i);
            if (property.isReadable() && property.metaType().id() == QMetaType::QString)
                inspect(name, QString::fromLatin1(property.name()), property.read(object).toString());
        }
        if (const auto* combo = qobject_cast<const QComboBox*>(object)) {
            for (int i = 0; i < combo->count(); ++i)
                inspect(name, QString("item[%1]").arg(i), combo->itemText(i));
        }
        if (const auto* table = qobject_cast<const QTableWidget*>(object)) {
            for (int column = 0; column < table->columnCount(); ++column) {
                if (const auto* header = table->horizontalHeaderItem(column))
                    inspect(name, QString("header[%1]").arg(column), header->text());
                for (int row = 0; row < table->rowCount(); ++row) {
                    if (const auto* item = table->item(row, column)) {
                        inspect(name, QString("cell[%1,%2]").arg(row).arg(column), item->text());
                        inspect(name, QString("tooltip[%1,%2]").arg(row).arg(column), item->toolTip());
                    }
                }
            }
        }
    }
    return issues;
}

static QJsonObject inspectPageGeometry(QWidget* container, QWidget* window) {
    auto* scroll = qobject_cast<QScrollArea*>(container);
    QWidget* page = scroll ? scroll->widget() : container;
    QWidget* viewport = scroll ? scroll->viewport() : container;
    if (!page || !viewport)
        return {{"horizontal_fit", false}, {"table_columns_fit", false},
                {"controls_no_overlap", false}, {"header_no_overlap", false}};
    QJsonArray widgets;
    QJsonArray tables;
    QList<QWidget*> controls;
    bool horizontalFit = page->width() <= viewport->width() + 1;
    bool tablesFit = true;
    bool cellControlsFit = true;
    const auto candidates = page->findChildren<QWidget*>();
    for (auto* widget : candidates) {
        if (!widget->isVisibleTo(page) || widget->objectName().isEmpty()
            || widget->objectName().startsWith("qt_") || insideItemView(widget)) continue;
        const QRect rect(widget->mapTo(viewport, QPoint()), widget->size());
        const bool fits = rect.left() >= -1 && rect.right() < viewport->width() + 1;
        horizontalFit &= fits;
        QJsonObject entry{{"object", widget->objectName()}, {"class", widget->metaObject()->className()},
                          {"viewport_rect", rectJson(rect)}, {"size_hint", sizeJson(widget->sizeHint())},
                          {"minimum_size_hint", sizeJson(widget->minimumSizeHint())},
                          {"minimum_size", sizeJson(widget->minimumSize())}, {"horizontal_fit", fits}};
        if (auto* label = qobject_cast<QLabel*>(widget)) entry.insert("text", label->text());
        if (auto* button = qobject_cast<QAbstractButton*>(widget)) entry.insert("text", button->text());
        widgets.append(entry);
        if (isLayoutControl(widget)) controls.append(widget);
        if (auto* table = qobject_cast<QTableWidget*>(widget)) {
            const int columnsWidth = table->horizontalHeader()->length();
            const int availableWidth = table->viewport()->width();
            const bool columnsFit = columnsWidth <= availableWidth + 1
                && table->horizontalScrollBar()->maximum() <= 1;
            tablesFit &= columnsFit;
            QJsonArray cellControls;
            int attachedCheckBoxes = 0;
            for (int row = 0; row < table->rowCount(); ++row) {
                for (int column = 0; column < table->columnCount(); ++column) {
                    auto* cell = table->cellWidget(row, column);
                    if (!cell) continue;
                    for (auto* checkBox : cell->findChildren<QCheckBox*>()) {
                        ++attachedCheckBoxes;
                        QStyleOptionButton option;
                        option.initFrom(checkBox);
                        option.state |= checkBox->isChecked() ? QStyle::State_On : QStyle::State_Off;
                        const QRect indicator = checkBox->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, checkBox);
                        const QRect inCell(checkBox->mapTo(cell, QPoint()), checkBox->size());
                        const bool fits = checkBox->rect().contains(indicator) && cell->rect().contains(inCell);
                        cellControlsFit &= fits;
                        cellControls.append(QJsonObject{{"object", checkBox->objectName()}, {"row", row}, {"column", column},
                            {"cell_size", sizeJson(cell->size())}, {"rect_in_cell", rectJson(inCell)},
                            {"size_hint", sizeJson(checkBox->sizeHint())},
                            {"minimum_size_hint", sizeJson(checkBox->minimumSizeHint())},
                            {"indicator_rect", rectJson(indicator)}, {"unclipped", fits}});
                    }
                }
            }
            const qsizetype actualCheckBoxes = table->findChildren<QCheckBox*>().size();
            const bool noStaleCellWidgets = attachedCheckBoxes == actualCheckBoxes;
            cellControlsFit &= noStaleCellWidgets;
            tables.append(QJsonObject{{"object", table->objectName()}, {"columns", table->columnCount()},
                                      {"rows", table->rowCount()}, {"columns_width", columnsWidth},
                                      {"viewport_width", availableWidth},
                                      {"horizontal_scroll_max", table->horizontalScrollBar()->maximum()},
                                      {"vertical_scroll_max", table->verticalScrollBar()->maximum()},
                                      {"all_columns_visible", columnsFit}, {"attached_checkboxes", attachedCheckBoxes},
                                      {"actual_checkboxes", static_cast<int>(actualCheckBoxes)},
                                      {"no_stale_cell_widgets", noStaleCellWidgets}, {"cell_controls", cellControls}});
        }
    }
    QList<QWidget*> headerWidgets;
    QJsonArray headerGeometry;
    bool headerFit = true;
    for (const auto& name : {"pageTitle", "pageCaption", "demoBadge", "helpButton",
                             "importConfigButton", "exportConfigButton"}) {
        auto* widget = window->findChild<QWidget*>(name);
        if (!widget || !widget->isVisibleTo(window)) continue;
        headerWidgets.append(widget);
        const QRect rect(widget->mapTo(window, QPoint()), widget->size());
        headerFit &= window->rect().contains(rect);
        headerGeometry.append(QJsonObject{{"object", name}, {"window_rect", rectJson(rect)},
                                         {"size_hint", sizeJson(widget->sizeHint())}});
    }
    const QJsonArray controlOverlaps = overlappingWidgets(controls, page);
    const QJsonArray headerOverlaps = overlappingWidgets(headerWidgets, window);
    QJsonArray directPanels;
    for (auto* panel : page->findChildren<QFrame*>(QString(), Qt::FindDirectChildrenOnly)) {
        QJsonObject entry{{"object", panel->objectName()}, {"geometry", rectJson(panel->geometry())},
                          {"size_hint", sizeJson(panel->sizeHint())}, {"minimum_size_hint", sizeJson(panel->minimumSizeHint())},
                          {"minimum_size", sizeJson(panel->minimumSize())}};
        if (panel->layout()) {
            entry.insert("layout_minimum_size", sizeJson(panel->layout()->minimumSize()));
            entry.insert("layout_size_hint", sizeJson(panel->layout()->sizeHint()));
        }
        directPanels.append(entry);
    }
    return {{"page", page->objectName()}, {"window_size", sizeJson(window->size())},
            {"viewport_size", sizeJson(viewport->size())}, {"page_size", sizeJson(page->size())},
            {"page_minimum_size", sizeJson(page->minimumSize())},
            {"page_minimum_size_hint", sizeJson(page->minimumSizeHint())},
            {"page_layout_minimum_size", sizeJson(page->layout() ? page->layout()->minimumSize() : QSize())},
            {"page_layout_size_hint", sizeJson(page->layout() ? page->layout()->sizeHint() : QSize())},
            {"vertical_scroll_value", scroll ? scroll->verticalScrollBar()->value() : 0},
            {"vertical_scroll_max", scroll ? scroll->verticalScrollBar()->maximum() : 0},
            {"horizontal_scroll_max", scroll ? scroll->horizontalScrollBar()->maximum() : 0},
            {"direct_panels", directPanels},
            {"device_pixel_ratio", window->devicePixelRatioF()},
            {"horizontal_fit", horizontalFit}, {"table_columns_fit", tablesFit},
            {"table_cell_controls_fit", cellControlsFit},
            {"controls_no_overlap", controlOverlaps.isEmpty()},
            {"header_no_overlap", headerFit && headerOverlaps.isEmpty()},
            {"widgets", widgets}, {"tables", tables}, {"header", headerGeometry},
            {"control_overlaps", controlOverlaps}, {"header_overlaps", headerOverlaps}};
}

} // namespace

namespace relink::diagnostics {

int runUiSelfTest(QApplication& app, MainWindow& window, AppState& state,
                  workspace::WorkspaceController& workspace, const UiSelfTestOptions& options) {
    const QString config = options.configPath;
    const QString output = options.snapshotDirectory;
    const bool runInteractionChecks = options.runInteractionChecks;
    QDir().mkpath(output);
    QTimer::singleShot(100, &window, [&app, &window, &state, &workspace, config, output, runInteractionChecks]() {
        bool success = true;
        QJsonArray checks;
        QJsonArray geometries;
        auto check = [&](bool ok, const char* name) {
            checks.append(QJsonObject{{"check", name}, {"passed", ok}});
            std::printf("UI_%s=%s\n", name, ok ? "PASS" : "FAIL");
            success &= ok;
        };
        auto flush = [&]() {
            // The complete interaction test runs inside one timer callback.
            // processEvents() alone does not complete deleteLater() callbacks:
            // replaced table cell widgets can otherwise survive in snapshots.
            for (int pass = 0; pass < 3; ++pass) {
                QApplication::processEvents();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
                if (window.layout()) window.layout()->activate();
                for (auto* widget : window.findChildren<QWidget*>())
                    if (widget->layout()) widget->layout()->activate();
            }
            window.repaint();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
            QApplication::processEvents();
        };
        const QStringList nav = {"navOverview", "navFavorites", "navTasks", "navPrices", "navStats", "navLogs", "navSettings", "navRun"};
        const QStringList pages = {"overview", "favorites", "tasks", "prices", "stats", "logs", "settings", "run"};
        auto* stack = window.findChild<QStackedWidget*>("pageStack");
        // Functional checks alone did not catch a historical UTF-8/GBK text
        // corruption. Verify the visible labels as well as widget identities.
        const QStringList navText = {QStringLiteral("工作台"), QStringLiteral("关注"), QStringLiteral("任务"),
            QStringLiteral("价格"), QStringLiteral("统计"), QStringLiteral("日志"), QStringLiteral("设置"), QStringLiteral("运行")};
        bool navigationTextValid = true;
        for (int i = 0; i < nav.size(); ++i) {
            const auto* control = window.findChild<QAbstractButton*>(nav[i]);
            navigationTextValid &= control && control->text() == navText[i];
        }
        check(navigationTextValid, "NAVIGATION_TEXT_UTF8");
        const auto textEquals = [&](const char* name, const QString& expected) {
            const auto* control = window.findChild<QAbstractButton*>(name);
            return control && control->text() == expected;
        };
        check(textEquals("importConfigButton", QStringLiteral("导入"))
              && textEquals("exportConfigButton", QStringLiteral("导出"))
              && textEquals("saveRunSettingsButton", QStringLiteral("保存参数")), "ACTION_TEXT_UTF8");
        const QStringList damagedText = {QStringLiteral("鎴戠"), QStringLiteral("鍏虫"), QStringLiteral("杩愯"),
            QStringLiteral("瀵煎"), QStringLiteral("璐"), QStringLiteral("閰嶇"), QStringLiteral("淇濆"),
            QStringLiteral("鐢熸"), QStringLiteral("缁熻"), QString(QChar(0xfffd))};
        bool visibleTextValid = true;
        for (auto* widget : window.findChildren<QWidget*>()) {
            QString displayed;
            if (auto* label = qobject_cast<QLabel*>(widget)) displayed = label->text();
            else if (auto* control = qobject_cast<QAbstractButton*>(widget)) displayed = control->text();
            else if (auto* edit = qobject_cast<QLineEdit*>(widget)) displayed = edit->placeholderText();
            for (const auto& damaged : damagedText) visibleTextValid &= !displayed.contains(damaged);
        }
        check(visibleTextValid, "NO_MOJIBAKE_LABELS");
        auto inspectGeometry = [&](int pageIndex, const QString& sizeName) {
            QJsonObject geometry = inspectPageGeometry(stack ? stack->currentWidget() : nullptr, &window);
            geometry.insert("render_size", sizeName);
            geometry.insert("snapshot", pages[pageIndex] + (sizeName == "compact" ? "_compact.png" : ".png"));
            geometries.append(geometry);
            const QByteArray prefix = (sizeName + "_" + pages[pageIndex]).toUpper().toUtf8();
            check(geometry.value("horizontal_fit").toBool(), (prefix + "_HORIZONTAL_FIT").constData());
            check(geometry.value("table_columns_fit").toBool(), (prefix + "_TABLE_COLUMNS").constData());
            check(geometry.value("table_cell_controls_fit").toBool(), (prefix + "_TABLE_CELL_CONTROLS").constData());
            check(geometry.value("controls_no_overlap").toBool(), (prefix + "_CONTROLS_NO_OVERLAP").constData());
            check(geometry.value("header_no_overlap").toBool(), (prefix + "_HEADER_NO_OVERLAP").constData());
        };
        // Samples a logical point of a widget inside a full-window render.
        auto sample = [&](const QImage& image, qreal dpr, QWidget* widget, const QPointF& point) {
            const QPointF mapped = QPointF(widget->mapTo(&window, QPoint(0, 0))) + point;
            return image.pixelColor(qBound(0, qRound(mapped.x() * dpr), image.width() - 1),
                                    qBound(0, qRound(mapped.y() * dpr), image.height() - 1));
        };
        auto mostlyLight = [](QWidget* widget) {
            if (!widget) return false;
            QWidget* top = widget->window();
            const QImage pixels = top->grab(QRect(widget->mapTo(top, QPoint()), widget->size())).toImage();
            int light = 0, total = 0;
            for (int y = 4; y < pixels.height(); y += 12) {
                for (int x = 4; x < pixels.width(); x += 12) {
                    const QColor color = pixels.pixelColor(x, y);
                    light += qMin(color.red(), qMin(color.green(), color.blue())) > 200;
                    ++total;
                }
            }
            return total > 0 && light > total * 0.80;
        };
        // The user asked for the neutral Windows white / gray look without blue.
        auto blueShare = [](const QImage& image) {
            qint64 blue = 0, total = 0;
            for (int y = 0; y < image.height(); y += 2) {
                for (int x = 0; x < image.width(); x += 2) {
                    const QColor color = image.pixelColor(x, y);
                    blue += color.blue() - qMax(color.red(), color.green()) > 30;
                    ++total;
                }
            }
            return total ? double(blue) / total : 1.0;
        };
        check(stack && stack->count() == 8, "EIGHT_PAGES");
        check(stack && stack->currentIndex() == 1, "DEFAULT_WATCHLIST");
        check(window.findChild<QFrame*>("catalogFilterBar") && window.findChild<QFrame*>("terminalInspector")
              && window.findChild<QFrame*>("watchlistChartPanel"), "WATCHLIST_STRUCTURE");
        check(window.findChild<QFrame*>("titleBar") && window.findChild<QFrame*>("navRail")
              && window.findChild<QFrame*>("contentLayer"), "STORE_SHELL");
        const QImage artwork(":/assets/demo_skin_atlas.png");
        check(artwork.isNull(), "ART_RESOURCE_REMOVED");
        check(!QFile::exists(":/assets/demo_skin_atlas.png"), "NO_DEMO_THUMBNAILS");
        check(FluentTheme::hasIconFont(), "ICON_FONT_AVAILABLE");
        flush();
        check(app.palette().color(QPalette::Window) == FluentTheme::window
              && app.palette().color(QPalette::Base) == FluentTheme::surface, "LIGHT_PALETTE");
        auto* preview = window.findChild<ArtworkView*>("skinPreview");
        check(preview && !preview->hasArtwork(), "PREVIEW_FITS");
        auto* workSurface = window.findChild<QWidget*>("watchlistWorkspace");
        auto* detailSurface = window.findChild<QFrame*>("terminalInspector");
        check(workSurface && detailSurface && detailSurface->geometry().left() - workSurface->geometry().right() >= 12,
              "SEPARATE_SURFACES");
        {
            const QPixmap shot = window.grab();
            const QImage image = shot.toImage();
            const qreal dpr = shot.devicePixelRatio();
            auto* list = window.findChild<QTableWidget*>("favoriteTable");
            bool cardRows = false;
            if (list && list->rowCount() > 1) {
                QWidget* view = list->viewport();
                const qreal x = list->columnViewportPosition(3) + 12;
                const qreal selectedY = list->rowViewportPosition(0) + list->rowHeight(0) / 2.0;
                const QColor selectedFill = sample(image, dpr, view, QPointF(x, selectedY));
                const QColor otherFill = sample(image, dpr, view, QPointF(x, list->rowViewportPosition(1) + 8));
                const QColor gap = sample(image, dpr, view, QPointF(x, list->rowViewportPosition(1) + 0.5));
                const QColor pill = sample(image, dpr, view, QPointF(list->columnViewportPosition(0) + 6, selectedY));
                cardRows = otherFill.lightness() >= 250 && selectedFill.lightness() < otherFill.lightness()
                    && gap.lightness() < otherFill.lightness() && pill.lightness() < 80;
            }
            check(cardRows, "CARD_ROWS_AND_SELECTION_RENDER");
            auto* layer = window.findChild<QFrame*>("contentLayer");
            const QColor corner = layer ? sample(image, dpr, layer, QPointF(1, 1)) : QColor();
            const QColor inside = layer ? sample(image, dpr, layer, QPointF(20, 20)) : QColor();
            check(layer && qAbs(corner.lightness() - FluentTheme::window.lightness()) <= 3
                  && qAbs(inside.lightness() - FluentTheme::layer.lightness()) <= 3, "CONTENT_LAYER_ROUNDED_CORNER");
            auto* favoritesNav = window.findChild<QPushButton*>("navFavorites");
            const QColor railPill = favoritesNav ? sample(image, dpr, favoritesNav, QPointF(1.5, favoritesNav->height() / 2.0)) : QColor();
            const QColor railPlate = favoritesNav ? sample(image, dpr, favoritesNav, QPointF(14, 8)) : QColor();
            check(favoritesNav && favoritesNav->isChecked() && railPill.lightness() < 80 && railPlate.lightness() >= 250,
                  "RAIL_SELECTION_RENDER");
        }
        bool comboTextFits = true;
        for (const auto& name : {"favoriteRarity", "favoriteTaskFilter", "inspectorCondition"}) {
            auto* combo = window.findChild<QComboBox*>(name);
            if (!combo) { comboTextFits = false; continue; }
            QStyleOptionComboBox option;
            option.initFrom(combo);
            option.currentText = combo->currentText();
            option.editable = combo->isEditable();
            const QRect textRect = combo->style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, combo);
            const bool thisComboFits = combo->fontMetrics().horizontalAdvance(combo->currentText()) <= textRect.width();
            comboTextFits &= thisComboFits;
        }
        check(comboTextFits, "FILTER_TEXT_FITS");
        auto contrast = [](const QColor& a, const QColor& b) {
            const auto luminance = [](const QColor& color) {
                const auto linear = [](double value) {
                    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
                };
                return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
            };
            const double x = luminance(a), y = luminance(b);
            return (qMax(x, y) + 0.05) / (qMin(x, y) + 0.05);
        };
        check(contrast(FluentTheme::text, FluentTheme::surface) >= 10.0, "MAIN_TEXT_CONTRAST");
        check(contrast(FluentTheme::secondary, FluentTheme::surface) >= 4.5
              && contrast(FluentTheme::secondary, FluentTheme::layer) >= 4.5, "SECONDARY_TEXT_CONTRAST");
        check(contrast(FluentTheme::muted, FluentTheme::surface) >= 3.0, "TERTIARY_TEXT_CONTRAST");
        check(contrast(FluentTheme::text, FluentTheme::selected) >= 10.0, "SELECTED_TEXT_CONTRAST");
        check(contrast(FluentTheme::onAccent, FluentTheme::accent) >= 4.5, "PRIMARY_TEXT_CONTRAST");
        auto navigationFits = [&]() {
            QList<QWidget*> controls;
            bool fits = true;
            for (const auto& name : nav) {
                auto* button = window.findChild<QPushButton*>(name);
                if (!button) { fits = false; continue; }
                const QRect rect(button->mapTo(&window, QPoint()), button->size());
                fits &= window.rect().contains(rect);
                fits &= QFontMetrics(FluentTheme::font(12)).horizontalAdvance(button->text()) + 4 <= button->width();
                controls.append(button);
            }
            for (const auto& name : {"favoriteSearch", "importConfigButton", "exportConfigButton", "helpButton", "demoBadge"}) {
                auto* widget = window.findChild<QWidget*>(name);
                if (!widget) { fits = false; continue; }
                fits &= window.rect().contains(QRect(widget->mapTo(&window, QPoint()), widget->size()));
                controls.append(widget);
            }
            return fits && overlappingWidgets(controls, &window).isEmpty();
        };
        auto searchCentered = [&]() {
            auto* search = window.findChild<QWidget*>("favoriteSearch");
            if (!search) return false;
            const int center = search->mapTo(&window, QPoint()).x() + search->width() / 2;
            return qAbs(center - window.width() / 2) <= 2;
        };
        auto inspectorFits = [&]() {
            auto* content = window.findChild<QWidget*>("inspectorContent");
            if (!content) return false;
            QList<QWidget*> elements;
            for (auto* widget : content->findChildren<QWidget*>()) {
                if (qobject_cast<QLabel*>(widget) || qobject_cast<QPushButton*>(widget) || qobject_cast<QComboBox*>(widget)
                    || qobject_cast<QSpinBox*>(widget) || qobject_cast<QDoubleSpinBox*>(widget))
                    elements.append(widget);
            }
            return overlappingWidgets(elements, content).isEmpty();
        };
        check(navigationFits(), "STANDARD_NAVIGATION_FITS");
        check(searchCentered(), "STANDARD_SEARCH_CENTERED");
        check(inspectorFits(), "STANDARD_INSPECTOR_NO_OVERLAP");
        check(mostlyLight(window.findChild<QWidget*>("favoritePriceChart")), "LIGHT_CHART_RENDER");
        check(mostlyLight(workSurface) && mostlyLight(detailSurface), "LIGHT_CONTENT_SURFACES_RENDER");
        auto* lightSearch = window.findChild<QLineEdit*>("favoriteSearch");
        if (lightSearch) {
            lightSearch->setFocus(Qt::TabFocusReason);
            flush();
            const QPixmap pixels = lightSearch->grab();
            const qreal dpr = pixels.devicePixelRatio();
            const QColor bottom = pixels.toImage().pixelColor(qRound(lightSearch->width() / 2.0 * dpr),
                                                              qRound((lightSearch->height() - 1.5) * dpr));
            check(bottom.lightness() < 90, "INPUT_FOCUS_UNDERLINE");
            pixels.save(output + "/search_focus.png");
            lightSearch->clearFocus();
            flush();
        } else check(false, "INPUT_FOCUS_UNDERLINE");
        auto* lightCombo = window.findChild<QComboBox*>("favoriteRarity");
        if (lightCombo) {
            lightCombo->showPopup();
            flush();
            check(mostlyLight(lightCombo->view()), "DROPDOWN_RENDER");
            lightCombo->view()->window()->grab().save(output + "/rarity_dropdown.png");
            lightCombo->hidePopup();
            flush();
        } else check(false, "DROPDOWN_RENDER");
        auto* createButton = window.findChild<QPushButton*>("favoriteCreateTaskButton");
        if (createButton) {
            const bool originallyEnabled = createButton->isEnabled();
            createButton->setEnabled(true);
            flush();
            const QPixmap enabledPixels = createButton->grab();
            createButton->setEnabled(false);
            flush();
            const QPixmap disabledPixels = createButton->grab();
            const qreal dpr = disabledPixels.devicePixelRatio();
            const QPoint point(qRound(createButton->width() / 2.0 * dpr), qRound(5 * dpr));
            const QColor before = enabledPixels.toImage().pixelColor(point);
            const QColor after = disabledPixels.toImage().pixelColor(point);
            check(qAbs(before.lightness() - after.lightness()) > 50, "DISABLED_CONTROL_DISTINCT");
            disabledPixels.save(output + "/create_disabled.png");
            createButton->setEnabled(originallyEnabled);
            flush();
        } else check(false, "DISABLED_CONTROL_DISTINCT");
        for (int i = 0; i < nav.size(); ++i) {
            auto* btn = window.findChild<QPushButton*>(nav[i]);
            if (btn) btn->click();
            flush();
            const bool pageOk = btn && stack && stack->currentIndex() == i && btn->isChecked();
            check(pageOk, QByteArray("NAV_" + pages[i].toUpper().toUtf8()).constData());
            check(mostlyLight(&window), QByteArray("LIGHT_PAGE_" + pages[i].toUpper().toUtf8()).constData());
            const QPixmap shot = window.grab();
            check(shot.save(output + "/" + pages[i] + ".png"), QByteArray("SNAPSHOT_" + pages[i].toUpper().toUtf8()).constData());
            if (pages[i] != "favorites" && pages[i] != "overview" && pages[i] != "prices")
                check(blueShare(shot.toImage()) < 0.002, QByteArray("NO_BLUE_" + pages[i].toUpper().toUtf8()).constData());
            if (auto* scroll = stack ? qobject_cast<QScrollArea*>(stack->currentWidget()) : nullptr)
                if (pages[i] == "run" || pages[i] == "settings") scroll->widget()->grab().save(output + "/" + pages[i] + "_full.png");
            inspectGeometry(i, "standard");
        }
        bool legacyHeroRemoved = true;
        for (auto* label : window.findChildren<QLabel*>()) {
            legacyHeroRemoved &= label->objectName() != "heroTitle" && label->objectName() != "heroDescription"
                && !label->text().contains("YOUR WATCHLIST. YOUR WORKFLOW.")
                && !label->text().contains(QStringLiteral("把关注，变成有序的自动任务"));
        }
        check(legacyHeroRemoved, "LEGACY_HERO_REMOVED");
        QJsonArray textEncodingIssues = inspectUiText(&window);
        check(textEncodingIssues.isEmpty(), "INITIAL_UI_TEXT_ENCODING");
        if (runInteractionChecks) {
            auto* replaySource = window.findChild<QLabel*>("replaySourceLabel");
            auto* replayState = window.findChild<QLabel*>("replayStateLabel");
            auto* replayStart = window.findChild<QPushButton*>("replayStartButton");
            auto* replayPause = window.findChild<QPushButton*>("replayPauseButton");
            auto* replayResume = window.findChild<QPushButton*>("replayResumeButton");
            auto* replayStop = window.findChild<QPushButton*>("replayStopButton");
            auto* workspaceProfiles = window.findChild<QComboBox*>("workspaceProfileCombo");
            if (workspaceProfiles)
                workspaceProfiles->setCurrentIndex(workspaceProfiles->findData(relink::workspace::WorkspaceController::builtinProfileId()));
            flush();
            check(replaySource && replayState && replayStart && replayPause && replayResume && replayStop
                      && replaySource->text().contains(QStringLiteral("回放"))
                      && replaySource->toolTip().contains(QStringLiteral("Replay"))
                      && replayState->property("status").toString() == QStringLiteral("Ready")
                      && replayStart->isEnabled() && !replayPause->isEnabled()
                      && !replayResume->isEnabled() && !replayStop->isEnabled(),
                  "REPLAY_UI_INITIAL");
            if (replayStart) replayStart->click();
            flush();
            check(replayState && replayState->property("status").toString() != QStringLiteral("Ready")
                      && replayPause && replayPause->isEnabled(), "REPLAY_UI_START");
            if (replayPause) replayPause->click();
            flush();
            check(replayState && replayState->property("status").toString() == QStringLiteral("Paused")
                      && replayResume && replayResume->isEnabled(), "REPLAY_UI_PAUSE");
            if (replayResume) replayResume->click();
            flush();
            check(replayState && replayState->property("status").toString() != QStringLiteral("Paused"), "REPLAY_UI_RESUME");
            if (replayStop) replayStop->click();
            flush();
            check(replayState && replayState->property("status").toString() == QStringLiteral("Stopped")
                      && replayStop && !replayStop->isEnabled(), "REPLAY_UI_STOP");
        }
        if (runInteractionChecks) {
            check(workspace.setMode(QStringLiteral("Demo")), "WORKSPACE_DEMO_MODE");
            window.setPage(1);
            flush();
            auto* search = window.findChild<QLineEdit*>("favoriteSearch");
            auto* table = window.findChild<QTableWidget*>("favoriteTable");
            check(search && table && table->rowCount() == state.skins.size(), "FAVORITE_ROWS");
            if (search && table) {
                search->setText("AUG");
                flush();
                check(table->rowCount() == 1, "FAVORITE_SEARCH");
                search->setText("NO_SUCH_SYNTHETIC_ITEM");
                flush();
                check(table->rowCount() == 0, "FAVORITE_EMPTY_SEARCH");
                search->clear();
                flush();
                window.setPage(2);
                flush();
                search->setText("AUG");
                flush();
                check(stack && stack->currentIndex() == 1 && table->rowCount() == 1, "TOP_SEARCH_NAVIGATES");
                search->clear();
                flush();
            }
            auto* follow = window.findChild<QCheckBox*>("follow_demo-01");
            if (follow) follow->click();
            flush();
            check(follow && !state.skins.first().followed, "FOLLOW_TOGGLE");
            state.skins.first().followed = true;
            state.notifyChanged();
            flush();
            auto* rifles = window.findChild<QPushButton*>("catalogFilter_rifle");
            auto* notFollowed = window.findChild<QPushButton*>("catalogFilter_unfollowed");
            auto* allItems = window.findChild<QPushButton*>("catalogFilter_all");
            if (rifles) rifles->click();
            flush();
            check(rifles && table && table->rowCount() == 5, "TYPE_FILTER");
            if (notFollowed) notFollowed->click();
            flush();
            check(notFollowed && table && table->rowCount() == 2, "FOLLOW_FILTER");
            if (allItems) allItems->click();
            flush();
            check(allItems && table && table->rowCount() == 12, "CLEAR_FILTER");
            auto* quality = window.findChild<QComboBox*>("favoriteRarity");
            if (quality) quality->setCurrentText(QStringLiteral("橙色"));
            flush();
            check(quality && table && table->rowCount() == 5, "RARITY_FILTER");
            if (quality) quality->setCurrentIndex(0);
            auto* linkedFilter = window.findChild<QComboBox*>("favoriteTaskFilter");
            if (linkedFilter) linkedFilter->setCurrentIndex(1);
            flush();
            check(linkedFilter && table && table->rowCount() == 5, "LINKED_FILTER");
            if (linkedFilter) linkedFilter->setCurrentIndex(2);
            flush();
            check(linkedFilter && table && table->rowCount() == 7, "UNLINKED_FILTER");
            if (linkedFilter) linkedFilter->setCurrentIndex(0);
            flush();
            if (table) table->selectRow(0);
            flush();
            auto* inlinePrice = window.findChild<QDoubleSpinBox*>("inspectorMaxPrice");
            auto* inlineWear = window.findChild<QDoubleSpinBox*>("inspectorMaxWear");
            auto* inlineQuantity = window.findChild<QSpinBox*>("inspectorQuantity");
            auto* inlineCondition = window.findChild<QComboBox*>("inspectorCondition");
            auto* inlineCreate = window.findChild<QPushButton*>("favoriteCreateTaskButton");
            check(inlinePrice && inlineWear && inlineQuantity && inlinePrice->value() == 650
                  && inlineWear->value() == 5 && inlineQuantity->value() == 3, "INSPECTOR_DEFAULTS");
            check(inlineCondition && inlineCondition->currentText() == QStringLiteral("成色S"), "INSPECTOR_CONDITION_DEFAULT");
            const int beforeInline = state.tasks.size();
            if (inlinePrice && inlineWear && inlineQuantity && inlineCondition && inlineCreate) {
                inlinePrice->setValue(680);
                inlineWear->setValue(4.125);
                inlineQuantity->setValue(2);
                inlineCondition->setCurrentText(QStringLiteral("成色A"));
                inlineCreate->click();
                flush();
            }
            const bool inlineCreated = state.tasks.size() == beforeInline + 1;
            check(inlineCreated, "INLINE_CREATE");
            check(inlineCreated && state.tasks.last().maxPrice == 680 && state.tasks.last().maxWear == 4.125
                  && state.tasks.last().quantity == 2 && state.tasks.last().skinId == "demo-01"
                  && state.tasks.last().condition == QStringLiteral("成色A") && state.tasks.last().enabled, "INLINE_VALUES");
            if (inlineCreated) { state.tasks.removeLast(); state.notifyChanged(); flush(); }
            if (inlinePrice) inlinePrice->setValue(777);
            state.notifyChanged();
            flush();
            check(inlinePrice && inlinePrice->value() == 777, "DRAFT_SURVIVES_REFRESH");
            if (table) table->selectRow(1);
            flush();
            check(inlinePrice && inlinePrice->value() == 450, "SELECTION_RELOADS_CONDITIONS");
            if (search) search->setText("NO_MATCHING_ITEM");
            flush();
            check(inlineCreate && !inlineCreate->isEnabled() && inlinePrice && !inlinePrice->isEnabled()
                  && inlineCondition && !inlineCondition->isEnabled(), "EMPTY_DISALLOWS_CREATE");
            if (search) search->clear();
            flush();
            window.setPage(2);
            flush();
            auto* taskTable = window.findChild<QTableWidget*>("taskTable");
            check(taskTable && taskTable->horizontalHeaderItem(2)->text() == QStringLiteral("成色") && taskTable->item(0, 2)
                  && taskTable->item(0, 2)->text() == QStringLiteral("成色S"), "TASK_CONDITION_COLUMN");
            const int before = state.tasks.size();
            auto* create = window.findChild<QPushButton*>("taskNewButton");
            if (create) {
                QTimer::singleShot(0, [&]() {
                    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    if (!dialog) return;
                    auto* name = dialog->findChild<QLineEdit*>("taskName");
                    auto* save = dialog->findChild<QPushButton*>("taskSaveButton");
                    if (name && save) {
                        save->click();
                        check(dialog->isVisible(), "TASK_EMPTY_NAME_REJECTED");
                        name->setText(QStringLiteral("离屏测试任务"));
                        auto* minimum = dialog->findChild<QDoubleSpinBox*>("taskMinPrice");
                        auto* maximum = dialog->findChild<QDoubleSpinBox*>("taskMaxPrice");
                        if (minimum && maximum) {
                            minimum->setValue(900);
                            maximum->setValue(100);
                            save->click();
                            check(dialog->isVisible(), "TASK_PRICE_RANGE_REJECTED");
                            minimum->setValue(10);
                            maximum->setValue(800);
                        }
                        if (auto* condition = dialog->findChild<QComboBox*>("taskCondition"))
                            condition->setCurrentText(QStringLiteral("成色A"));
                        auto* validation = dialog->findChild<QLabel*>("taskValidationLabel");
                        if (validation) validation->clear();
                        const QJsonArray dialogTextIssues = inspectUiText(dialog);
                        for (const auto& issue : dialogTextIssues) textEncodingIssues.append(issue);
                        check(dialogTextIssues.isEmpty(), "TASK_EDITOR_TEXT_ENCODING");
                        check(mostlyLight(dialog), "LIGHT_TASK_EDITOR_RENDER");
                        dialog->repaint();
                        dialog->grab().save(output + "/task_editor.png");
                        save->click();
                    } else dialog->reject();
                });
                create->click();
            }
            flush();
            check(state.tasks.size() == before + 1, "TASK_CREATE");
            check(state.tasks.size() == before + 1 && state.tasks.last().condition == QStringLiteral("成色A"), "TASK_EDITOR_CONDITION");
            if (taskTable && taskTable->rowCount()) taskTable->selectRow(taskTable->rowCount() - 1);
            auto* edit = window.findChild<QPushButton*>("taskEditButton");
            if (edit) {
                QTimer::singleShot(0, [&]() {
                    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    if (!dialog) return;
                    auto* name = dialog->findChild<QLineEdit*>("taskName");
                    auto* save = dialog->findChild<QPushButton*>("taskSaveButton");
                    if (name && save) { name->setText(QStringLiteral("离屏已编辑任务")); save->click(); }
                    else dialog->reject();
                });
                edit->click();
            }
            flush();
            check(state.tasks.last().name == QStringLiteral("离屏已编辑任务"), "TASK_EDIT");
            auto* remove = window.findChild<QPushButton*>("taskDeleteButton");
            if (remove) {
                QTimer::singleShot(0, [&]() {
                    auto* box = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    if (!box || box->objectName() != "confirmDialog") return;
                    check(mostlyLight(box), "CONFIRM_DIALOG_RENDER");
                    box->grab().save(output + "/confirm_dialog.png");
                    if (auto* yes = box->findChild<QPushButton*>("confirmAcceptButton")) yes->click();
                });
                remove->click();
            }
            flush();
            check(state.tasks.size() == before, "TASK_DELETE");
            state.startSimulation();
            state.simulateTick();
            flush();
            check(state.simulatedScans > 0 && state.simulatedSuccess > 0, "LOCAL_SIMULATION");
            state.pauseSimulation();
            const int scans = state.simulatedScans;
            state.simulateTick();
            check(!state.simulationRunning && scans == state.simulatedScans, "SIMULATION_PAUSE");
            window.setPage(3);
            flush();
            auto* priceCombo = window.findChild<QComboBox*>("pricesSkinCombo");
            if (priceCombo) priceCombo->setCurrentIndex(2);
            flush();
            auto* metric = window.findChild<QWidget*>("priceMetric0");
            auto* value = metric ? metric->findChild<QLabel*>("metricValue") : nullptr;
            check(priceCombo && value && value->text() == "215.00", "PRICE_ITEM_SELECTION");
            window.setPage(5);
            flush();
            auto* logSearch = window.findChild<QLineEdit*>("logSearch");
            auto* logTable = window.findChild<QTableWidget*>("logsTable");
            if (logSearch) logSearch->setText("NO_SUCH_LOG_ENTRY");
            flush();
            check(logSearch && logTable && logTable->rowCount() == 0, "LOG_SEARCH");
            if (logSearch) logSearch->clear();
            window.setPage(7);
            flush();
            auto* delay = window.findChild<QSpinBox*>("runPurchaseDelay");
            auto* refreshToggle = window.findChild<QCheckBox*>("runRefreshPage");
            auto* hotkey = window.findChild<QComboBox*>("runHotkeyCombo");
            auto* limitBlue = window.findChild<QSpinBox*>("runLimitBlue");
            auto* publicityStep = window.findChild<QDoubleSpinBox*>("runPublicityStep");
            auto* interval = window.findChild<QSpinBox*>("runClickInterval");
            check(delay && delay->value() == 830 && refreshToggle && !refreshToggle->isChecked() && hotkey
                  && hotkey->currentText() == "F2" && interval && interval->value() == 10 && interval->isEnabled(),
                  "RUN_SETTINGS_DEFAULTS");
            // PR07: deterministic in-memory import preview.  The dialog must
            // expose rows and diagnostics while keeping the apply action
            // disabled and leaving AppState untouched.
            window.previewImportDemo();
            flush();
            auto* importPreview = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            auto* importTable = importPreview ? importPreview->findChild<QTableWidget*>("importPreviewTable") : nullptr;
            auto* importStatus = importPreview ? importPreview->findChild<QLabel*>("importPreviewStatus") : nullptr;
            auto* importApply = importPreview ? importPreview->findChild<QPushButton*>("importPreviewApplyButton") : nullptr;
            auto* importClose = importPreview ? importPreview->findChild<QPushButton*>("importPreviewCloseButton") : nullptr;
            check(importPreview && importPreview->objectName() == "importPreviewDialog" && mostlyLight(importPreview), "IMPORT_PREVIEW_DIALOG");
            check(importTable && importTable->rowCount() == 1 && importTable->columnCount() == 5
                  && importStatus && !importStatus->text().isEmpty(), "IMPORT_PREVIEW_ROWS");
            check(importApply && !importApply->isEnabled() && importClose && importClose->isEnabled(), "IMPORT_PREVIEW_READ_ONLY");
            if (importPreview) {
                importPreview->grab().save(output + "/import_preview_dialog.png");
                const QJsonArray previewTextIssues = inspectUiText(importPreview);
                for (const auto& issue : previewTextIssues) textEncodingIssues.append(issue);
                importPreview->close();
                flush();
            }
            if (delay) delay->setValue(900);
            if (refreshToggle) refreshToggle->click();
            if (hotkey) hotkey->setCurrentText("F5");
            if (limitBlue) limitBlue->setValue(3);
            if (publicityStep) publicityStep->setValue(2.5);
            flush();
            check(state.run.purchaseDelayMs == 900 && state.run.refreshPage && state.run.hotkey == "F5"
                  && state.run.limitBlue == 3 && state.run.publicityStepMs == 2.5, "RUN_SETTINGS_EDIT");
            state.run.purchaseDelayMs = 777;
            state.run.burstClick = false;
            state.notifyChanged();
            flush();
            check(delay && delay->value() == 777 && interval && !interval->isEnabled(), "RUN_SETTINGS_REFRESH");
            QString runError;
            AppState runReload;
            check(state.saveTo(config, &runError) && runReload.loadFrom(config, &runError)
                  && runReload.run.purchaseDelayMs == 777 && runReload.run.hotkey == "F5", "RUN_SETTINGS_PERSIST");
            state.run = RunSettings{};
            state.notifyChanged();
            flush();
            bool helpShown = false;
            if (auto* help = window.findChild<QPushButton*>("helpButton")) {
                QTimer::singleShot(0, [&]() {
                    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    if (!dialog) return;
                    helpShown = dialog->objectName() == "helpDialog" && mostlyLight(dialog);
                    dialog->grab().save(output + "/help_dialog.png");
                    dialog->accept();
                });
                help->click();
            }
            flush();
            check(helpShown, "HELP_DIALOG");
            QString error;
            check(state.saveTo(config, &error), "CONFIG_SAVE");
            AppState reloaded;
            check(reloaded.loadFrom(config, &error) && reloaded.tasks.size() == state.tasks.size(), "CONFIG_RELOAD");
            check(state.exportPricesCsv(output + "/demo_prices.csv", &error), "CSV_EXPORT");
            const QString badPath = output + "/invalid-startup.json";
            { QFile bad(badPath); if (bad.open(QIODevice::WriteOnly)) bad.write("{broken-original-config"); }
            AppState recovery;
            const QString recoveryPath = relink::application::prepareStartupConfig(recovery, badPath, true);
            const bool recoveredSaved = recovery.saveTo(recoveryPath, &error);
            QFile preserved(badPath);
            preserved.open(QIODevice::ReadOnly);
            check(recoveredSaved && recoveryPath != badPath && preserved.readAll() == "{broken-original-config", "CORRUPT_CONFIG_PRESERVED");
            state.loadDemo();
            state.configPath = config;
            window.refreshAll();
            window.setPage(0);
            flush();
            window.grab().save(output + "/overview.png");
            check(relink::workspace::runWorkspaceUiSelfTest(window, workspace, output + "/workspace"), "WORKSPACE_INTEGRATION");
        }
        window.resize(1180, 820);
        flush();
        check(window.size() == QSize(1180, 820), "COMPACT_WINDOW_SIZE");
        check(navigationFits(), "COMPACT_NAVIGATION_FITS");
        check(searchCentered(), "COMPACT_SEARCH_CENTERED");
        for (int i = 0; i < pages.size(); ++i) {
            window.setPage(i);
            flush();
            if (i == 1) check(inspectorFits(), "COMPACT_INSPECTOR_NO_OVERLAP");
            if (auto* scroll = stack ? qobject_cast<QScrollArea*>(stack->currentWidget()) : nullptr) {
                scroll->verticalScrollBar()->setValue(0);
                flush();
            }
            const bool captured = window.grab().save(output + "/" + pages[i] + "_compact.png");
            // Keep the original named compact-render check for existing report consumers.
            check(captured, i == 0 ? "COMPACT_RENDER" : (QByteArray("SNAPSHOT_COMPACT_") + pages[i].toUpper().toUtf8()).constData());
            inspectGeometry(i, "compact");
        }
        const QJsonArray finalTextIssues = inspectUiText(&window);
        for (const auto& issue : finalTextIssues) textEncodingIssues.append(issue);
        check(finalTextIssues.isEmpty(), "FINAL_UI_TEXT_ENCODING");
        QFile geometryFile(output + "/geometry_report.json");
        const QByteArray geometryJson = QJsonDocument(QJsonObject{
            {"offscreen", true}, {"application_version", app.applicationVersion()},
            {"scale_factor", QString::fromUtf8(qgetenv("QT_SCALE_FACTOR"))},
            {"vertical_scrolling_allowed", true}, {"pages", geometries}}).toJson();
        const bool geometryWritten = geometryFile.open(QIODevice::WriteOnly)
            && geometryFile.write(geometryJson) == geometryJson.size();
        geometryFile.close();
        check(geometryWritten, "GEOMETRY_REPORT_WRITTEN");
        QFile results(output + "/ui_results.json");
        if (results.open(QIODevice::WriteOnly)) results.write(QJsonDocument(QJsonObject{
            {"offscreen", true}, {"game_connected", false}, {"system_input_sent", false},
            {"application_version", app.applicationVersion()},
            {"scale_factor", QString::fromUtf8(qgetenv("QT_SCALE_FACTOR"))},
            {"device_pixel_ratio", window.devicePixelRatioF()},
            {"text_encoding_issues", textEncodingIssues},
            {"passed", success}, {"checks", checks}}).toJson());
        std::printf("UI_SELF_TEST=%s; offscreen=true; game_connected=false; system_input_sent=false\n", success ? "PASS" : "FAIL");
        std::fflush(stdout);
        app.exit(success ? 0 : 1);
    });
    return app.exec();
}

} // namespace relink::diagnostics
