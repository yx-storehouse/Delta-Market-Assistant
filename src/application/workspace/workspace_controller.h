#pragma once

#include "config/import_types.h"
#include "application/workspace/workspace_projection.h"
#include <QObject>
#include <QStringList>
#include <QVector>
#include <memory>

namespace relink::workspace {

// Owns the SQLite connections on its construction thread. Widgets only consume
// projections; all writes are committed before a new projection is published.
class WorkspaceController final : public QObject {
    Q_OBJECT
public:
    explicit WorkspaceController(QObject* parent = nullptr);
    ~WorkspaceController() override;
    bool open(const QString& rootPath, bool readOnly = false);
    bool close();
    const WorkspaceProjection& projection() const;
    QString lastError() const;
    QJsonObject selectedProfileDocument() const;
    QJsonObject runProfileDocument() const;
    static QString builtinProfileId();
    static QString csvCell(QString value);

    bool refresh();
    bool saveReviewedPreview(const QJsonObject& preview, const config::ReviewChoices& choices);
    bool selectProfile(const QString& profileId);
    bool selectBuiltInFixture();
    bool selectRun(const QString& runId);
    bool setMode(const QString& mode); // Demo or Replay, only while stopped.
    void setReviewDirty(bool dirty);
    void discardReviewDraft();
    bool start();
    bool pause();
    bool resume();
    bool step();
    bool advance(int steps = 100); // Coalesces changed() into one notification.
    bool stop();
    bool exportRecordsCsv(const QString& path);

signals:
    void changed();
    void errorOccurred(const QString& message);

private:
    class Impl;
    std::unique_ptr<Impl> d;
};
} // namespace relink::workspace
