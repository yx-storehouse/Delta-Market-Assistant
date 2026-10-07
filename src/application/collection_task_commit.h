#pragma once
#include "application/collection_task_import.h"
namespace relink::application {
bool commitCollectionTaskImport(const QByteArray& bytes, const QJsonObject& dictionary,
    const catalog::Catalog& catalog, AppState& state, CollectionTaskImportPreview* report = nullptr,
    QString* error = nullptr);
}
