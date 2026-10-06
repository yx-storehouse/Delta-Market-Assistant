#pragma once

#include <QMainWindow>
#include <QByteArray>
#include <QJsonObject>
#include <QVector>

#include <functional>

namespace relink::runtime { class ReplayController; }

class AppState;
class QStackedWidget;
class QPushButton;
class QLabel;
class QLineEdit;
class QComboBox;
class QTableWidget;
class QCheckBox;
class QProgressBar;
class QDoubleSpinBox;
class QSpinBox;
class QFrame;
class QAction;
class QResizeEvent;
class MetricCard;
class PriceChart;
class PillButton;
class ArtworkView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(AppState* state, QWidget* parent = nullptr);
    void setPage(int index);
    QString currentPageName() const;
    void refreshAll();

public slots:
    // Deterministic in-memory entry used by the offscreen self-test.  It opens
    // the same read-only dialog as the file import path without touching disk.
    void previewImportDemo();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    QWidget* buildTitleBar();
    QWidget* buildNavigationRail();
    QWidget* buildOverview();
    QWidget* buildFavorites();
    QWidget* buildTasks();
    QWidget* buildPrices();
    QWidget* buildStats();
    QWidget* buildLogs();
    QWidget* buildSettings();
    QWidget* buildRunSettings();
    void refreshOverview();
    void refreshFavorites();
    void refreshFavoriteDetail();
    void refreshTasks();
    void refreshPrices();
    void refreshStats();
    void refreshLogs();
    void refreshRunSettings();
    void refreshReplayProjection();
    void showHelp();
    void editTask(const QString& id = QString(), const QString& skinId = QString());
    void deleteSelectedTasks();
    void setSelectedTasksEnabled(bool enabled);
    void importConfiguration();
    void previewImportBytes(const QByteArray& bytes, const QString& sourceName);
    void showImportPreview(const QJsonObject& preview, const QString& sourceName);
    void exportConfiguration();
    void exportPrices();
    void saveSettings();
    void applyDensity();
    QString selectedTaskId() const;
    QString selectedFavoriteId() const;
    void createInspectorTask();
    void fitWatchlistColumns();

    AppState* m_state;
    bool m_refreshing = false;
    QStackedWidget* m_pages = nullptr;
    QVector<QPushButton*> m_nav;
    QLabel* m_statusBadge = nullptr;
    QPushButton* m_startButton = nullptr;
    QPushButton* m_pauseButton = nullptr;
    relink::runtime::ReplayController* m_replayController = nullptr;
    QLabel* m_replaySourceLabel = nullptr;
    QLabel* m_replayStateLabel = nullptr;
    QLabel* m_replayReasonLabel = nullptr;
    QLabel* m_replayProgressLabel = nullptr;
    QPushButton* m_replayStartButton = nullptr;
    QPushButton* m_replayPauseButton = nullptr;
    QPushButton* m_replayResumeButton = nullptr;
    QPushButton* m_replayStopButton = nullptr;
    QVector<MetricCard*> m_overviewCards;
    PriceChart* m_overviewChart = nullptr;
    QTableWidget* m_overviewTasks = nullptr;
    QTableWidget* m_overviewLogs = nullptr;
    QLabel* m_overviewTaskCount = nullptr;
    QLineEdit* m_favoriteSearch = nullptr;
    QAction* m_searchClear = nullptr;
    QComboBox* m_rarityFilter = nullptr;
    QComboBox* m_favoriteStateFilter = nullptr;
    QTableWidget* m_favoritesTable = nullptr;
    QLabel* m_favoritesCount = nullptr;
    QFrame* m_inspector = nullptr;
    ArtworkView* m_favoriteArt = nullptr;
    QLabel* m_favoriteTitle = nullptr;
    QLabel* m_favoriteMetadata = nullptr;
    QLabel* m_favoritePrice = nullptr;
    QLabel* m_favoriteChange = nullptr;
    QLabel* m_factRarity = nullptr;
    QLabel* m_factCondition = nullptr;
    QLabel* m_factWear = nullptr;
    QLabel* m_favoriteTaskInfo = nullptr;
    QPushButton* m_favoriteCreate = nullptr;
    PriceChart* m_favoriteChart = nullptr;
    QLabel* m_favoriteChartTitle = nullptr;
    QDoubleSpinBox* m_inspectorPrice = nullptr;
    QDoubleSpinBox* m_inspectorWear = nullptr;
    QSpinBox* m_inspectorQuantity = nullptr;
    QComboBox* m_inspectorCondition = nullptr;
    QVector<PillButton*> m_catalogFilters;
    QString m_catalogFilter = QStringLiteral("all");
    QString m_inspectorSkinId;
    QTableWidget* m_tasksTable = nullptr;
    QLabel* m_taskCount = nullptr;
    QPushButton* m_taskStart = nullptr;
    QComboBox* m_priceSkin = nullptr;
    QVector<MetricCard*> m_priceCards;
    PriceChart* m_priceChart = nullptr;
    QTableWidget* m_priceHistory = nullptr;
    QVector<MetricCard*> m_statsCards;
    QLabel* m_statsNote = nullptr;
    QVector<QLabel*> m_reasonLabels;
    QVector<QProgressBar*> m_reasonBars;
    QLineEdit* m_logSearch = nullptr;
    QComboBox* m_logLevel = nullptr;
    QTableWidget* m_logsTable = nullptr;
    QLabel* m_logCount = nullptr;
    QLineEdit* m_configDirectory = nullptr;
    QCheckBox* m_autoScrollLogs = nullptr;
    QCheckBox* m_compactTables = nullptr;
    QLabel* m_settingsMessage = nullptr;
    QLabel* m_runTaskSummary = nullptr;
    QPushButton* m_importPreviewButton = nullptr;
    QVector<std::function<void()>> m_runBinders;
};
