#pragma once

#include "runtime.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QString>

namespace relink::runtime {

struct ReplayUiProjection {
    QString source;
    QString runState;
    QString runStateText;
    QString matchReason;
    QString progress;
    bool canStart = false;
    bool canPause = false;
    bool canResume = false;
    bool canStop = false;
};

// Read-only projection for PR07 import previews.  This projection intentionally
// carries no executable configuration or commit action; it is derived entirely
// from the LegacyImportPreview JSON emitted by config::previewV1/preview13Columns.
struct ImportPreviewUiProjection {
    QString format;
    QString encoding;
    QString sourceHash;
    int rowCount = 0;
    int reviewRows = 0;
    int invalidRows = 0;
    int errorCount = 0;
    int diagnosticCount = 0;
    bool committable = false;
    bool readOnly = true;
    QString statusText;
    QString readOnlyNote;
};

class UiProjection {
public:
    static ReplayUiProjection replay(const RunSnapshot& snapshot);
    static ImportPreviewUiProjection importPreview(const QJsonObject& preview);
};

} // namespace relink::runtime
