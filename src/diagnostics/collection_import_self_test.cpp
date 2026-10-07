#include "diagnostics/collection_import_self_test.h"
#include "application/catalog_startup.h"
#include "application/collection_task_import.h"
#include "catalog/skin_catalog.h"
#include "domain.h"
#include "mainwindow.h"
#include "ui/dialogs/collection_import_dialog.h"
#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <cstdio>
namespace {
QByteArray read(const QString& path) {QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray();}
QByteArray head(int kind, quint64 n) {int e=0;while(e<3&&n>=(quint64(1)<<(8*(1<<e))))++e;QByteArray b(1,char(kind|(e<<6)));for(int i=0;i<(1<<e);++i)b.append(char((n>>(i*8))&255));return b;}
QByteArray encode(const QJsonValue& v) {
    if(v.isBool())return QByteArray(1,char(v.toBool()));if(v.isDouble())return head(4,v.toInteger());
    if(v.isString()){auto b=v.toString().toUtf8();return head(7,b.size())+b;}
    if(v.isArray()){auto b=head(3,v.toArray().size());for(auto item:v.toArray())b+=encode(item);return b;}
    auto o=v.toObject();auto b=head(2,o.size());for(auto it=o.begin();it!=o.end();++it)b+=encode(it.key())+encode(it.value());return b;
}
QByteArray fixture() {
    QJsonArray pairs;
    for(int row=0;row<2;++row) {
        QJsonObject fields{{QStringLiteral("启用"),row==0},{QStringLiteral("枪名设置栏"),10602},
            {QStringLiteral("成色设置栏"),row},{QStringLiteral("最低价格设置栏"),10},
            {QStringLiteral("最高价格设置栏"),row==0?600:400},{QStringLiteral("磨损度设置栏"),5},{QStringLiteral("限量设置栏"),row==0?0:3}};
        for(auto it=fields.begin();it!=fields.end();++it)pairs.append(QJsonArray{it.key()+"_"+QString::number(row),it.value()});
    }
    return encode(QJsonObject{{QStringLiteral("自动收藏-任务配置"),pairs}});
}
}
namespace relink::diagnostics {
int runCollectionImportSelfTest(QApplication& app,const QString& requested,const QString& source) {
    QTemporaryDir temp; const auto output=requested.isEmpty()?temp.filePath("shots"):requested;QDir().mkpath(output);
    bool success=true;QJsonArray checks;
    auto check=[&](bool ok,const char* name){success &= ok;checks.append(QJsonObject{{"check",name},{"passed",ok}});std::printf("COLLECTION_IMPORT_%s=%s\n",name,ok?"PASS":"FAIL");};
    auto flush=[&]{for(int i=0;i<3;++i){app.processEvents();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);}};
    const auto bytes=source.isEmpty()?fixture():read(source);check(!bytes.isEmpty(),"SOURCE_READ");
    const auto dictionary=QJsonDocument::fromJson(read(":/data/relink_0925_catalog.json")).object();
    catalog::CatalogStore store;AppState state;QString error;
    const auto path=temp.filePath("config.json");
    check(application::prepareCatalogStartup(state,store,path,true,read(":/catalog/skins.json"),&error),"REAL_STARTUP");
    if(state.skins.size()!=149)return 1;
    state.skins.first().followed=true;
    Task original;original.id="keep-user-task";original.skinId=state.skins.first().id;original.name=QStringLiteral("保留既有任务");original.maxPrice=123;state.tasks.append(original);state.saveTo(path,&error);
    const auto oldDisk=read(path);
    application::CollectionTaskImportPreview expected;
    check(application::previewCollectionTaskImport(bytes,dictionary,store.catalog(),state,&expected,&error),"SOURCE_RESOLVED");
    if(expected.rows.isEmpty())return 1;
    if(expected.sourceSha256=="cff452bcd4b77a17fb3cc9a23f55e0131304098f7ac0057205f619bbd4406253")
        check(expected.rowCount==50&&expected.enabledCount==21&&expected.distinctProducts==9&&expected.totalDistinctProducts==13,"USER_50_21_9_ACTIVE_13_TOTAL");
    MainWindow window(&state,nullptr,nullptr,&store);window.show();window.setPage(2);flush();
    auto* entry=window.findChild<QPushButton*>("taskImportCollectionButton");check(entry&&entry->isVisible(),"TASK_PAGE_ENTRY");
    if(entry)entry->click();flush();auto* dialog=window.findChild<ui::CollectionImportDialog*>("collectionImportDialog");
    check(dialog&&dialog->isVisible(),"REAL_ENTRY_OPENS_DIALOG");if(!dialog)return 1;
    check(dialog->loadSavedValue(bytes,source.isEmpty()?QStringLiteral("临时测试.savedValue"):QFileInfo(source).fileName(),&error),"REAL_DIALOG_PREVIEW");flush();
    check(state.tasks.size()==1&&read(path)==oldDisk,"PREVIEW_NO_MUTATION");
    check(dialog->grab().save(output+"/import_preview.png"),"PREVIEW_SCREENSHOT");
    const auto beforeTasks=state.tasks.size();
    QDir().mkpath(temp.filePath("directory-as-file"));state.configPath=temp.filePath("directory-as-file");
    dialog->findChild<QPushButton*>("collectionImportCommitButton")->click();flush();
    check(dialog->isVisible()&&state.tasks.size()==beforeTasks&&read(path)==oldDisk,"SAVE_FAILURE_PRESERVES_STATE_AND_DIALOG");
    check(!dialog->findChild<QLabel*>("collectionImportMessage")->text().isEmpty(),"SAVE_ERROR_VISIBLE");
    state.configPath=path;dialog->findChild<QPushButton*>("collectionImportCommitButton")->click();flush();
    check(!window.findChild<ui::CollectionImportDialog*>("collectionImportDialog"),"CLOSE_ONLY_AFTER_PERSISTENCE");
    check(state.tasks.size()==1+expected.rowCount&&state.tasks.first().id==original.id&&state.skins.first().followed,"APPEND_KEEPS_TASK_AND_FOLLOW");
    check(read(path+".before-task-import.bak")==oldDisk,"EXACT_PREIMPORT_BACKUP");
    AppState reopened;check(reopened.loadFrom(path,&error)&&reopened.tasks.size()==state.tasks.size(),"RELOAD_PERSISTED_TASKS");
    bool sourceRows=true;for(int i=0;i<expected.rows.size();++i){const auto& actual=reopened.tasks[i+1];const auto& wanted=expected.rows[i].task;sourceRows &= actual.importSource.row==wanted.importSource.row&&actual.quantity==wanted.quantity&&actual.condition==wanted.condition&&actual.minPrice==wanted.minPrice&&actual.maxPrice==wanted.maxPrice&&actual.maxWear==wanted.maxWear&&actual.enabled==wanted.enabled&&actual.importSource.fields==wanted.importSource.fields;}
    check(sourceRows,"EACH_SOURCE_ROW_AND_PARAMETERS_PERSISTED");
    check(state.tasks[1].quantity==0,"UNLIMITED_ZERO_PRESERVED");
    check(!state.simulationRunning&&state.simulatedScans==0,"IMPORT_DOES_NOT_START_BUSINESS");
    window.setPage(2);flush();check(window.grab().save(output+"/imported_tasks.png"),"TASKS_SCREENSHOT");
    window.resize(1180,820);flush();check(window.grab().save(output+"/imported_tasks_compact.png"),"COMPACT_SCREENSHOT");
    state.tasks[1].maxPrice=777;state.notifyChanged();
    window.previewCollectionTasks(bytes,QStringLiteral("同一文件改名.savedValue"));flush();dialog=window.findChild<ui::CollectionImportDialog*>("collectionImportDialog");
    check(dialog&&dialog->preview().addedCount==0&&dialog->preview().duplicateCount==expected.rowCount,"RENAMED_FILE_DEDUPLICATES");
    check(dialog&&!dialog->findChild<QPushButton*>("collectionImportCommitButton")->isEnabled()&&state.tasks[1].maxPrice==777,"DUPLICATE_PRESERVES_USER_EDITS");
    if(dialog)dialog->reject();flush();
    check(source.isEmpty()||read(source)==bytes,"SOURCE_FILE_UNCHANGED");
    QFile report(output+"/import_report.json");report.open(QIODevice::WriteOnly);report.write(QJsonDocument(QJsonObject{{"passed",success},{"source_sha256",expected.sourceSha256},{"rows",expected.rowCount},{"enabled",expected.enabledCount},{"checks",checks},{"game_input_sent",false},{"offscreen",true}}).toJson());report.close();
    std::printf("COLLECTION_IMPORT_SELF_TEST=%s; assertions=%lld; rows=%d; enabled=%d; offscreen=true; game_input_sent=false\n",success?"PASS":"FAIL",static_cast<long long>(checks.size()),expected.rowCount,expected.enabledCount);std::fflush(stdout);window.hide();return success?0:1;
}
}
