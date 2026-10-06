#pragma once

#include <QVector>
#include <QWidget>
#include <functional>

class AppState;
class QLabel;

namespace relink::ui {
// Owns legacy Demo parameter controls. The optional workspace panel is composed
// as an opaque child: all controller interactions remain in the shell.
class RunSettingsPage final : public QWidget {
    Q_OBJECT
public:
    explicit RunSettingsPage(AppState* state, QWidget* workspacePanel = nullptr,
                             QWidget* parent = nullptr);
    void refresh();
    void showSaveResult(const QString& message);
signals:
    void saveRequested();
    void importPreviewRequested();
    void navigateRequested(int pageIndex);
    void helpRequested();
private:
    AppState* m_state;
    const bool m_workspacePresent;
    QLabel* m_saveMessage = nullptr;
    QLabel* m_runTaskSummary = nullptr;
    QVector<std::function<void()>> m_runBinders;
};
} // namespace relink::ui
