#pragma once
#include <QString>
#include <QByteArray>
class AppState;
namespace relink::catalog { class CatalogStore; }
namespace relink::application {
bool prepareCatalogStartup(AppState& state, catalog::CatalogStore& store,
    const QString& requestedConfig, bool loadExisting, const QByteArray& builtin,
    QString* error = nullptr);
}
