#include "diagnostics/collection_task_cli.h"
#include "application/collection_task_commit.h"
#include "application/catalog_configuration.h"
#include "catalog/skin_catalog.h"
#include "domain.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <cstdio>
namespace relink::diagnostics {
int runCollectionTaskImportCli(int argc,char** argv) {
    QCoreApplication app(argc,argv);QCommandLineParser parser;parser.addHelpOption();
    parser.addOption({"import-collection-tasks","Append .savedValue into an explicit config, data only.","path"});
    parser.addOption({"config","Explicit destination JSON config, required.","path"});parser.process(app);
    if(parser.value("config").isEmpty()||parser.value("import-collection-tasks").isEmpty()) {std::fprintf(stderr,"E_IMPORT_REQUIRES_EXPLICIT_CONFIG\n");return 2;}
    QString error;application::CollectionTaskImportPreview report;AppState state;catalog::CatalogStore store;
    const QString path=QFileInfo(parser.value("config")).absoluteFilePath();
    QFile input(parser.value("import-collection-tasks")),builtin(":/catalog/skins.json"),dictionary(":/data/relink_0925_catalog.json");
    bool ok=input.open(QIODevice::ReadOnly)&&input.size()<=8*1024*1024&&builtin.open(QIODevice::ReadOnly)&&dictionary.open(QIODevice::ReadOnly);
    if(!ok)error=QStringLiteral("任务或目录文件读取失败，文件上限 8 MiB。");
    if(ok)ok=store.load(builtin.readAll(),QFileInfo(path).absolutePath()+"/catalog_extensions.json",&error);
    if(ok&&QFileInfo::exists(path))ok=state.loadFrom(path,&error);
    state.configPath=path;
    // Prepare in memory, unlike normal startup. Invalid imports must not even
    // migrate or rewrite the existing config; commit() owns the sole save.
    if(ok)ok=application::applyCatalogConfiguration(state,store.catalog(),nullptr,&error);
    if(ok)ok=application::commitCollectionTaskImport(input.read(8*1024*1024+1),QJsonDocument::fromJson(dictionary.readAll()).object(),store.catalog(),state,&report,&error);
    std::printf("COLLECTION_IMPORT=%s; rows=%d; enabled=%d; added=%d; duplicates=%d; game_input_sent=false\n",ok?"PASS":"FAIL",report.rowCount,report.enabledCount,report.addedCount,report.duplicateCount);
    if(!ok) std::fprintf(stderr,"%s\n",error.toUtf8().constData());
    std::fflush(stdout);
    return ok?0:1;
}
}
