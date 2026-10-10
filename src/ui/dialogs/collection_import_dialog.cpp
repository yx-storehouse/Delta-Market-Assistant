#include "collection_import_dialog.h"
#include "fluenttheme.h"
#include "ui/presentation/ui_helpers.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <utility>

namespace relink::ui {
namespace {
QString gradeText(const QString& grade) { return grade.isEmpty() ? QStringLiteral("未记录") : grade; }
QString number(double value) { return QString::number(value, 'g', 15); }
}

CollectionImportDialog::CollectionImportDialog(const QJsonObject& dictionary,
        const catalog::Catalog* catalog, const AppState* state, QWidget* parent)
    : QDialog(parent), m_dictionary(dictionary), m_catalog(catalog), m_state(state)
{
    setObjectName(QStringLiteral("collectionImportDialog"));
    setWindowTitle(QStringLiteral("导入收藏任务"));
    resize(1120, 700);
    setMinimumSize(850, 540);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* content = new QFrame;
    content->setObjectName(QStringLiteral("dialogContent"));
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(24, 22, 24, 20);
    body->setSpacing(12);
    auto* heading = new QHBoxLayout;
    heading->addWidget(label(QStringLiteral("导入收藏任务"), QStringLiteral("dialogTitle")));
    heading->addStretch();
    m_choose = button(QStringLiteral("选择文件"), QStringLiteral("collectionImportChooseButton"),
                      ButtonKind::Standard, Glyph::Folder);
    heading->addWidget(m_choose);
    body->addLayout(heading);
    auto* caption = label(QStringLiteral("读取 .savedValue 中的收藏条件，确认后追加到任务列表。原有任务、关注与运行设置保持不变。"),
                          QStringLiteral("cardCaption"));
    caption->setWordWrap(true);
    body->addWidget(caption);
    m_file = label(QStringLiteral("尚未选择文件"), QStringLiteral("collectionImportFile"));
    m_file->setTextFormat(Qt::PlainText);
    m_file->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_file->setWordWrap(true);
    body->addWidget(m_file);
    m_hash = label(QString(), QStringLiteral("collectionImportHash"));
    m_hash->setTextFormat(Qt::PlainText);
    m_hash->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_hash->setStyleSheet(QStringLiteral("color: #5E5E5E; font-size: 11px;"));
    m_hash->setWordWrap(true);
    body->addWidget(m_hash);
    m_counts = label(QStringLiteral("选择文件后预览全部任务"), QStringLiteral("collectionImportCounts"));
    m_counts->setTextFormat(Qt::PlainText);
    m_counts->setWordWrap(true);
    body->addWidget(m_counts);
    m_table = table({QStringLiteral("行"), QStringLiteral("赛季"), QStringLiteral("商品"),
                     QStringLiteral("品阶 / 标记"), QStringLiteral("成色"), QStringLiteral("最低价格"),
                     QStringLiteral("最高价格"), QStringLiteral("最大磨损"), QStringLiteral("限量"),
                     QStringLiteral("启用"), QStringLiteral("处理")},
                    QStringLiteral("collectionImportTable"), RowStyle::Lines);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto* header = m_table->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    const int widths[] = {46, 60, 220, 114, 72, 94, 94, 88, 58, 58, 74};
    for (int i = 0; i < 11; ++i) m_table->setColumnWidth(i, widths[i]);
    header->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->verticalHeader()->setDefaultSectionSize(40);
    body->addWidget(m_table, 1);
    auto* note = label(QStringLiteral("限量 0 表示不限；停用行保持停用；相同文件再次导入会跳过已导入行。"),
                       QStringLiteral("cardCaption"));
    note->setWordWrap(true);
    body->addWidget(note);
    m_message = label(QString(), QStringLiteral("collectionImportMessage"));
    m_message->setTextFormat(Qt::PlainText);
    m_message->setWordWrap(true);
    body->addWidget(m_message);
    outer->addWidget(content, 1);
    auto* commands = new QFrame;
    commands->setObjectName(QStringLiteral("dialogCommands"));
    auto* row = new QHBoxLayout(commands);
    row->setContentsMargins(24, 18, 24, 18);
    row->setSpacing(8);
    row->addStretch();
    m_commit = button(QStringLiteral("确认追加"), QStringLiteral("collectionImportCommitButton"), ButtonKind::Accent);
    m_commit->setMinimumWidth(160);
    m_commit->setEnabled(false);
    m_commit->setDefault(true);
    auto* cancel = button(QStringLiteral("取消"), QStringLiteral("collectionImportCancelButton"));
    cancel->setMinimumWidth(100);
    row->addWidget(m_commit);
    row->addWidget(cancel);
    outer->addWidget(commands);
    connect(m_choose, &QPushButton::clicked, this, &CollectionImportDialog::selectFile);
    connect(m_commit, &QPushButton::clicked, this, &CollectionImportDialog::requestCommit);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
}

void CollectionImportDialog::showMessage(const QString& text, bool error) {
    m_message->setText(text);
    m_message->setStyleSheet(error ? QStringLiteral("color: #C42B1C;") : QStringLiteral("color: #5E5E5E;"));
}

void CollectionImportDialog::clearPreview(const QString& error) {
    m_preview = {};
    m_bytes.clear();
    m_sourceName.clear();
    m_table->setRowCount(0);
    m_file->setText(QStringLiteral("文件未通过校验"));
    m_hash->clear();
    m_counts->setText(QStringLiteral("未追加任务，现有数据保持不变"));
    m_commit->setEnabled(false);
    m_commit->setText(QStringLiteral("确认追加"));
    showMessage(error, true);
}

bool CollectionImportDialog::loadSavedValue(const QByteArray& bytes, const QString& sourceName, QString* error) {
    if (m_committing) {
        if (error) *error = QStringLiteral("正在保存当前导入，请等待保存完成。");
        return false;
    }
    application::CollectionTaskImportPreview next;
    QString failure;
    const bool valid = m_catalog && m_state && application::previewCollectionTaskImport(
        bytes, m_dictionary, *m_catalog, *m_state, &next, &failure);
    if (!valid) {
        if (failure.isEmpty()) failure = QStringLiteral("皮肤目录尚未就绪。");
        clearPreview(failure);
        if (error) *error = failure;
        return false;
    }
    m_bytes = bytes;
    m_sourceName = sourceName;
    m_preview = std::move(next);
    renderPreview();
    if (error) error->clear();
    return true;
}

bool CollectionImportDialog::loadSavedValueFile(const QString& path, QString* error) {
    QFile file(path);
    QString failure;
    if (!file.open(QIODevice::ReadOnly)) failure = QStringLiteral("读取文件失败：%1").arg(file.errorString());
    else if (file.size() <= 0 || file.size() > 8 * 1024 * 1024)
        failure = QStringLiteral("请选择非空且不超过 8 MiB 的 .savedValue 文件。");
    if (!failure.isEmpty()) {
        if (!m_committing) clearPreview(failure);
        if (error) *error = failure;
        return false;
    }
    return loadSavedValue(file.read(8 * 1024 * 1024 + 1), QFileInfo(path).fileName(), error);
}

void CollectionImportDialog::selectFile() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择收藏任务文件"), QString(),
                                                     QStringLiteral("收藏任务 (*.savedValue);;所有文件 (*)"));
    if (!path.isEmpty()) loadSavedValueFile(path);
}

void CollectionImportDialog::renderPreview() {
    m_file->setText(m_sourceName);
    m_hash->setText(QStringLiteral("SHA-256  %1").arg(m_preview.sourceSha256));
    m_counts->setText(QStringLiteral("%1 行任务  ·  %2 行启用  ·  %3 个商品（启用 %4 个）  ·  追加 %5 行  ·  跳过 %6 行")
                     .arg(m_preview.rowCount).arg(m_preview.enabledCount).arg(m_preview.totalDistinctProducts)
                     .arg(m_preview.distinctProducts).arg(m_preview.addedCount).arg(m_preview.duplicateCount));
    m_table->setRowCount(m_preview.rows.size());
    for (int row = 0; row < m_preview.rows.size(); ++row) {
        const auto& item = m_preview.rows[row];
        const auto& task = item.task;
        put(m_table, row, 0, QString::number(task.importSource.row + 1));
        put(m_table, row, 1, item.seasonId)->setToolTip(item.seasonId + QStringLiteral(" · ") + item.seasonLabel);
        auto* product = put(m_table, row, 2, item.displayName);
        product->setToolTip(item.displayName + QStringLiteral("\n商品 ID：") + item.productId);
        product->setData(Qt::UserRole, item.productId);
        put(m_table, row, 3, gradeText(item.grade) + (item.variantLabel.isEmpty() ? QString() : QStringLiteral(" / ") + item.variantLabel));
        put(m_table, row, 4, task.condition);
        put(m_table, row, 5, number(task.minPrice));
        put(m_table, row, 6, number(task.maxPrice));
        put(m_table, row, 7, number(task.maxWear));
        put(m_table, row, 8, task.quantity == 0 ? QStringLiteral("不限") : QString::number(task.quantity));
        put(m_table, row, 9, task.enabled ? QStringLiteral("启用") : QStringLiteral("停用"));
        put(m_table, row, 10, item.alreadyImported ? QStringLiteral("已导入") : QStringLiteral("待追加"));
    }
    m_commit->setEnabled(m_preview.addedCount > 0 && !m_committing);
    m_commit->setText(m_preview.addedCount > 0
        ? QStringLiteral("确认追加 %1 行").arg(m_preview.addedCount) : QStringLiteral("无需追加"));
    showMessage(m_preview.addedCount == 0
        ? QStringLiteral("这些任务已导入，没有新增行；既有任务的后续修改不会被覆盖。")
        : QStringLiteral("请核对条件；确认后保存到本地，不自动开始任务。"));
}

void CollectionImportDialog::requestCommit() {
    if (m_committing || m_bytes.isEmpty()) return;
    const QByteArray bytes = m_bytes;
    const QString name = m_sourceName;
    if (!loadSavedValue(bytes, name) || m_preview.addedCount <= 0) return;
    m_committing = true;
    m_commit->setEnabled(false);
    m_choose->setEnabled(false);
    showMessage(QStringLiteral("正在保存收藏任务…"));
    emit commitRequested();
}

void CollectionImportDialog::showCommitError(const QString& error) {
    m_committing = false;
    m_choose->setEnabled(true);
    m_commit->setEnabled(!m_bytes.isEmpty() && m_preview.addedCount > 0);
    showMessage(error, true);
}
} // namespace relink::ui
