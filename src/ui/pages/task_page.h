#pragma once

#include <QWidget>

class AppState;
class QLabel;
class QPushButton;
class QTableWidget;

namespace relink::ui {
// Legacy Demo task editor boundary. This page owns its widgets and mutations;
// it neither knows MainWindow nor accesses the persistent workspace controller.
class TaskPage final : public QWidget {
    Q_OBJECT
public:
    explicit TaskPage(AppState* state, QWidget* parent = nullptr);
    void refresh();
    void setSimulationAvailable(bool available);
    void setCompact(bool compact);
    void editTask(const QString& id = QString(), const QString& skinId = QString());
private:
    QString selectedTaskId() const;
    void deleteSelectedTasks();
    void setSelectedTasksEnabled(bool enabled);
    AppState* m_state;
    bool m_refreshing = false;
    QTableWidget* m_tasksTable = nullptr;
    QLabel* m_taskCount = nullptr;
    QPushButton* m_taskStart = nullptr;
};
} // namespace relink::ui
