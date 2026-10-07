#include "savedvalue_preview.h"
#include "config/savedvalue_reader.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <cstdio>
namespace relink::diagnostics {
int runSavedValuePreview(int argc,char** argv){
 QCoreApplication app(argc,argv);QCommandLineParser parser;parser.addHelpOption();
 parser.addOption({"savedvalue-preview","Decode collection settings without executing input or opening UI.","file"});parser.process(app);
 QFile input(parser.value("savedvalue-preview"));QJsonObject result;
 if(!input.open(QIODevice::ReadOnly) || input.size()>8*1024*1024)result={{"valid",false},{"error","E_SAVEDVALUE_FILE"}};
 else{
  QFile catalog(QStringLiteral(":/data/relink_0925_catalog.json"));
  if(!catalog.open(QIODevice::ReadOnly))result={{"valid",false},{"error","E_SAVEDVALUE_DICTIONARY"}};
  else result=config::savedValueCollectionSnapshot(input.read(8*1024*1024+1),QJsonDocument::fromJson(catalog.readAll()).object());
 }
 result["game_input_sent"]=false;result["source_file_modified"]=false;
 const auto bytes=QJsonDocument(result).toJson(QJsonDocument::Compact);std::fwrite(bytes.constData(),1,size_t(bytes.size()),stdout);std::fputc('\n',stdout);std::fflush(stdout);
 return result["valid"].toBool()?0:2;
}
}
