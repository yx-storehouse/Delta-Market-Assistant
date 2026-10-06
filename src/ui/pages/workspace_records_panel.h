#pragma once

#include <QFrame>

namespace relink::workspace { struct WorkspaceProjection; }
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace relink::ui {
// A read-only projection view. The shell handles selection/export commands;
// rendering has no controller, database, filesystem, or AppState dependency.
class WorkspaceRecordsPanel final : public QFrame {
    Q_OBJECT
public:
    explicit WorkspaceRecordsPanel(QWidget* parent = nullptr);
    void refresh(const relink::workspace::WorkspaceProjection& view, const QString& error = QString());
    void showExportResult(const QString& path);
signals:
    void runSelected(const QString& runId);
    void exportRequested();
private:
    QComboBox* m_workspaceRuns = nullptr;
    QLabel* m_workspaceRecordsState = nullptr;
    QTableWidget* m_workspaceRecords = nullptr;
    QPushButton* m_workspaceExport = nullptr;
};
} // namespace relink::ui
