#pragma once

#include "domain.h"
#include <QDialog>

namespace relink::ui {
// The editor has no write access to AppState. Cancelling discards its local
// draft; the owning page commits task() only after QDialog::Accepted.
class TaskEditorDialog final : public QDialog {
    Q_OBJECT
public:
    explicit TaskEditorDialog(const AppState* state, const Task* task = nullptr,
                              const QString& skinId = QString(), QWidget* parent = nullptr);
    Task task() const { return m_task; }
private:
    Task m_task;
};
} // namespace relink::ui
