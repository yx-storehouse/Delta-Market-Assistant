#pragma once

#include <QByteArray>
#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QWidget;

namespace relink::catalog { class CatalogStore; }

namespace relink::ui {
// The owner retains the store. All mutations go through its validated,
// atomic append operations; this dialog has no runtime/game dependencies.
class SkinCatalogDialog final : public QDialog {
    Q_OBJECT
public:
    explicit SkinCatalogDialog(catalog::CatalogStore* store, QWidget* parent = nullptr);
    void refresh();
    // Separate from the file picker so import behavior can be tested offscreen.
    bool importCatalogJson(const QByteArray& bytes, QString* error = nullptr);

signals:
    void catalogChanged();

private:
    void refreshTable();
    void updateSeasonFields();
    void addSkin();
    void importFile();
    void exportFile(bool extensionsOnly);
    void exportTemplate();
    void showMessage(const QString& message, bool error = false);

    catalog::CatalogStore* m_store = nullptr;
    QTabWidget* m_tabs = nullptr;
    QTableWidget* m_table = nullptr;
    QLabel* m_count = nullptr;
    QLabel* m_message = nullptr;
    QLineEdit* m_search = nullptr;
    QComboBox* m_filter = nullptr;
    QComboBox* m_season = nullptr;
    QSpinBox* m_seasonNumber = nullptr;
    QLineEdit* m_seasonName = nullptr;
    QWidget* m_seasonFields = nullptr;
    QLineEdit* m_productId = nullptr;
    QLineEdit* m_weapon = nullptr;
    QLineEdit* m_skinSeries = nullptr;
    QComboBox* m_variant = nullptr;
    QComboBox* m_grade = nullptr;
};
} // namespace relink::ui
