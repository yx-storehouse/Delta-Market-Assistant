#pragma once

#include "config/import_types.h"

#include <QJsonObject>
#include <QString>

#include <variant>

namespace relink::config {

// The result of turning a read-only LegacyImportPreview into a reviewed v2
// document.  This is deliberately a plain JSON boundary: PR10 can persist the
// same document without coupling the profile workflow to QtSql.
struct ProfileCommitResult {
    QJsonObject document;
    QString profileId;
    int profileRevision = 0;
    bool committed = false;
};

struct ProfileStoreError {
    QString code;
    QString message;
};

using ProfileStoreResult = std::variant<ProfileCommitResult, ProfileStoreError>;

class ProfileStore {
public:
    // Materialize a reviewed profile in memory.  No file is opened and the
    // preview object is never modified.  A preview with parse errors, missing
    // confirmations, or invalid choices returns a stable error code.
    static ProfileStoreResult materializePreview(const QJsonObject& preview,
                                                 const ReviewChoices& choices,
                                                 const QJsonObject& existing = {});

    // Read a v2 document from disk.  This helper is intentionally read-only.
    static bool load(const QString& path, QJsonObject& document, QString* error = nullptr);

    // Materialize and atomically replace path with QSaveFile.  The old file is
    // not touched if materialization, open, write, or commit fails.
    static bool commitPreview(const QJsonObject& preview,
                              const ReviewChoices& choices,
                              const QString& path,
                              QString* error = nullptr,
                              ProfileCommitResult* committed = nullptr);
};

} // namespace relink::config

