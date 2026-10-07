#pragma once

#include "application/workspace/workspace_projection.h"
#include "config/import_types.h"
#include <QHash>
#include <QJsonObject>
#include <QString>

namespace relink::workspace {

struct ProfileCatalogResult {
    QVector<ProfileRow> rows;
    QHash<QString, QJsonObject> documents;
    QString error;
    bool ok = false;
};

// Owns profile catalogue concerns independently from replay and persistence
// orchestration. It performs a deterministic scan and keeps ConfigV2 validation
// at one boundary so callers never operate on a partially accepted profile.
class ProfileCatalog final {
public:
    static QString builtinProfileId();
    static QJsonObject builtinDocument();
    static bool validateDocument(const QJsonObject& document, QString* error = nullptr);

    ProfileCatalogResult scan(const QString& rootPath, const QString& selectedProfileId) const;
    bool materializeAndValidate(const QJsonObject& preview,
                                const config::ReviewChoices& choices,
                                QJsonObject* document,
                                QString* error = nullptr) const;
};

} // namespace relink::workspace
