#include "task_page.h"
#include "domain.h"
#include "fluenttheme.h"
#include "widgets.h"
#include "ui/dialogs/task_editor_dialog.h"
#include "ui/presentation/ui_helpers.h"
#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

namespace relink::ui {
TaskPage::TaskPage(AppState* state, QWidget* parent)
    : QWidget(parent), m_state(state)
{
    auto* page = this;
    page->setObjectName(QStringLiteral("tasksPage"));
    auto* layout = pageLayout(page);
    pageHeader(layout, QStringLiteral("自动任务"), &m_taskCount);
    layout->addSpacing(16);
    auto* commands = new QHBoxLayout;
    commands->setSpacing(4);
    auto* add = button(QStringLiteral("新增任务"), QStringLiteral("taskNewButton"), ButtonKind::Accent, Glyph::Add);
    auto* edit = button(QStringLiteral("编辑"), QStringLiteral("taskEditButton"), ButtonKind::Subtle, Glyph::Edit);
    auto* remove = button(QStringLiteral("删除"), QStringLiteral("taskDeleteButton"), ButtonKind::Subtle, Glyph::Delete);
    auto* enable = button(QStringLiteral("启用选中"), QStringLiteral("enableSelectedTasksButton"), ButtonKind::Subtle, Glyph::CheckMark);
    auto* disable = button(QStringLiteral("暂停选中"), QStringLiteral("pauseSelectedTasksButton"), ButtonKind::Subtle, Glyph::Pause);
    m_taskStart = button(QStringLiteral("运行模拟"), QStringLiteral("tasksSimulationButton"), ButtonKind::Standard, Glyph::Play);
    commands->addWidget(add);
    commands->addSpacing(8);
    commands->addWidget(edit);
    commands->addWidget(remove);
    commands->addWidget(rule(QStringLiteral("filterDivider")));
    commands->addWidget(enable);
    commands->addWidget(disable);
    commands->addStretch();
    commands->addWidget(m_taskStart);
    layout->addLayout(commands);
    layout->addSpacing(12);
    m_tasksTable = table({QStringLiteral("任务名称"), QStringLiteral("目标皮肤"), QStringLiteral("成色"), QStringLiteral("价格区间"),
                          QStringLiteral("最大磨损"), QStringLiteral("数量"), QStringLiteral("启用"), QStringLiteral("状态")},
                         QStringLiteral("taskTable"), RowStyle::Cards);
    auto* columns = m_tasksTable->horizontalHeader();
    columns->setStretchLastSection(false);
    columns->setSectionResizeMode(QHeaderView::Fixed);
    columns->setSectionResizeMode(0, QHeaderView::Stretch);
    columns->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tasksTable->setColumnWidth(2, 76);
    m_tasksTable->setColumnWidth(3, 156);
    m_tasksTable->setColumnWidth(4, 92);
    m_tasksTable->setColumnWidth(5, 64);
    m_tasksTable->setColumnWidth(6, 72);
    m_tasksTable->setColumnWidth(7, 124);
    m_tasksTable->horizontalHeaderItem(6)->setTextAlignment(Qt::AlignCenter);
    m_tasksTable->verticalHeader()->setDefaultSectionSize(56);
    m_tasksTable->setMinimumHeight(330);
    layout->addWidget(m_tasksTable, 1);
    layout->addSpacing(8);
    auto* note = label(QStringLiteral("Ctrl / Shift 多选 · 双击编辑 · 当前仅模拟运行，不执行购买"), QStringLiteral("tertiaryLabel"));
    note->setWordWrap(true);
    layout->addWidget(note);
    connect(add, &QPushButton::clicked, this, [this] { editTask(); });
    connect(edit, &QPushButton::clicked, this, [this] { if (!selectedTaskId().isEmpty()) editTask(selectedTaskId()); });
    connect(m_tasksTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem*) {
        if (!selectedTaskId().isEmpty()) editTask(selectedTaskId());
    });
    connect(remove, &QPushButton::clicked, this, &TaskPage::deleteSelectedTasks);
    connect(enable, &QPushButton::clicked, this, [this] { setSelectedTasksEnabled(true); });
    connect(disable, &QPushButton::clicked, this, [this] { setSelectedTasksEnabled(false); });
    connect(m_taskStart, &QPushButton::clicked, m_state, &AppState::startSimulation);

}

QString TaskPage::selectedTaskId() const { return selectedId(m_tasksTable); }

void TaskPage::refresh()
{
    if (m_refreshing) return;
    QScopedValueRollback<bool> refreshing(m_refreshing, true);
    const QString previous = selectedTaskId();
    QStringList selected;
    for (const auto& row : m_tasksTable->selectionModel()->selectedRows())
        if (auto* item = m_tasksTable->item(row.row(), 0)) selected.append(item->data(Qt::UserRole).toString());
    QSignalBlocker guard(m_tasksTable);
    m_tasksTable->setRowCount(m_state->tasks.size());
    int active = 0;
    for (int row = 0; row < m_state->tasks.size(); ++row) {
        const auto& task = m_state->tasks[row];
        const auto* skin = findSkin(m_state, task.skinId);
        if (task.enabled) ++active;
        put(m_tasksTable, row, 0, task.name, QColor(), task.id);
        put(m_tasksTable, row, 1, skin ? skinChoice(*skin) : QStringLiteral("目标已移除"), skin ? QColor() : FluentTheme::muted);
        put(m_tasksTable, row, 2, task.condition, FluentTheme::secondary);
        put(m_tasksTable, row, 3, QStringLiteral("%1 — %2").arg(amount(task.minPrice), amount(task.maxPrice)));
        put(m_tasksTable, row, 4, QString::number(task.maxWear, 'f', 3), FluentTheme::secondary);
        put(m_tasksTable, row, 5, QString::number(task.quantity));
        auto* toggle = new ToggleSwitch;
        toggle->setObjectName("taskEnabled_" + task.id);
        toggle->setChecked(task.enabled);
        toggle->setToolTip(task.enabled ? QStringLiteral("暂停任务") : QStringLiteral("启用任务"));
        m_tasksTable->setCellWidget(row, 6, centered(toggle));
        connect(toggle, &QCheckBox::toggled, this, [this, id = task.id](bool checked) {
            if (m_refreshing) return;
            for (auto& value : m_state->tasks) {
                if (value.id != id) continue;
                value.enabled = checked;
                value.status = checked ? QStringLiteral("待启动") : QStringLiteral("演示已暂停");
                break;
            }
            m_state->notifyChanged();
        });
        putStatus(m_tasksTable, row, 7, task.status, taskTone(task));
    }
    m_tasksTable->clearSelection();
    for (int row = 0; row < m_tasksTable->rowCount(); ++row) {
        const QString id = m_tasksTable->item(row, 0)->data(Qt::UserRole).toString();
        if (selected.contains(id))
            m_tasksTable->selectionModel()->select(m_tasksTable->model()->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        if (id == previous) m_tasksTable->setCurrentCell(row, 0, QItemSelectionModel::NoUpdate);
    }
    m_taskCount->setText(QStringLiteral("%1 条任务 · %2 条启用").arg(m_state->tasks.size()).arg(active));
}

void TaskPage::editTask(const QString& id, const QString& skinId)
{
    if (m_state->simulationRunning) m_state->pauseSimulation();
    Task existing;
    bool editing = false;
    for (const auto& task : m_state->tasks) if (task.id == id) { existing = task; editing = true; break; }
    TaskEditorDialog dialog(m_state, editing ? &existing : nullptr, skinId, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const Task task = dialog.task();
    if (editing) {
        for (auto& target : m_state->tasks) {
            if (target.id == id) { target = task; break; }
        }
        m_state->addLog("INFO", QStringLiteral("更新本地任务：") + task.name);
        m_state->resetTaskSimulation(task.id);
    } else {
        m_state->tasks.append(task);
        m_state->addLog("INFO", QStringLiteral("新增本地任务：") + task.name);
        m_state->notifyChanged();
    }
}

void TaskPage::deleteSelectedTasks()
{
    QStringList ids;
    for (const auto& index : m_tasksTable->selectionModel()->selectedRows())
        if (auto* item = m_tasksTable->item(index.row(), 0)) ids.append(item->data(Qt::UserRole).toString());
    if (ids.isEmpty()) return;
    if (!confirm(this, QStringLiteral("删除任务"), QStringLiteral("删除选中的 %1 条本地任务？关注列表保持不变。").arg(ids.size()), QStringLiteral("删除")))
        return;
    for (int i = m_state->tasks.size() - 1; i >= 0; --i) {
        if (ids.contains(m_state->tasks[i].id)) m_state->tasks.removeAt(i);
    }
    m_state->addLog("INFO", QStringLiteral("删除 %1 条本地任务").arg(ids.size()));
    m_state->notifyChanged();
}

void TaskPage::setSelectedTasksEnabled(bool enabled)
{
    QStringList ids;
    for (const auto& index : m_tasksTable->selectionModel()->selectedRows())
        if (auto* item = m_tasksTable->item(index.row(), 0)) ids.append(item->data(Qt::UserRole).toString());
    if (ids.isEmpty()) return;
    for (auto& task : m_state->tasks) {
        if (ids.contains(task.id)) {
            task.enabled = enabled;
            task.status = enabled ? QStringLiteral("待启动") : QStringLiteral("演示已暂停");
        }
    }
    m_state->addLog("INFO", QStringLiteral("%1 %2 条选中任务").arg(enabled ? QStringLiteral("启用") : QStringLiteral("暂停")).arg(ids.size()));
    m_state->notifyChanged();
}

void TaskPage::setSimulationAvailable(bool available)
{
    m_taskStart->setEnabled(available);
}

void TaskPage::setCompact(bool compact)
{
    m_tasksTable->verticalHeader()->setDefaultSectionSize(compact ? 46 : 56);
}
} // namespace relink::ui
