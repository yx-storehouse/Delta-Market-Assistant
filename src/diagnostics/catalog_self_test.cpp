#include "diagnostics/catalog_self_test.h"
#include "application/catalog_startup.h"
#include "application/catalog_configuration.h"
#include "catalog/skin_catalog.h"
#include "domain.h"
#include "mainwindow.h"
#include "widgets.h"
#include "ui/dialogs/skin_catalog_dialog.h"
#include "ui/presentation/ui_helpers.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <cstdio>

namespace relink::diagnostics {
int runCatalogSelfTest(QApplication& app, const QString& requestedOutput) {
    QTemporaryDir temporary;
    const QString output = requestedOutput.isEmpty() ? temporary.filePath("snapshots") : requestedOutput;
    QDir().mkpath(output);
    QJsonArray checks;
    bool success=true;
    auto check=[&](bool ok,const char* name) {
        success &= ok; checks.append(QJsonObject{{"check",name},{"passed",ok}});
        std::printf("CATALOG_%s=%s\n",name,ok?"PASS":"FAIL");
    };
    auto read=[](const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray(); };
    auto flush=[&] { for(int i=0;i<4;++i) { app.processEvents(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); } };
    const auto builtin=read(":/catalog/skins.json");
    QString error;
    catalog::CatalogStore store;
    AppState state;
    const QString config=temporary.filePath("config.json");
    check(application::prepareCatalogStartup(state,store,config,true,builtin,&error),"SAME_NORMAL_STARTUP_PATH");
    if (state.skins.size()!=149) { check(false,"STARTUP_149"); return 1; }
    check(state.skins.size()==149 && store.catalog().seasons.size()==11,"REAL_149_11_SEASONS");
    check(state.tasks.isEmpty(),"NO_DEFAULT_TASKS");
    bool unknown=true; for (const auto& skin:state.skins) unknown &= !skin.priceKnown&&!skin.wearKnown&&!skin.changeKnown&&!skin.followed&&!skin.catalogProductId.isEmpty();
    check(unknown,"NO_INVENTED_QUOTES_WEAR_OR_FOLLOWS");
    state.startSimulation(); state.simulateTick();
    check(!state.simulationRunning&&state.simulatedScans==0,"NORMAL_MODE_HAS_NO_SIMULATOR");
    check(!QJsonDocument::fromJson(read(config)).object().value("demo").toBool(true),"REAL_CONFIG_NOT_DEMO");
    MainWindow window(&state,nullptr,nullptr,&store);
    window.show(); flush();
    auto* table=window.findChild<QTableWidget*>("favoriteTable");
    auto* search=window.findChild<QLineEdit*>("favoriteSearch");
    auto* art=window.findChild<ArtworkView*>("skinPreview");
    check(table&&table->rowCount()==149,"ALL_PRODUCTS_IN_MAIN_UI");
    bool blank=table&&table->rowCount()==149;
    if (table) for(int row=0;row<table->rowCount();++row) blank &= table->item(row,2)->text()==QStringLiteral("—") && table->item(row,3)->text()==QStringLiteral("—") && table->item(row,4)->data(ui::SamplesRole).toList().isEmpty();
    check(blank,"UNKNOWN_CELLS_AND_EMPTY_SPARKLINES");
    check(art&&!art->hasArtwork()&&!QFile::exists(":/assets/demo_skin_atlas.png"),"THUMBNAILS_EMPTY");
    auto* condition=window.findChild<QComboBox*>("inspectorCondition");
    check(!window.findChild<QPushButton*>("favoriteCreateTaskButton")->isEnabled(),"NEW_TASK_REQUIRES_USER_PRICE");
    check(condition&&condition->currentText()==QStringLiteral("不限")&&condition->findText(QStringLiteral("成色C"))>=0,"CONDITION_SEPARATE_FROM_QUALITY");
    auto* color=window.findChild<QComboBox*>("favoriteRarity");
    if(color) color->setCurrentIndex(1);
    check(color&&color->findText(QStringLiteral("史诗品阶"))>0&&color->findText(QStringLiteral("紫色"))<0
          &&table->rowCount()>0&&table->rowCount()<149,"GRADE_FILTER");
    if(color) color->setCurrentIndex(0);
    search->setText("S11"); check(table->rowCount()==13,"SEASON_SEARCH");
    search->setText(QStringLiteral("极品")); check(table->rowCount()==14,"VARIANT_SEARCH");
    search->clear();
    window.setPage(0); flush();
    check(!window.findChild<QWidget*>("replayPanel")->isVisible()&&!window.findChild<QWidget*>("startSimulationButton")->isVisible(),"NO_NORMAL_REPLAY_CONTROLS");
    window.setPage(3); flush();
    check(window.findChild<QTableWidget*>("priceHistoryTable")->rowCount()==0,"NO_FABRICATED_HISTORY");
    const QStringList pages={"overview","favorites","tasks","prices","stats","logs","settings","run"};
    bool noDemoText=true, validText=true;
    for(int i=0;i<pages.size();++i) {
        window.setPage(i); flush();
        check(window.grab().save(output+"/"+pages[i]+".png"),qPrintable("SCREENSHOT_"+pages[i]));
        for(auto* label:window.findChildren<QLabel*>()) if(label->isVisible()) {
            noDemoText &= !label->text().contains(QStringLiteral("演示"))&&!label->text().contains(QStringLiteral("模拟数据"));
            validText &= !label->text().contains("??")&&!label->text().contains(QChar::ReplacementCharacter);
        }
        for(auto* button:window.findChildren<QPushButton*>()) if(button->isVisible()) noDemoText &= !button->text().contains(QStringLiteral("模拟"));
    }
    check(noDemoText,"NO_VISIBLE_DEMO_UI");
    check(validText,"VISIBLE_CATALOG_TEXT_UTF8");
    window.setPage(1); window.resize(1180,820); flush();
    check(window.grab().save(output+"/favorites_compact.png"),"COMPACT_SCREENSHOT");
    window.resize(1560,980); flush();
    const QString retainedId=state.skins.first().id;
    state.skins.first().followed=true;
    Task task; task.id="user-preserved-task"; task.name=QStringLiteral("保留原任务"); task.skinId=retainedId; task.maxPrice=123.45;task.maxWear=1.234567;task.condition="B";task.quantity=7;
    state.tasks.append(task); state.notifyChanged();
    window.findChild<QPushButton*>("manageSkinCatalogButton")->click(); flush();
    auto* dialog=window.findChild<ui::SkinCatalogDialog*>("skinCatalogDialog");
    check(dialog&&dialog->isVisible(),"REAL_MAINTENANCE_ENTRY");
    if (!dialog) return 1;
    check(dialog->grab().save(output+"/catalog_browse.png"),"CATALOG_BROWSE_SCREENSHOT");
    dialog->findChild<QTabWidget*>("catalogTabs")->setCurrentIndex(1);
    dialog->findChild<QComboBox*>("catalogAddSeason")->setCurrentIndex(0);
    dialog->findChild<QSpinBox*>("catalogSeasonNumber")->setValue(12);
    dialog->findChild<QLineEdit*>("catalogSeasonName")->setText(QStringLiteral("仅测试的未来赛季"));
    dialog->findChild<QLineEdit*>("catalogWeapon")->setText(QStringLiteral("AUG突击步枪"));
    dialog->findChild<QLineEdit*>("catalogSkinSeries")->setText(QStringLiteral("仅测试皮肤"));
    dialog->findChild<QLineEdit*>("catalogProductId")->setText("user:offscreen-s12");
    flush(); check(dialog->grab().save(output+"/catalog_append.png"),"CATALOG_ADD_SCREENSHOT");
    dialog->findChild<QPushButton*>("catalogAddButton")->click(); flush();
    const QString addedId=application::catalogSkinId("user:offscreen-s12");
    check(store.catalog().skins.size()==150&&state.skins.size()==150&&store.catalog().findSeason("S12"),"APPEND_UPDATES_MAIN_STATE");
    check(state.tasks.size()==1&&state.tasks.first().maxPrice==123.45&&state.tasks.first().maxWear==1.234567&&state.tasks.first().condition=="B"&&state.tasks.first().quantity==7,"ORIGINAL_TASK_UNCHANGED");
    check(ui::findSkin(&state,retainedId)&&ui::findSkin(&state,retainedId)->followed,"ORIGINAL_FOLLOW_PRESERVED");
    const auto extensionBefore=read(store.extensionPath());
    check(!dialog->importCatalogJson(store.exportJson(),&error)&&read(store.extensionPath())==extensionBefore&&state.skins.size()==150,"DUPLICATE_IMPORT_NO_CHANGE");
    // Reject collisions with an existing, non-catalog user skin before disk commit.
    Skin legacyCollision; legacyCollision.id="catalog:user:conflict";legacyCollision.catalogProductId="unrelated-legacy";
    legacyCollision.name=QStringLiteral("保留自定义皮肤");legacyCollision.series="legacy";legacyCollision.condition=QStringLiteral("未采集");legacyCollision.rarity=QStringLiteral("待核对");
    state.skins.append(legacyCollision);
    catalog::Catalog conflict;conflict.seasons={{"S12",QStringLiteral("仅测试的未来赛季")}};
    conflict.skins={{"user:conflict","S12",QStringLiteral("仅测试的未来赛季"),"AUG",QStringLiteral("仅测试冲突"),{},{},QStringLiteral("AUG - 仅测试冲突"),{}}};
    const auto stableExtension=read(store.extensionPath());
    check(!dialog->importCatalogJson(catalog::serializeCatalog(conflict),&error)&&read(store.extensionPath())==stableExtension&&store.catalog().skins.size()==150,"CONFIG_COLLISION_REJECTED_BEFORE_COMMIT");
    state.skins.removeLast();
    dialog->accept();flush();
    search->setText("user:offscreen-s12"); flush();
    check(table->rowCount()==1,"APPENDED_ITEM_SEARCHABLE");
    auto* maxPrice=window.findChild<QDoubleSpinBox*>("inspectorMaxPrice"); maxPrice->setValue(42.50);
    auto* create=window.findChild<QPushButton*>("favoriteCreateTaskButton");
    if (!create) create=window.findChild<QPushButton*>("createFavoriteTaskButton");
    // Resolve stable object by the explicit condition action as a final guard.
    if(!create) for(auto* button:window.findChildren<QPushButton*>()) if(button->text()==QStringLiteral("创建任务")) {create=button;break;}
    if(create) create->click();
    check(create&&state.tasks.size()==2&&state.tasks.last().skinId==addedId&&state.tasks.last().maxPrice==42.50,"APPENDED_ITEM_USABLE_IN_TASK");
    check(state.saveTo(config,&error),"SAVE_USER_CONFIGURATION");
    catalog::CatalogStore reloadedStore; AppState reloaded;
    check(application::prepareCatalogStartup(reloaded,reloadedStore,config,true,builtin,&error)&&reloaded.skins.size()==150&&reloaded.tasks.size()==2,"RESTART_RETAINS_APPEND_AND_TASKS");
    catalog::CatalogStore emptyStore; AppState secondInstall;
    check(application::prepareCatalogStartup(secondInstall,emptyStore,temporary.filePath("new-install/config.json"),true,builtin,&error)&&secondInstall.skins.size()==149&&secondInstall.tasks.isEmpty(),"TEST_ADDITIONS_NOT_IN_BUILTIN");
    // A broken extension must not overwrite either user data file.
    const auto configBefore=read(config);
    {QFile broken(store.extensionPath());broken.open(QIODevice::WriteOnly);broken.write("broken-json");}
    catalog::CatalogStore badStore;AppState badState;
    check(!application::prepareCatalogStartup(badState,badStore,config,true,builtin,&error)&&read(config)==configBefore&&read(store.extensionPath())=="broken-json","BROKEN_EXTENSION_PRESERVED");
    // Historical untouched demos get a byte-for-byte backup, not a new catalog
    // with the old fake prices relabelled as real observations.
    AppState legacy;legacy.loadTestFixture();
    for(auto& t:legacy.tasks) t.enabled=false;
    for(auto& skin:legacy.skins) skin.followed=false;
    const QString legacyPath=temporary.filePath("legacy/config.json");QDir().mkpath(temporary.filePath("legacy"));legacy.saveTo(legacyPath,&error);
    const auto original=read(legacyPath);AppState migrated;catalog::CatalogStore migratedStore;
    check(application::prepareCatalogStartup(migrated,migratedStore,legacyPath,true,builtin,&error)&&migrated.skins.size()==149&&migrated.tasks.isEmpty(),"DEMO_MIGRATION_TO_REAL");
    check(read(legacyPath+".before-real-catalog.bak")==original,"MIGRATION_BACKUP_EXACT");
    QFile report(output+"/catalog_report.json");report.open(QIODevice::WriteOnly);report.write(QJsonDocument(QJsonObject{{"passed",success},{"checks",checks},{"offscreen",true},{"game_connected",false},{"system_input_sent",false}}).toJson());report.close();
    std::printf("CATALOG_SELF_TEST=%s; assertions=%lld; offscreen=true; game_connected=false; system_input_sent=false\n",success?"PASS":"FAIL",static_cast<long long>(checks.size()));
    std::fflush(stdout);window.hide();return success?0:1;
}
}
