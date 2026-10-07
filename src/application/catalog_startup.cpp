#include "application/catalog_startup.h"
#include "application/catalog_configuration.h"
#include "application/startup_config.h"
#include "catalog/skin_catalog.h"
#include "domain.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
namespace relink::application {
bool prepareCatalogStartup(AppState& state, catalog::CatalogStore& store,
    const QString& requestedConfig, bool loadExisting, const QByteArray& builtin, QString* error) {
    if (error) error->clear();
    // Extensions stay beside the requested user config, independent of releases
    // and independent of the recovery filename chosen for a malformed config.
    const QString extension = QFileInfo(requestedConfig).absolutePath() + "/catalog_extensions.json";
    if (!store.load(builtin, extension, error)) return false;
    const QString destination = prepareStartupConfig(state, requestedConfig, loadExisting);
    const auto before = QJsonDocument(encodeV1Config(state.skins, state.tasks, state.run)).toJson(QJsonDocument::Compact);
    if (!applyCatalogConfiguration(state, store.catalog(), nullptr, error)) return false;
    const auto after = QJsonDocument(encodeV1Config(state.skins, state.tasks, state.run)).toJson(QJsonDocument::Compact);
    if (loadExisting && QFileInfo::exists(destination) && before != after) {
        QString backup = destination + ".before-real-catalog.bak";
        for (int suffix=1; QFileInfo::exists(backup); ++suffix)
            backup = destination + ".before-real-catalog-" + QString::number(suffix) + ".bak";
        if (!QFile::copy(destination, backup)) {
            if (error) *error = QStringLiteral("旧配置备份失败，原文件未改动：") + destination;
            return false;
        }
    }
    if (loadExisting && !state.saveTo(destination, error)) return false;
    return true;
}
}
