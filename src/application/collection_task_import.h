#pragma once

#include "catalog/skin_catalog.h"
#include "domain.h"

namespace relink::application {

struct CollectionTaskImportRow {
    Task task;
    QString productId, seasonId, seasonLabel, displayName, menuColor, variantLabel;
    bool alreadyImported = false;
};

struct CollectionTaskImportPreview {
    QString sourceSha256, dictionarySha256;
    QVector<CollectionTaskImportRow> rows;
    QVector<Task> mergedTasks;
    int rowCount = 0, enabledCount = 0, distinctProducts = 0; // Enabled products only.
    int totalDistinctProducts = 0; // Includes disabled original placeholder rows.
    int addedCount = 0, duplicateCount = 0;
    // Purchase/runtime settings are retained only for inspection, never applied.
    QJsonObject inactiveSettingsMetadata;
};

// Source identity is content-addressed; filenames and paths do not affect dedup.
QString collectionImportTaskId(const QString& sourceSha256, int sourceRow);

// Pure data-only preparation. Rejects an entire input if even a disabled row
// lacks an exact dictionary/catalog identity. No writes or state notifications.
// On error the supplied preview and AppState remain exactly unchanged.
bool previewCollectionTaskImport(const QByteArray& bytes, const QJsonObject& dictionary,
                                 const catalog::Catalog& catalog, const AppState& state,
                                 CollectionTaskImportPreview* preview,
                                 QString* error = nullptr);

// Revalidates against the current state and appends atomically in memory.
// Existing tasks, follows, observations and run settings remain untouched.
// A caller requiring disk durability should save preview.mergedTasks first and
// only then publish them to the state; this helper itself performs no disk I/O.
bool applyCollectionTaskImport(const QByteArray& bytes, const QJsonObject& dictionary,
                               const catalog::Catalog& catalog, AppState& state,
                               CollectionTaskImportPreview* report = nullptr,
                               QString* error = nullptr);

} // namespace relink::application
