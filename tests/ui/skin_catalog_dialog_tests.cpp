// Catalog management remains offline; no MainWindow or game dependencies.
#include "catalog/skin_catalog.h"
#include "domain.h"
#include "fluenttheme.h"
#include "ui/dialogs/skin_catalog_dialog.h"
#include "ui/dialogs/task_editor_dialog.h"
#include "ui/pages/task_page.h"
#include "ui/pages/run_settings_page.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* description) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", description); }
}
template<class T> T* child(QObject& object, const char* name) {
    auto* found = object.findChild<T*>(QString::fromLatin1(name));
    if (!found) { std::fprintf(stderr, "MISSING %s\n", name); std::exit(2); }
    return found;
}
QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
relink::catalog::Skin referenceSkin(const QString& id, const QString& season,
        const QString& seasonLabel, const QString& weapon, const QString& series) {
    return {id, season, seasonLabel, weapon, series, QString(), QStringLiteral("purple"),
            QString(), weapon + QStringLiteral(" - ") + series, QString()};
}
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    const QString fonts = qEnvironmentVariable("WINDIR", "C:/Windows") + QStringLiteral("/Fonts/");
    for (const auto& name : {QStringLiteral("msyh.ttc"), QStringLiteral("msyhbd.ttc"),
         QStringLiteral("segoeui.ttf"), QStringLiteral("seguisb.ttf"),
         QStringLiteral("SegoeIcons.ttf"), QStringLiteral("segmdl2.ttf")})
        QFontDatabase::addApplicationFont(fonts + name);
#endif
    app.setStyle(QStringLiteral("Fusion"));
    app.setPalette(FluentTheme::palette());
    app.setFont(FluentTheme::font(13));
    using namespace relink::catalog;
    using relink::ui::SkinCatalogDialog;
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary directory");
    const QString extensionPath = temporary.filePath(QStringLiteral("catalog.json"));
    Catalog builtin;
    builtin.seasons = {{QStringLiteral("S6"), QStringLiteral("棱镜攻势 S2")},
                       {QStringLiteral("S11"), QStringLiteral("疾风魅影")}};
    builtin.skins = {
        referenceSkin("10602", "S6", QStringLiteral("棱镜攻势 S2"), QStringLiteral("AUG突击步枪"), QStringLiteral("天命")),
        referenceSkin("11102", "S11", QStringLiteral("疾风魅影"), QStringLiteral("AUG突击步枪"), QStringLiteral("黑银先锋"))};
    const QByteArray source = serializeCatalog(builtin);
    CatalogStore store;
    QString error;
    check(store.load(source, extensionPath, &error), "initial reference catalog loads");
    SkinCatalogDialog dialog(&store);
    int changed = 0;
    QObject::connect(&dialog, &SkinCatalogDialog::catalogChanged, &app, [&] { ++changed; });
    auto* table = child<QTableWidget>(dialog, "catalogTable");
    auto* search = child<QLineEdit>(dialog, "catalogSearch");
    auto* filter = child<QComboBox>(dialog, "catalogSeasonFilter");
    check(!dialog.isVisible(), "constructor does not open a visible window");
    check(table->rowCount() == 2 && changed == 0 && !QFile::exists(extensionPath), "render does not create data or mutation");
    check(table->item(0, 3)->text() == QStringLiteral("—"), "variant not inferred from menu color");
    check(table->item(0, 4)->text() == QStringLiteral("紫色"), "menu color retained as textual metadata");
    check(table->item(0, 5)->text() == QStringLiteral("—"), "unknown quality remains unassigned");
    check(table->findChildren<QLabel*>().isEmpty(), "table has no thumbnail label or generated artwork");
    search->setText(QStringLiteral("黑银"));
    check(table->rowCount() == 1 && table->item(0, 6)->text() == "11102", "search locates exact source product");
    search->clear();
    filter->setCurrentIndex(filter->findData(QStringLiteral("S6")));
    check(table->rowCount() == 1 && table->item(0, 6)->text() == "10602", "season filter");
    filter->setCurrentIndex(0);
    search->setText(QStringLiteral("no match"));
    check(table->rowCount() == 0, "empty search has no fabricated fallback rows");
    search->clear();

    child<QTabWidget>(dialog, "catalogTabs")->setCurrentIndex(1);
    auto* add = child<QPushButton>(dialog, "catalogAddButton");
    add->click();
    check(changed == 0 && store.catalog().skins.size() == 2, "empty add rejected");
    check(child<QLabel>(dialog, "catalogMessage")->text().contains(QStringLiteral("武器")), "validation describes missing field");
    auto* season = child<QComboBox>(dialog, "catalogAddSeason");
    auto* weapon = child<QLineEdit>(dialog, "catalogWeapon");
    auto* series = child<QLineEdit>(dialog, "catalogSkinSeries");
    auto* productId = child<QLineEdit>(dialog, "catalogProductId");
    auto* color = child<QComboBox>(dialog, "catalogMenuColor");
    season->setCurrentIndex(season->findData(QStringLiteral("S6")));
    weapon->setText(QStringLiteral("P90冲锋枪"));
    series->setText(QStringLiteral("天命"));
    productId->setText(QStringLiteral("10602"));
    color->setCurrentIndex(color->findData(QStringLiteral("purple")));
    add->click();
    check(changed == 0 && store.catalog().skins.size() == 2 && !QFile::exists(extensionPath), "duplicate source ID rejected without writes");
    productId->setText(QStringLiteral("10603"));
    add->click();
    check(changed == 1 && store.catalog().skins.size() == 3, "same-season append saved and emitted once");
    const auto* p90 = store.catalog().findSkin(QStringLiteral("10603"));
    check(p90 && p90->thumbnailPath.isEmpty() && p90->gameQualityName.isEmpty(), "new skin keeps reserved image and unknown quality empty");
    check(QFile::exists(extensionPath) && !read(extensionPath).isEmpty(), "append is persisted before completion");
    check(weapon->text().isEmpty() && series->text().isEmpty() && productId->text().isEmpty(), "form clears identity fields after save");
    check(season->currentData().toString() == "S6", "same-season continuation remains selected");

    // Future-season test fixture exists only in this temporary test catalog.
    season->setCurrentIndex(0);
    child<QSpinBox>(dialog, "catalogSeasonNumber")->setValue(12);
    child<QLineEdit>(dialog, "catalogSeasonName")->setText(QStringLiteral("测试赛季"));
    weapon->setText(QStringLiteral("测试武器"));
    series->setText(QStringLiteral("测试系列"));
    add->click();
    check(changed == 2 && store.catalog().skins.size() == 4 && store.catalog().seasons.size() == 3, "new season and first skin commit together");
    const auto& added = store.catalog().skins.last();
    check(added.productId.startsWith("user:") && added.seasonId == "S12", "blank ID receives stable local identity");
    check(season->currentData().toString() == "S12", "new season becomes continuation selection");
    const QByteArray afterAdd = read(extensionPath);
    const QString addedId = added.productId;
    CatalogStore reopened;
    check(reopened.load(source, extensionPath, &error) && reopened.catalog().findSkin(addedId), "local generated ID survives restart");
    check(!dialog.importCatalogJson("not json", &error) && !error.isEmpty(), "invalid JSON import rejected with error");
    check(changed == 2 && read(extensionPath) == afterAdd, "invalid import preserves disk and no change signal");
    check(!dialog.importCatalogJson(store.exportJson(), &error), "full catalog reimport rejects collisions");
    check(read(extensionPath) == afterAdd && changed == 2, "reimport never overwrites existing records");

    Catalog addition;
    addition.seasons = {{QStringLiteral("S6"), QStringLiteral("棱镜攻势 S2")}};
    addition.skins = {referenceSkin("10604", "S6", QStringLiteral("棱镜攻势 S2"), QStringLiteral("SR-25射手步枪"), QStringLiteral("天命"))};
    check(dialog.importCatalogJson(serializeCatalog(addition), &error), "append import accepts new ID from an existing season");
    check(changed == 3 && store.catalog().skins.size() == 5 && table->rowCount() == 5, "successful import updates table and signal once");
    check(store.catalog().findSkin("10602")->weapon == QStringLiteral("AUG突击步枪"), "original source row unchanged by all appends");
    SkinCatalogDialog unavailable(nullptr);
    check(!child<QPushButton>(unavailable, "catalogAddButton")->isEnabled(), "missing catalog disables add");
    check(!unavailable.importCatalogJson(source, &error) && !error.isEmpty(), "missing catalog import has clear result");
    AppState production;
    ::Skin knownSkin;
    knownSkin.id = QStringLiteral("10602");
    knownSkin.name = QStringLiteral("AUG突击步枪 - 天命");
    knownSkin.series = QStringLiteral("S6");
    knownSkin.condition = QStringLiteral("不限");
    production.skins.append(knownSkin);
    relink::ui::TaskPage productionTasks(&production);
    productionTasks.setSimulationAvailable(true);
    auto* simulation = child<QPushButton>(productionTasks, "tasksSimulationButton");
    check(simulation->isHidden() && !simulation->isEnabled(), "normal task page has no simulation action");
    simulation->click();
    check(!production.simulationRunning, "hidden task button cannot execute simulation");
    relink::ui::TaskEditorDialog productionEditor(&production);
    auto* threshold = child<QDoubleSpinBox>(productionEditor, "taskMaxPrice");
    check(threshold->value() == 0 && threshold->specialValueText() == QStringLiteral("请填写"), "unknown quote does not invent task price threshold");
    check(!productionEditor.windowTitle().contains(QStringLiteral("模拟")), "normal editor title refers to saved tasks");
    child<QLineEdit>(productionEditor, "taskName")->setText(QStringLiteral("真实目录收藏条件"));
    child<QPushButton>(productionEditor, "taskSaveButton")->click();
    check(productionEditor.result() != QDialog::Accepted
          && child<QLabel>(productionEditor, "taskValidationLabel")->text().contains(QStringLiteral("最高价格")),
          "new normal task requires a user-provided positive maximum");
    threshold->setValue(230);
    child<QPushButton>(productionEditor, "taskSaveButton")->click();
    check(productionEditor.result() == QDialog::Accepted && productionEditor.task().maxPrice == 230
          && productionEditor.task().skinId == "10602", "explicit threshold produces real-ID task draft");
    Task zeroPriceTask = productionEditor.task();
    zeroPriceTask.maxPrice = 0;
    relink::ui::TaskEditorDialog zeroPriceEdit(&production, &zeroPriceTask);
    child<QPushButton>(zeroPriceEdit, "taskSaveButton")->click();
    check(zeroPriceEdit.result() == QDialog::Accepted && zeroPriceEdit.task().maxPrice == 0,
          "editing preserves a pre-existing zero maximum without broad schema changes");
    relink::ui::RunSettingsPage productionRun(&production);
    check(child<QPushButton>(productionRun, "runImportPreviewButton")->text() == QStringLiteral("选择配置文件"), "normal import uses explicit real file intent");
    if (argc == 2) {
        CatalogStore fullStore;
        check(fullStore.load(read(QString::fromLocal8Bit(argv[1])), temporary.filePath("real_extensions.json"), &error), "full source catalog loads");
        SkinCatalogDialog fullDialog(&fullStore);
        check(child<QTableWidget>(fullDialog, "catalogTable")->rowCount() == 149, "all 149 source skins visible without fallback data");
        bool emptyThumbnails = true;
        for (const auto& skin : fullStore.catalog().skins) emptyThumbnails = emptyThumbnails && skin.thumbnailPath.isEmpty();
        check(emptyThumbnails, "all source thumbnail fields remain blank");
    }
    // Optional offscreen visual acceptance uses the actual bundled catalog,
    // never the isolated future-season test fixture above.
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--render")) {
        const QString catalogPath = QString::fromLocal8Bit(argv[2]);
        const QString outputDirectory = QString::fromLocal8Bit(argv[3]);
        CatalogStore reference;
        check(reference.load(read(catalogPath), temporary.filePath("render_extensions.json"), &error), "render source loads");
        check(QDir().mkpath(outputDirectory), "render output directory");
        QFile theme(QStringLiteral("src/theme.qss"));
        if (theme.open(QIODevice::ReadOnly)) app.setStyleSheet(QString::fromUtf8(theme.readAll()));
        SkinCatalogDialog render(&reference);
        render.resize(1120, 780);
        render.show();
        QApplication::processEvents();
        check(render.grab().save(QDir(outputDirectory).filePath("catalog_browse.png")), "browse rendered offscreen");
        child<QTabWidget>(render, "catalogTabs")->setCurrentIndex(1);
        QApplication::processEvents();
        check(render.grab().save(QDir(outputDirectory).filePath("catalog_append.png")), "append rendered offscreen");
        check(!QFile::exists(temporary.filePath("render_extensions.json")), "visual render remains read-only");
        render.hide();
    }
    std::printf("skin_catalog_dialog_tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
