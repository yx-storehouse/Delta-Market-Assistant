#pragma once

#include "application/workspace/workspace_projection.h"
#include <QString>

namespace relink::workspace {

struct ExportResult { bool ok = false; QString error; };

class RecordsExporter final {
public:
    static QString csvCell(QString value);
    ExportResult writeCsv(const QString& path, const QString& rootPath,
                          const QString& databasePath, const WorkspaceProjection& projection) const;
};

} // namespace relink::workspace
