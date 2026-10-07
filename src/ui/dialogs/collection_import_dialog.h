#pragma once

#include "application/collection_task_import.h"
#include <QDialog>

class QLabel;
class QPushButton;
class QTableWidget;

namespace relink::ui {
// A read-only import preview. The owner persists preview().mergedTasks before
// accepting the dialog; this widget never mutates AppState or starts a runner.
class CollectionImportDialog final : public QDialog {
    Q_OBJECT
public:
    explicit CollectionImportDialog(const QJsonObject& dictionary,
                                    const catalog::Catalog* catalog,
                                    const AppState* state, QWidget* parent = nullptr);
    bool loadSavedValue(const QByteArray& bytes, const QString& sourceName,
                        QString* error = nullptr);
    bool loadSavedValueFile(const QString& path, QString* error = nullptr);
    const application::CollectionTaskImportPreview& preview() const { return m_preview; }
    const QByteArray& sourceBytes() const { return m_bytes; }
    QString sourceName() const { return m_sourceName; }
    void showCommitError(const QString& error);

signals:
    // The owner calls accept() after a durable commit or showCommitError() on
    // failure. No success state is shown merely because the button was pressed.
    void commitRequested();

private:
    void selectFile();
    void requestCommit();
    void renderPreview();
    void showMessage(const QString& text, bool error = false);
    void clearPreview(const QString& error);

    QJsonObject m_dictionary;
    const catalog::Catalog* m_catalog = nullptr;
    const AppState* m_state = nullptr;
    application::CollectionTaskImportPreview m_preview;
    QByteArray m_bytes;
    QString m_sourceName;
    QLabel* m_file = nullptr;
    QLabel* m_hash = nullptr;
    QLabel* m_counts = nullptr;
    QLabel* m_message = nullptr;
    QTableWidget* m_table = nullptr;
    QPushButton* m_choose = nullptr;
    QPushButton* m_commit = nullptr;
    bool m_committing = false;
};
} // namespace relink::ui
