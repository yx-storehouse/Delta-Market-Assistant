#include "workspace_records_panel.h"
#include "application/workspace/workspace_projection.h"
#include "ui/presentation/ui_helpers.h"
#include "fluenttheme.h"
#include "widgets.h"
#include <QComboBox>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

namespace relink::ui {
WorkspaceRecordsPanel::WorkspaceRecordsPanel(QWidget* parent)
    : QFrame(parent)
{
    auto* panel = this;
    setObjectName(QStringLiteral("workspaceRecordsPanel"));
    setProperty("card", true);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(10);
    auto* heading = new QHBoxLayout;
    heading->addWidget(label(QStringLiteral("回放记录 · 已提交"), QStringLiteral("sectionTitle")));
    heading->addStretch();
    m_workspaceRuns = new FluentComboBox;
    m_workspaceRuns->setObjectName(QStringLiteral("workspaceRunCombo"));
    m_workspaceRuns->setMinimumWidth(300);
    m_workspaceRuns->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_workspaceRuns->setMinimumContentsLength(28);
    heading->addWidget(m_workspaceRuns);
    m_workspaceExport = button(QStringLiteral("导出 CSV"), QStringLiteral("workspaceExportCsvButton"), ButtonKind::Standard, Glyph::Export);
    heading->addWidget(m_workspaceExport);
    layout->addLayout(heading);
    m_workspaceRecordsState = label(QString(), QStringLiteral("workspaceRecordsState"));
    m_workspaceRecordsState->setWordWrap(true);
    layout->addWidget(m_workspaceRecordsState);
    m_workspaceRecords = table({QStringLiteral("序号 / 时点"), QStringLiteral("事件"), QStringLiteral("关联 / 原因"),
                                QStringLiteral("观测 / 模拟成交价"), QStringLiteral("来源 / 时钟")},
                               QStringLiteral("workspaceRecordsTable"), RowStyle::Lines);
    auto* columns = m_workspaceRecords->horizontalHeader();
    columns->setStretchLastSection(false);
    columns->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    // The custom delegate's generic size hint can size this column to the
    // two-character heading. Keep full event names legible instead of "开始…".
    columns->setSectionResizeMode(1, QHeaderView::Fixed);
    m_workspaceRecords->setColumnWidth(1, 124);
    columns->setSectionResizeMode(2, QHeaderView::Stretch);
    columns->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    columns->setSectionResizeMode(4, QHeaderView::Stretch);
    m_workspaceRecords->setMinimumHeight(280);
    m_workspaceRecords->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    layout->addWidget(m_workspaceRecords);
    connect(m_workspaceRuns, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0) emit runSelected(m_workspaceRuns->itemData(index).toString());
    });
    connect(m_workspaceExport, &QPushButton::clicked, this, &WorkspaceRecordsPanel::exportRequested);
}

void WorkspaceRecordsPanel::refresh(const relink::workspace::WorkspaceProjection& view,
                                    const QString& error)
{
    // Replacing the selector content is a render operation, never user intent.
    const QSignalBlocker runsGuard(m_workspaceRuns);
    auto shown = [](const QString& value) { return value.isEmpty() ? QStringLiteral("—") : value; };
    m_workspaceRuns->clear();
    m_workspaceRuns->addItem(QStringLiteral("当前工作区"), QString());
    bool recovered = false;
    for (const auto& run : view.runs) {
        m_workspaceRuns->addItem(QStringLiteral("%1 · r%2 · %3%4").arg(run.profileName).arg(run.profileRevision)
            .arg(workspaceText(run.state), run.recovered && run.state != QStringLiteral("Recovered") ? QStringLiteral(" · 已恢复") : QString()), run.id);
        m_workspaceRuns->setItemData(m_workspaceRuns->count() - 1,
            QStringLiteral("%1\n状态：%2\n来源：%3").arg(run.id, run.state, run.source), Qt::ToolTipRole);
        if (run.id == view.selectedRunId) recovered = run.recovered;
    }
    m_workspaceRuns->setCurrentIndex(qMax(0, m_workspaceRuns->findData(view.selectedRunId)));
    m_workspaceRuns->setEnabled(view.opened && !view.active && !view.dirty);
    m_workspaceRecords->setRowCount(view.records.size());
    for (int i = 0; i < view.records.size(); ++i) {
        const auto& row = view.records[i];
        put(m_workspaceRecords, i, 0, QStringLiteral("%1 · %2 ms").arg(row.seq).arg(row.atMonoMs));
        put(m_workspaceRecords, i, 1, workspaceText(row.type))->setData(Qt::UserRole, row.type);
        put(m_workspaceRecords, i, 2, QStringLiteral("%1 · %2").arg(shown(row.listingId), workspaceText(row.reason)));
        put(m_workspaceRecords, i, 3, QStringLiteral("%1 / %2").arg(shown(row.observedPrice), shown(row.confirmedPrice)));
        put(m_workspaceRecords, i, 4, QStringLiteral("%1 / %2").arg(workspaceText(row.source), shortWorkspaceId(row.clockDomainId)));
        for (int col = 0; col < m_workspaceRecords->columnCount(); ++col)
            m_workspaceRecords->item(i, col)->setToolTip(QStringLiteral("事件 %1\n运行 %2\n类型 %3\n原因 %4\n卖单 %5 / 观察 %6\n来源 %7\n时钟 %8 · %9 ms")
                .arg(row.eventId, row.runId, row.type, row.reason, row.listingId, row.observationId, row.source, row.clockDomainId).arg(row.atMonoMs));
    }
    m_workspaceRecordsState->setText(QStringLiteral("%1 · %2 条已提交记录 · 运行 %3\n固定方案：%4 · 版本 %5 · 记录只读，清空演示日志不影响此账本。%6\n成功 %7 · 失败 %8 · 结果未知 %9 · 仍占用预留 %10%11")
        .arg(view.historyReadOnly ? QStringLiteral("历史运行 / 只读") : QStringLiteral("当前运行"))
        .arg(view.records.size()).arg(shortWorkspaceId(view.selectedRunId), shown(view.runProfileName)).arg(view.runProfileRevision)
        .arg(error.isEmpty() ? QString() : QStringLiteral("\n操作失败：") + error)
        .arg(view.confirmedSuccess).arg(view.failedCount).arg(view.unknownCount).arg(view.reservationCount)
        .arg(recovered ? QStringLiteral(" · 已恢复，不自动继续或重发") : QString()));
    m_workspaceRecordsState->setToolTip(QStringLiteral("运行：%1\n固定方案：%2\n版本：%3")
        .arg(view.selectedRunId, view.runProfileId).arg(view.runProfileRevision));
    m_workspaceExport->setEnabled(view.opened && !view.records.isEmpty());
}

void WorkspaceRecordsPanel::showExportResult(const QString& path)
{
    m_workspaceRecordsState->setText(QStringLiteral("已导出只读记录：%1 · CSV 公式字段已转义").arg(QDir::toNativeSeparators(path)));
}
} // namespace relink::ui
