#include "mainwindow.h"
#include "domain.h"
#include "catalog/skin_catalog.h"
#include "application/catalog_configuration.h"
#include "ui/dialogs/skin_catalog_dialog.h"
#include "config/v1_adapter.h"
#include "application/runtime/replay_controller.h"
#include "application/runtime/ui_projection.h"
#include "application/workspace/workspace_controller.h"
#include "widgets.h"
#include "fluenttheme.h"
#include "ui/presentation/ui_helpers.h"
#include "ui/pages/task_page.h"
#include "ui/pages/run_settings_page.h"
#include "ui/pages/workspace_records_panel.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTime>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

using namespace relink::ui;

namespace {

const QStringList titles = {QStringLiteral("工作台"), QStringLiteral("我的关注"), QStringLiteral("自动任务"),
                            QStringLiteral("价格中心"), QStringLiteral("运行统计"), QStringLiteral("运行日志"),
                            QStringLiteral("设置"), QStringLiteral("运行设置")};
constexpr int RunPage = 7;
QString menuColorName(const QString& color) {
    if (color == "red") return QStringLiteral("红色");
    if (color == "orange") return QStringLiteral("橙色");
    if (color == "purple") return QStringLiteral("紫色");
    if (color == "blue") return QStringLiteral("蓝色");
    return QStringLiteral("未记录");
}

// Review edits remain visible on save failure; Escape and the title-bar close
// take the same explicit discard path as the Cancel command.
class ReviewDialog final : public QDialog {
public:
    explicit ReviewDialog(QWidget* parent) : QDialog(parent) {}
    bool dirty = false;
    void reject() override {
        if (dirty) {
            QMessageBox prompt(QMessageBox::Question, QStringLiteral("保留审定草稿"),
                QStringLiteral("审定内容尚未保存。继续编辑，或明确放弃本次草稿？"),
                QMessageBox::NoButton, this);
            prompt.setObjectName(QStringLiteral("profileReviewDiscardDialog"));
            auto* keep = prompt.addButton(QStringLiteral("继续编辑"), QMessageBox::RejectRole);
            auto* discard = prompt.addButton(QStringLiteral("放弃草稿"), QMessageBox::DestructiveRole);
            prompt.setDefaultButton(keep);
            prompt.exec();
            if (prompt.clickedButton() != discard) return;
        }
        QDialog::reject();
    }
protected:
    void closeEvent(QCloseEvent* event) override {
        event->ignore();
        reject();
    }
};

class AppMark final : public QWidget
{
public:
    explicit AppMark(int size) { setFixedSize(size, size); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        FluentTheme::paintAppIcon(&painter, QRectF(rect()));
    }
};

} // namespace

MainWindow::MainWindow(AppState* state, QWidget* parent, relink::workspace::WorkspaceController* workspace, relink::catalog::CatalogStore* catalog)
    : QMainWindow(parent), m_state(state), m_catalog(catalog), m_workspace(workspace)
{
    if (m_catalog) {
        // A user extension must also fit existing config identities BEFORE the
        // store commits it; failed imports leave both files and memory intact.
        QPointer<AppState> owner(m_state);
        m_catalog->setCommitValidator([owner](const relink::catalog::Catalog& candidate, QString* error) {
            if (!owner) { if (error) *error = QStringLiteral("配置窗口已关闭，请重新打开程序。"); return false; }
            AppState draft;
            draft.skins = owner->skins; draft.tasks = owner->tasks; draft.run = owner->run;
            return relink::application::applyCatalogConfiguration(draft, candidate, nullptr, error);
        });
    }
    if (!m_workspace && !m_catalog) {
        m_replayController = new relink::runtime::ReplayController(this);
        connect(m_replayController, &relink::runtime::ReplayController::changed, this, &MainWindow::refreshReplayProjection);
    }
    m_savedConfiguration = QJsonDocument(encodeV1Config(m_state->skins, m_state->tasks, m_state->run)).toJson(QJsonDocument::Compact);
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(QStringLiteral("Relink Studio"));
    setWindowIcon(FluentTheme::appIcon());
    resize(1560, 980);
    setMinimumSize(1180, 820);
    auto* central = new QWidget;
    central->setObjectName(QStringLiteral("appShell"));
    setCentralWidget(central);
    auto* shell = new QVBoxLayout(central);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);
    shell->addWidget(buildTitleBar());
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(buildNavigationRail());
    // NavigationView content layer: lighter than Mica, rounded top-left corner.
    auto* layer = new QFrame;
    layer->setObjectName(QStringLiteral("contentLayer"));
    auto* layerLayout = new QVBoxLayout(layer);
    layerLayout->setContentsMargins(1, 1, 0, 0);
    layerLayout->setSpacing(0);
    m_pages = new QStackedWidget;
    m_pages->setObjectName(QStringLiteral("pageStack"));
    m_pages->addWidget(scrollablePage(buildOverview()));
    m_pages->addWidget(buildFavorites());
    m_pages->addWidget(scrollablePage(buildTasks()));
    m_pages->addWidget(scrollablePage(buildPrices()));
    m_pages->addWidget(scrollablePage(buildStats()));
    m_pages->addWidget(scrollablePage(buildLogs()));
    m_pages->addWidget(scrollablePage(buildSettings()));
    m_pages->addWidget(scrollablePage(buildRunSettings()));
    layerLayout->addWidget(m_pages);
    body->addWidget(layer, 1);
    shell->addLayout(body, 1);
    connect(m_state, &AppState::changed, this, &MainWindow::refreshAll);
    if (m_workspace) {
        connect(m_workspace, &relink::workspace::WorkspaceController::changed, this, &MainWindow::refreshWorkspace);
        connect(m_workspace, &relink::workspace::WorkspaceController::errorOccurred, this, [this](const QString&) { refreshWorkspace(); });
    }
    QSettings preferences;
    m_autoScrollLogs->setChecked(preferences.value(QStringLiteral("ui/autoScrollLogs"), true).toBool());
    m_compactTables->setChecked(preferences.value(QStringLiteral("ui/compactTables"), false).toBool());
    applyDensity();
    setPage(1);
    refreshAll();
}

QWidget* MainWindow::buildTitleBar()
{
    auto* bar = new QFrame;
    bar->setObjectName(QStringLiteral("titleBar"));
    bar->setFixedHeight(52);
    auto* actions = new QHBoxLayout(bar);
    actions->setContentsMargins(16, 8, 16, 10);
    actions->setSpacing(4);
    actions->addStretch();
    // Store-style search, centred on the window by resizeEvent; it filters the watchlist.
    m_favoriteSearch = new QLineEdit(bar);
    m_favoriteSearch->setObjectName(QStringLiteral("favoriteSearch"));
    m_favoriteSearch->setPlaceholderText(QStringLiteral("搜索皮肤、系列"));
    m_favoriteSearch->setFixedSize(520, 34);
    auto* searchAction = m_favoriteSearch->addAction(
        FluentTheme::glyphIcon(Glyph::Search, FluentTheme::secondary, FluentTheme::text), QLineEdit::TrailingPosition);
    searchAction->setToolTip(QStringLiteral("搜索关注条目"));
    m_searchClear = m_favoriteSearch->addAction(
        FluentTheme::glyphIcon(Glyph::Clear, FluentTheme::secondary, FluentTheme::text), QLineEdit::TrailingPosition);
    m_searchClear->setToolTip(QStringLiteral("清除"));
    m_searchClear->setVisible(false);
    connect(searchAction, &QAction::triggered, this, [this] { setPage(1); m_favoriteSearch->setFocus(); });
    connect(m_searchClear, &QAction::triggered, m_favoriteSearch, &QLineEdit::clear);
    auto* importButton = button(QStringLiteral("导入"), QStringLiteral("importConfigButton"), ButtonKind::Subtle, Glyph::Import);
    auto* exportButton = button(QStringLiteral("导出"), QStringLiteral("exportConfigButton"), ButtonKind::Subtle, Glyph::Export);
    auto* helpButton = button(QString(), QStringLiteral("helpButton"), ButtonKind::Subtle, Glyph::Help);
    helpButton->setToolTip(QStringLiteral("帮助"));
    helpButton->setAccessibleName(QStringLiteral("帮助"));
    helpButton->setFixedWidth(36);
    actions->addWidget(importButton);
    actions->addWidget(exportButton);
    actions->addWidget(helpButton);
    actions->addSpacing(8);
    m_statusBadge = label(QString(), QStringLiteral("demoBadge"));
    m_statusBadge->setTextFormat(Qt::RichText);
    actions->addWidget(m_statusBadge);
    connect(importButton, &QPushButton::clicked, this, &MainWindow::importConfiguration);
    connect(exportButton, &QPushButton::clicked, this, &MainWindow::exportConfiguration);
    connect(helpButton, &QPushButton::clicked, this, &MainWindow::showHelp);
    return bar;
}

QWidget* MainWindow::buildNavigationRail()
{
    auto* rail = new QFrame;
    rail->setObjectName(QStringLiteral("navRail"));
    rail->setFixedWidth(76);
    auto* column = new QVBoxLayout(rail);
    column->setContentsMargins(6, 2, 6, 12);
    column->setSpacing(4);
    struct Entry { int page; const char* name; const char* text; char16_t glyph; char16_t selectedGlyph; };
    const Entry entries[] = {
        {1, "navFavorites", "关注", Glyph::Star, Glyph::StarFill},
        {2, "navTasks", "任务", Glyph::Tasks, 0},
        {RunPage, "navRun", "运行", Glyph::Lightning, 0},
        {3, "navPrices", "价格", Glyph::Trend, 0},
        {0, "navOverview", "工作台", Glyph::Home, Glyph::HomeFill},
        {4, "navStats", "统计", Glyph::Pie, 0},
        {5, "navLogs", "日志", Glyph::History, 0},
        {6, "navSettings", "设置", Glyph::Settings, Glyph::SettingsFill},
    };
    m_nav.resize(titles.size());
    for (const auto& entry : entries) {
        if (entry.page == 6) column->addStretch();
        auto* nav = new NavRailButton(QString::fromUtf8(entry.text), entry.glyph, entry.selectedGlyph);
        nav->setObjectName(QString::fromLatin1(entry.name));
        column->addWidget(nav, 0, Qt::AlignHCenter);
        m_nav[entry.page] = nav;
        connect(nav, &QPushButton::clicked, this, [this, page = entry.page] { setPage(page); });
    }
    return rail;
}

QWidget* MainWindow::buildOverview()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("overviewPage"));
    auto* layout = pageLayout(page);
    auto* header = pageHeader(layout, titles[0]);
    m_pauseButton = button(QStringLiteral("暂停"), QStringLiteral("pauseSimulationButton"), ButtonKind::Standard, Glyph::Pause);
    m_startButton = button(QStringLiteral("开始模拟"), QStringLiteral("startSimulationButton"), ButtonKind::Accent, Glyph::Play);
    header->addWidget(m_pauseButton, 0, Qt::AlignVCenter);
    header->addWidget(m_startButton, 0, Qt::AlignVCenter);
    connect(m_startButton, &QPushButton::clicked, m_state, &AppState::startSimulation);
    connect(m_pauseButton, &QPushButton::clicked, m_state, &AppState::pauseSimulation);
    layout->addSpacing(20);

    auto* strip = card(QStringLiteral("metricStrip"));
    auto* metrics = new QHBoxLayout(strip);
    metrics->setContentsMargins(0, 8, 0, 8);
    metrics->setSpacing(0);
    const QStringList names = {QStringLiteral("我的关注"), QStringLiteral("启用任务"), QStringLiteral("扫描条目"), (m_catalog ? QStringLiteral("确认结果") : QStringLiteral("模拟确认"))};
    for (int i = 0; i < names.size(); ++i) {
        auto* metric = new MetricCard(names[i], QStringLiteral("0"), QString());
        metric->setObjectName(QStringLiteral("overviewMetric%1").arg(i));
        metric->setProperty("separator", i < names.size() - 1);
        m_overviewCards.append(metric);
        metrics->addWidget(metric, 1);
    }
    layout->addWidget(strip);

    // Persistent replay commands share the injected workspace controller. The
    // independent v1 demo remains available only in its explicit Demo mode.
    auto* replayPanel = card(QStringLiteral("replayPanel"));
    auto* replayLayout = new QGridLayout(replayPanel);
    replayLayout->setContentsMargins(16, 12, 16, 12);
    replayLayout->setHorizontalSpacing(16);
    replayLayout->setVerticalSpacing(6);
    auto* replayTitle = label(m_workspace ? QStringLiteral("回放工作区 · 已提交账本") : QStringLiteral("M1 回放验证"), QStringLiteral("replayTitle"));
    replayTitle->setProperty("section", true);
    replayLayout->addWidget(replayTitle, 0, 0, 1, 2);
    m_replaySourceLabel = label(QString(), QStringLiteral("replaySourceLabel"));
    m_replayStateLabel = label(QString(), QStringLiteral("replayStateLabel"));
    m_replayReasonLabel = label(QString(), QStringLiteral("replayReasonLabel"));
    m_replayProgressLabel = label(QString(), QStringLiteral("replayProgressLabel"));
    m_replayReasonLabel->setWordWrap(true);
    m_replayProgressLabel->setProperty("tertiary", true);
    replayLayout->addWidget(label(QStringLiteral("来源"), QStringLiteral("replaySourceCaption")), 1, 0);
    replayLayout->addWidget(m_replaySourceLabel, 1, 1);
    replayLayout->addWidget(label(QStringLiteral("运行状态"), QStringLiteral("replayStateCaption")), 2, 0);
    replayLayout->addWidget(m_replayStateLabel, 2, 1);
    replayLayout->addWidget(label(QStringLiteral("匹配说明"), QStringLiteral("replayReasonCaption")), 3, 0, Qt::AlignTop);
    replayLayout->addWidget(m_replayReasonLabel, 3, 1);
    replayLayout->addWidget(m_replayProgressLabel, 4, 0, 1, 2);
    auto* replayCommands = new QHBoxLayout;
    replayCommands->setContentsMargins(0, 4, 0, 0);
    replayCommands->setSpacing(6);
    m_replayStartButton = button(QStringLiteral("开始回放"), QStringLiteral("replayStartButton"), ButtonKind::Accent, Glyph::Play);
    m_replayPauseButton = button(QStringLiteral("暂停回放"), QStringLiteral("replayPauseButton"), ButtonKind::Standard, Glyph::Pause);
    m_replayResumeButton = button(QStringLiteral("继续回放"), QStringLiteral("replayResumeButton"), ButtonKind::Standard, Glyph::Play);
    m_replayStopButton = button(QStringLiteral("停止回放"), QStringLiteral("replayStopButton"), ButtonKind::Subtle, Glyph::Clear);
    replayCommands->addWidget(m_replayStartButton);
    replayCommands->addWidget(m_replayPauseButton);
    replayCommands->addWidget(m_replayResumeButton);
    if (m_workspace) {
        m_replayStepButton = button(QStringLiteral("单步"), QStringLiteral("replayStepButton"));
        m_replayAdvanceButton = button(QStringLiteral("快速推进"), QStringLiteral("replayAdvanceButton"));
        replayCommands->addWidget(m_replayStepButton);
        replayCommands->addWidget(m_replayAdvanceButton);
        connect(m_replayStepButton, &QPushButton::clicked, this, [this] { m_workspace->step(); });
        connect(m_replayAdvanceButton, &QPushButton::clicked, this, [this] { m_workspace->advance(); });
    }
    replayCommands->addWidget(m_replayStopButton);
    replayCommands->addStretch();
    if (m_workspace) {
        auto* manage = button(QStringLiteral("选择方案"), QStringLiteral("workspaceManageButton"), ButtonKind::Subtle);
        replayCommands->addWidget(manage);
        connect(manage, &QPushButton::clicked, this, [this] { setPage(RunPage); });
    }
    replayLayout->addLayout(replayCommands, 5, 0, 1, 2);
    layout->addWidget(replayPanel);
    replayPanel->setVisible(!m_catalog);
    m_startButton->setVisible(!m_catalog);
    m_pauseButton->setVisible(!m_catalog);
    layout->addSpacing(28);
    if (m_workspace) {
        connect(m_replayStartButton, &QPushButton::clicked, this, [this] { if (confirmWorkspaceTransition(QStringLiteral("开始回放"))) m_workspace->start(); });
        connect(m_replayPauseButton, &QPushButton::clicked, this, [this] { m_workspace->pause(); });
        connect(m_replayResumeButton, &QPushButton::clicked, this, [this] { m_workspace->resume(); });
        connect(m_replayStopButton, &QPushButton::clicked, this, [this] { m_workspace->stop(); });
    } else if (m_replayController) {
        connect(m_replayStartButton, &QPushButton::clicked, m_replayController, &relink::runtime::ReplayController::startReplay);
        connect(m_replayPauseButton, &QPushButton::clicked, m_replayController, &relink::runtime::ReplayController::pauseReplay);
        connect(m_replayResumeButton, &QPushButton::clicked, m_replayController, &relink::runtime::ReplayController::resumeReplay);
        connect(m_replayStopButton, &QPushButton::clicked, m_replayController, &relink::runtime::ReplayController::stopReplay);
    }

    auto* middle = new QHBoxLayout;
    middle->setSpacing(16);
    auto* chartColumn = new QVBoxLayout;
    chartColumn->setSpacing(0);
    auto* pricesLink = link(QStringLiteral("查看全部价格"), QStringLiteral("overviewPricesLink"));
    sectionHeader(chartColumn, QStringLiteral("价格观察"))->addWidget(pricesLink);
    auto* chartPanel = card(QStringLiteral("overviewChartPanel"));
    auto* chartLayout = new QVBoxLayout(chartPanel);
    chartLayout->setContentsMargins(20, 16, 20, 12);
    m_overviewChart = new PriceChart;
    m_overviewChart->setObjectName(QStringLiteral("overviewPriceChart"));
    m_overviewChart->setMinimumHeight(240);
    chartLayout->addWidget(m_overviewChart, 1);
    chartColumn->addWidget(chartPanel, 1);
    middle->addLayout(chartColumn, 3);

    auto* taskColumn = new QVBoxLayout;
    taskColumn->setSpacing(0);
    auto* taskHeader = sectionHeader(taskColumn, QStringLiteral("任务队列"));
    m_overviewTaskCount = label(QString(), QStringLiteral("tertiaryLabel"));
    taskHeader->insertWidget(1, m_overviewTaskCount, 0, Qt::AlignVCenter);
    auto* tasksLink = link(QStringLiteral("管理任务"), QStringLiteral("overviewTasksLink"));
    taskHeader->addWidget(tasksLink);
    auto* taskPanel = card(QStringLiteral("overviewTaskPanel"));
    auto* taskLayout = new QVBoxLayout(taskPanel);
    taskLayout->setContentsMargins(12, 6, 12, 8);
    m_overviewTasks = table({QStringLiteral("任务"), QStringLiteral("数量"), QStringLiteral("状态")},
                            QStringLiteral("overviewTasksTable"), RowStyle::Lines);
    auto* taskColumns = m_overviewTasks->horizontalHeader();
    taskColumns->setStretchLastSection(false);
    taskColumns->setSectionResizeMode(0, QHeaderView::Stretch);
    taskColumns->setSectionResizeMode(1, QHeaderView::Fixed);
    taskColumns->setSectionResizeMode(2, QHeaderView::Fixed);
    m_overviewTasks->setColumnWidth(1, 60);
    m_overviewTasks->setColumnWidth(2, 124);
    m_overviewTasks->setMinimumHeight(212);
    taskLayout->addWidget(m_overviewTasks, 1);
    taskColumn->addWidget(taskPanel, 1);
    middle->addLayout(taskColumn, 2);
    layout->addLayout(middle, 1);
    layout->addSpacing(28);

    auto* logsLink = link(QStringLiteral("全部日志"), QStringLiteral("overviewLogsLink"));
    sectionHeader(layout, QStringLiteral("最近活动"))->addWidget(logsLink);
    auto* logPanel = card(QStringLiteral("overviewLogPanel"));
    auto* logLayout = new QVBoxLayout(logPanel);
    logLayout->setContentsMargins(12, 4, 12, 4);
    m_overviewLogs = table({QStringLiteral("时间"), QStringLiteral("级别"), QStringLiteral("事件")},
                           QStringLiteral("overviewLogsTable"), RowStyle::Lines);
    m_overviewLogs->horizontalHeader()->hide();
    auto* logColumns = m_overviewLogs->horizontalHeader();
    logColumns->setStretchLastSection(false);
    logColumns->setSectionResizeMode(0, QHeaderView::Fixed);
    logColumns->setSectionResizeMode(1, QHeaderView::Fixed);
    logColumns->setSectionResizeMode(2, QHeaderView::Stretch);
    m_overviewLogs->setColumnWidth(0, 92);
    m_overviewLogs->setColumnWidth(1, 116);
    m_overviewLogs->setMinimumHeight(44);
    logLayout->addWidget(m_overviewLogs);
    layout->addWidget(logPanel);
    connect(pricesLink, &QPushButton::clicked, this, [this] { setPage(3); });
    connect(tasksLink, &QPushButton::clicked, this, [this] { setPage(2); });
    connect(logsLink, &QPushButton::clicked, this, [this] { setPage(5); });
    return page;
}

QWidget* MainWindow::buildFavorites()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("favoritesPage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(32, 24, 32, 24);
    layout->setSpacing(0);
    auto* catalogHeader = pageHeader(layout, titles[1], &m_favoritesCount);
    if (m_catalog) {
        auto* manageCatalog = button(QStringLiteral("皮肤资料 / 追加赛季"), QStringLiteral("manageSkinCatalogButton"));
        catalogHeader->addWidget(manageCatalog);
        connect(manageCatalog, &QPushButton::clicked, this, &MainWindow::manageSkinCatalog);
    }
    layout->addSpacing(16);

    auto* filterBar = new QFrame;
    filterBar->setObjectName(QStringLiteral("catalogFilterBar"));
    auto* filters = new QHBoxLayout(filterBar);
    filters->setContentsMargins(0, 0, 0, 0);
    filters->setSpacing(6);
    const auto addFilter = [&](const QString& text, const QString& key) {
        auto* pill = new PillButton(text);
        pill->setObjectName(QStringLiteral("catalogFilter_") + key);
        pill->setProperty("filterKey", key);
        pill->setChecked(key == QStringLiteral("all"));
        filters->addWidget(pill);
        m_catalogFilters.append(pill);
        connect(pill, &QPushButton::clicked, this, [this, key] { m_catalogFilter = key; refreshFavorites(); });
    };
    addFilter(QStringLiteral("全部"), QStringLiteral("all"));
    addFilter(QStringLiteral("已关注"), QStringLiteral("followed"));
    addFilter(QStringLiteral("未关注"), QStringLiteral("unfollowed"));
    filters->addWidget(rule(QStringLiteral("filterDivider")));
    addFilter(QStringLiteral("步枪"), QStringLiteral("rifle"));
    addFilter(QStringLiteral("冲锋枪"), QStringLiteral("smg"));
    addFilter(QStringLiteral("狙击枪"), QStringLiteral("sniper"));
    addFilter(m_catalog ? QStringLiteral("机枪") : QStringLiteral("轻机枪"), QStringLiteral("lmg"));
    filters->addStretch();
    m_rarityFilter = new FluentComboBox;
    m_rarityFilter->setObjectName(QStringLiteral("favoriteRarity"));
    m_rarityFilter->addItem((m_catalog ? QStringLiteral("全部颜色") : QStringLiteral("全部品级")));
    m_rarityFilter->setFixedWidth(132);
    m_favoriteStateFilter = new FluentComboBox;
    m_favoriteStateFilter->setObjectName(QStringLiteral("favoriteTaskFilter"));
    m_favoriteStateFilter->addItems({QStringLiteral("全部任务"), QStringLiteral("已关联"), QStringLiteral("未关联")});
    m_favoriteStateFilter->setFixedWidth(132);
    filters->addWidget(m_rarityFilter);
    filters->addWidget(m_favoriteStateFilter);
    layout->addWidget(filterBar);
    layout->addSpacing(16);

    auto* body = new QHBoxLayout;
    body->setSpacing(16);
    auto* workspace = new QWidget;
    workspace->setObjectName(QStringLiteral("watchlistWorkspace"));
    auto* work = new QVBoxLayout(workspace);
    work->setContentsMargins(0, 0, 0, 0);
    work->setSpacing(12);
    m_favoritesTable = table({QStringLiteral("关注"), QStringLiteral("物品"), QStringLiteral("磨损"), (m_catalog ? QStringLiteral("已知报价") : QStringLiteral("演示报价")),
                              QStringLiteral("样本趋势"), QStringLiteral("目标价格"), QStringLiteral("任务状态")},
                             QStringLiteral("favoriteTable"), RowStyle::Cards);
    auto* columns = m_favoritesTable->horizontalHeader();
    columns->setStretchLastSection(false);
    columns->setSectionResizeMode(QHeaderView::Fixed);
    columns->setSectionResizeMode(1, QHeaderView::Stretch);
    m_favoritesTable->setColumnWidth(0, 44);
    m_favoritesTable->setMinimumHeight(220);
    work->addWidget(m_favoritesTable, 1);

    auto* chartPanel = card(QStringLiteral("watchlistChartPanel"));
    chartPanel->setFixedHeight(236);
    auto* chartLayout = new QVBoxLayout(chartPanel);
    chartLayout->setContentsMargins(20, 14, 20, 10);
    chartLayout->setSpacing(4);
    auto* chartHeader = new QHBoxLayout;
    chartHeader->setSpacing(8);
    chartHeader->addWidget(label(QStringLiteral("价格走势"), QStringLiteral("cardTitle")));
    m_favoriteChartTitle = label(QString(), QStringLiteral("cardCaption"));
    chartHeader->addWidget(m_favoriteChartTitle);
    chartHeader->addStretch();
    chartHeader->addWidget(label((m_catalog ? QStringLiteral("暂无价格采样") : QStringLiteral("模拟数据")), QStringLiteral("tertiaryLabel")));
    chartLayout->addLayout(chartHeader);
    m_favoriteChart = new PriceChart;
    m_favoriteChart->setObjectName(QStringLiteral("favoritePriceChart"));
    m_favoriteChart->setHeaderVisible(false);
    chartLayout->addWidget(m_favoriteChart, 1);
    work->addWidget(chartPanel);
    body->addWidget(workspace, 1);

    m_inspector = card(QStringLiteral("terminalInspector"));
    m_inspector->setFixedWidth(320);
    auto* inspectorOuter = new QVBoxLayout(m_inspector);
    inspectorOuter->setContentsMargins(1, 1, 1, 1);
    auto* inspectorScroll = new QScrollArea;
    inspectorScroll->setObjectName(QStringLiteral("inspectorScrollArea"));
    inspectorScroll->setWidgetResizable(true);
    inspectorScroll->setFrameShape(QFrame::NoFrame);
    inspectorScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    inspectorScroll->viewport()->setAutoFillBackground(false);
    auto* inspectorContent = new QWidget;
    inspectorContent->setObjectName(QStringLiteral("inspectorContent"));
    auto* detail = new QVBoxLayout(inspectorContent);
    detail->setContentsMargins(20, 20, 20, 20);
    detail->setSpacing(0);
    inspectorScroll->setWidget(inspectorContent);
    inspectorOuter->addWidget(inspectorScroll);

    m_favoriteArt = new ArtworkView;
    m_favoriteArt->setObjectName(QStringLiteral("skinPreview"));
    m_favoriteArt->setFixedHeight(148);
    m_favoriteArt->setToolTip((m_catalog ? QStringLiteral("缩略图预留，暂不显示图片。") : QStringLiteral("生成的演示插图，不代表游戏中的真实皮肤外观。")));
    detail->addWidget(m_favoriteArt);
    detail->addSpacing(16);
    m_favoriteTitle = label(QString(), QStringLiteral("detailTitle"));
    m_favoriteTitle->setWordWrap(true);
    detail->addWidget(m_favoriteTitle);
    detail->addSpacing(2);
    m_favoriteMetadata = label(QString(), QStringLiteral("cardCaption"));
    detail->addWidget(m_favoriteMetadata);
    detail->addSpacing(12);
    auto* quote = new QHBoxLayout;
    quote->setSpacing(10);
    m_favoritePrice = label(QString(), QStringLiteral("inspectorPrice"));
    m_favoriteChange = label(QString(), QStringLiteral("favoriteChangeLabel"));
    quote->addWidget(m_favoritePrice, 0, Qt::AlignBottom);
    quote->addWidget(m_favoriteChange, 0, Qt::AlignBottom);
    quote->addStretch();
    detail->addLayout(quote);
    detail->addWidget(label((m_catalog ? QStringLiteral("未采集的市场数据保持空白") : QStringLiteral("演示报价 · 非实时行情")), QStringLiteral("tertiaryLabel")));
    detail->addSpacing(16);
    auto* facts = new QHBoxLayout;
    facts->setSpacing(12);
    const auto addFact = [&](const QString& caption, QLabel*& value) {
        if (facts->count() > 0) facts->addWidget(rule(QStringLiteral("factDivider")));
        auto* block = new QVBoxLayout;
        block->setSpacing(2);
        block->addWidget(label(caption, QStringLiteral("factCaption")));
        value = label(QStringLiteral("—"), QStringLiteral("factValue"));
        block->addWidget(value);
        facts->addLayout(block, 1);
    };
    addFact((m_catalog ? QStringLiteral("菜单颜色") : QStringLiteral("品级")), m_factRarity);
    addFact(QStringLiteral("成色"), m_factCondition);
    addFact(QStringLiteral("磨损"), m_factWear);
    detail->addLayout(facts);
    detail->addSpacing(16);
    detail->addWidget(rule(QStringLiteral("inspectorRule")));
    detail->addSpacing(16);
    detail->addWidget(label(QStringLiteral("任务条件"), QStringLiteral("cardTitle")));
    detail->addSpacing(10);
    m_inspectorPrice = new FluentDoubleSpinBox;
    m_inspectorPrice->setObjectName(QStringLiteral("inspectorMaxPrice"));
    m_inspectorPrice->setRange(0, 999999999);
    m_inspectorPrice->setDecimals(2);
    if (m_catalog) m_inspectorPrice->setSpecialValueText(QStringLiteral("请填写"));
    m_inspectorCondition = new FluentComboBox;
    m_inspectorCondition->setObjectName(QStringLiteral("inspectorCondition"));
    m_inspectorCondition->addItems(conditionOptions(m_state));
    m_inspectorWear = new FluentDoubleSpinBox;
    m_inspectorWear->setObjectName(QStringLiteral("inspectorMaxWear"));
    m_inspectorWear->setRange(0, 100);
    m_inspectorWear->setDecimals(6);
    m_inspectorQuantity = new FluentSpinBox;
    m_inspectorQuantity->setObjectName(QStringLiteral("inspectorQuantity"));
    m_inspectorQuantity->setRange(1, 9999);
    for (QWidget* field : {static_cast<QWidget*>(m_inspectorPrice), static_cast<QWidget*>(m_inspectorCondition),
                           static_cast<QWidget*>(m_inspectorWear), static_cast<QWidget*>(m_inspectorQuantity)})
        field->setMinimumWidth(64);
    auto* fields = new QGridLayout;
    fields->setHorizontalSpacing(12);
    fields->setVerticalSpacing(4);
    fields->addWidget(label(QStringLiteral("最高价格"), QStringLiteral("fieldLabel")), 0, 0);
    fields->addWidget(label(QStringLiteral("成色"), QStringLiteral("fieldLabel")), 0, 1);
    fields->addWidget(m_inspectorPrice, 1, 0);
    fields->addWidget(m_inspectorCondition, 1, 1);
    fields->setRowMinimumHeight(2, 8);
    fields->addWidget(label(QStringLiteral("最大磨损"), QStringLiteral("fieldLabel")), 3, 0);
    fields->addWidget(label(QStringLiteral("数量上限"), QStringLiteral("fieldLabel")), 3, 1);
    fields->addWidget(m_inspectorWear, 4, 0);
    fields->addWidget(m_inspectorQuantity, 4, 1);
    fields->setColumnStretch(0, 3);
    fields->setColumnStretch(1, 2);
    detail->addLayout(fields);
    detail->addSpacing(16);
    m_favoriteCreate = button(QStringLiteral("创建任务"), QStringLiteral("favoriteCreateTaskButton"), ButtonKind::Accent, Glyph::Add);
    m_favoriteCreate->setFixedHeight(36);
    detail->addWidget(m_favoriteCreate);
    detail->addSpacing(8);
    auto* taskRow = new QHBoxLayout;
    m_favoriteTaskInfo = label(QString(), QStringLiteral("inspectorTaskInfo"));
    taskRow->addWidget(m_favoriteTaskInfo);
    taskRow->addStretch();
    auto* manage = link(QStringLiteral("查看任务"), QStringLiteral("favoritesTasksButton"));
    taskRow->addWidget(manage);
    detail->addLayout(taskRow);
    detail->addStretch();
    detail->addSpacing(12);
    auto* note = label((m_catalog ? QStringLiteral("皮肤资料来自已核对目录；极品 / 优品与任务成色分别记录。缩略图暂时留空。") : QStringLiteral("本地模拟模式：插图与价格均为演示素材，创建任务只保存筛选条件。")), QStringLiteral("inspectorNote"));
    note->setWordWrap(true);
    detail->addWidget(note);
    body->addWidget(m_inspector);
    layout->addLayout(body, 1);

    connect(m_favoriteSearch, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_searchClear->setVisible(!text.isEmpty());
        if (m_refreshing) return;
        if (!text.isEmpty() && m_pages->currentIndex() != 1) setPage(1);
        refreshFavorites();
    });
    connect(m_rarityFilter, &QComboBox::currentTextChanged, this, [this] { if (!m_refreshing) refreshFavorites(); });
    connect(m_favoriteStateFilter, &QComboBox::currentIndexChanged, this, [this] { if (!m_refreshing) refreshFavorites(); });
    connect(m_favoritesTable, &QTableWidget::itemSelectionChanged, this, [this] { if (!m_refreshing) refreshFavoriteDetail(); });
    connect(m_favoriteCreate, &QPushButton::clicked, this, &MainWindow::createInspectorTask);
    if (m_catalog) {
        m_favoriteCreate->setToolTip(QStringLiteral("填写最高价格后创建任务"));
        connect(m_inspectorPrice, &QDoubleSpinBox::valueChanged, this, [this](double value) {
            m_favoriteCreate->setEnabled(findSkin(m_state, selectedFavoriteId()) && value > 0);
        });
    }
    connect(manage, &QPushButton::clicked, this, [this] { setPage(2); });
    return page;
}

QWidget* MainWindow::buildTasks()
{
    m_taskPage = new TaskPage(m_state);
    return m_taskPage;
}

QWidget* MainWindow::buildPrices()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("pricesPage"));
    auto* layout = pageLayout(page);
    auto* header = pageHeader(layout, titles[3]);
    header->addWidget(label(QStringLiteral("关注目标"), QStringLiteral("cardCaption")), 0, Qt::AlignVCenter);
    m_priceSkin = new FluentComboBox;
    m_priceSkin->setObjectName(QStringLiteral("pricesSkinCombo"));
    m_priceSkin->setMinimumWidth(260);
    header->addWidget(m_priceSkin, 0, Qt::AlignVCenter);
    auto* exportButton = button(QStringLiteral("导出 CSV"), QStringLiteral("exportPricesButton"), ButtonKind::Standard, Glyph::Download);
    header->addWidget(exportButton, 0, Qt::AlignVCenter);
    layout->addSpacing(20);

    auto* strip = card(QStringLiteral("metricStrip"));
    auto* metrics = new QHBoxLayout(strip);
    metrics->setContentsMargins(0, 8, 0, 8);
    metrics->setSpacing(0);
    const QStringList names = {(m_catalog ? QStringLiteral("当前已知报价") : QStringLiteral("当前演示报价")), QStringLiteral("样本最低报价"), QStringLiteral("样本最高报价")};
    for (int i = 0; i < names.size(); ++i) {
        auto* metric = new MetricCard(names[i], QStringLiteral("—"), (m_catalog ? QStringLiteral("未采集的数值不作估算") : QStringLiteral("演示价格 · 非实时市场数据")));
        metric->setObjectName(QStringLiteral("priceMetric%1").arg(i));
        metric->setProperty("separator", i < names.size() - 1);
        m_priceCards.append(metric);
        metrics->addWidget(metric, 1);
    }
    layout->addWidget(strip);
    layout->addSpacing(28);

    sectionHeader(layout, QStringLiteral("价格走势"))->addWidget(label((m_catalog ? QStringLiteral("采集后显示历史曲线") : QStringLiteral("24 个样本 · 演示数据")), QStringLiteral("tertiaryLabel")));
    auto* chartPanel = card(QStringLiteral("priceChartPanel"));
    auto* chartLayout = new QVBoxLayout(chartPanel);
    chartLayout->setContentsMargins(20, 16, 20, 12);
    m_priceChart = new PriceChart;
    m_priceChart->setObjectName(QStringLiteral("priceHistoryChart"));
    m_priceChart->setMinimumHeight(220);
    chartLayout->addWidget(m_priceChart, 1);
    layout->addWidget(chartPanel, 3);
    layout->addSpacing(28);

    sectionHeader(layout, QStringLiteral("样本记录"))->addWidget(label((m_catalog ? QStringLiteral("暂无历史采样") : QStringLiteral("本地合成数据")), QStringLiteral("tertiaryLabel")));
    auto* historyPanel = card(QStringLiteral("priceHistoryPanel"));
    auto* historyLayout = new QVBoxLayout(historyPanel);
    historyLayout->setContentsMargins(12, 6, 12, 8);
    m_priceHistory = table({QStringLiteral("样本"), QStringLiteral("商品"), (m_catalog ? QStringLiteral("已知价格") : QStringLiteral("演示价格")), QStringLiteral("类型"), QStringLiteral("数据来源")},
                           QStringLiteral("priceHistoryTable"), RowStyle::Lines);
    auto* historyColumns = m_priceHistory->horizontalHeader();
    historyColumns->setStretchLastSection(false);
    historyColumns->setSectionResizeMode(QHeaderView::Fixed);
    historyColumns->setSectionResizeMode(1, QHeaderView::Stretch);
    m_priceHistory->setColumnWidth(0, 96);
    m_priceHistory->setColumnWidth(2, 120);
    m_priceHistory->setColumnWidth(3, 112);
    m_priceHistory->setColumnWidth(4, 220);
    m_priceHistory->setMinimumHeight(300);
    historyLayout->addWidget(m_priceHistory, 1);
    layout->addWidget(historyPanel, 3);
    connect(m_priceSkin, &QComboBox::currentIndexChanged, this, [this] { if (!m_refreshing) refreshPrices(); });
    connect(exportButton, &QPushButton::clicked, this, &MainWindow::exportPrices);
    return page;
}

QWidget* MainWindow::buildStats()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("statsPage"));
    auto* layout = pageLayout(page);
    pageHeader(layout, titles[4]);
    layout->addSpacing(20);
    if (m_workspace) {
        auto* ledger = card(QStringLiteral("workspaceStatsPanel"));
        auto* ledgerLayout = new QVBoxLayout(ledger);
        ledgerLayout->setContentsMargins(16, 14, 16, 14);
        ledgerLayout->addWidget(label(QStringLiteral("已提交回放账本"), QStringLiteral("sectionTitle")));
        auto* values = new QHBoxLayout;
        values->setSpacing(0);
        const QStringList names{(m_catalog ? QStringLiteral("确认成功") : QStringLiteral("模拟确认成功")), QStringLiteral("模拟确认失败"),
                                QStringLiteral("结果未知"), QStringLiteral("仍占用预留")};
        for (int i = 0; i < names.size(); ++i) {
            auto* metric = new MetricCard(names[i], QStringLiteral("0"), QStringLiteral("仅已提交数据"));
            metric->setObjectName(QStringLiteral("workspaceStatsMetric%1").arg(i));
            metric->setProperty("separator", i < names.size() - 1);
            m_workspaceStatsCards.append(metric);
            values->addWidget(metric, 1);
        }
        ledgerLayout->addLayout(values);
        m_workspaceStatsSummary = label(QString(), QStringLiteral("workspaceStatsSummary"));
        m_workspaceStatsSummary->setWordWrap(true);
        ledgerLayout->addWidget(m_workspaceStatsSummary);
        layout->addWidget(ledger);
        layout->addSpacing(24);
        layout->addWidget(label(QStringLiteral("本地演示统计 · 独立口径"), QStringLiteral("groupLabel")));
        layout->addSpacing(10);
    }
    auto* strip = card(QStringLiteral("metricStrip"));
    auto* metrics = new QHBoxLayout(strip);
    metrics->setContentsMargins(0, 8, 0, 8);
    metrics->setSpacing(0);
    const QStringList names = {(m_catalog ? QStringLiteral("扫描条目") : QStringLiteral("模拟扫描")), (m_catalog ? QStringLiteral("条件命中") : QStringLiteral("模拟条件命中")), (m_catalog ? QStringLiteral("确认成功") : QStringLiteral("模拟确认成功")), QStringLiteral("实际成交 / 实际支出")};
    const QStringList details = {(m_catalog ? QStringLiteral("尚无运行记录") : QStringLiteral("本次演示扫描条目")), (m_catalog ? QStringLiteral("尚无匹配记录") : QStringLiteral("模拟筛选符合条件")), (m_catalog ? QStringLiteral("仅显示实际回读结果") : QStringLiteral("只计入模拟确认结果")), QStringLiteral("未连接执行模块")};
    for (int i = 0; i < names.size(); ++i) {
        auto* metric = new MetricCard(names[i], i == 3 ? QStringLiteral("—") : QStringLiteral("0"), details[i]);
        metric->setObjectName(QStringLiteral("statsMetric%1").arg(i));
        metric->setProperty("separator", i < names.size() - 1);
        m_statsCards.append(metric);
        metrics->addWidget(metric, 1);
    }
    layout->addWidget(strip);
    layout->addSpacing(28);

    auto* content = new QHBoxLayout;
    content->setSpacing(16);
    auto* left = new QVBoxLayout;
    left->setSpacing(0);
    sectionHeader(left, (m_catalog ? QStringLiteral("结果分布") : QStringLiteral("模拟结果分布")))->addWidget(label((m_catalog ? QStringLiteral("尚无运行记录") : QStringLiteral("基于本次演示计数")), QStringLiteral("tertiaryLabel")));
    auto* distribution = card(QStringLiteral("distributionPanel"));
    auto* distributionLayout = new QVBoxLayout(distribution);
    distributionLayout->setContentsMargins(24, 22, 24, 20);
    distributionLayout->setSpacing(0);
    const QStringList reasons = {QStringLiteral("未命中筛选条件"), (m_catalog ? QStringLiteral("已命中 · 待确认") : QStringLiteral("已命中 · 尚未模拟确认")), (m_catalog ? QStringLiteral("确认成功") : QStringLiteral("模拟确认成功"))};
    for (const auto& reason : reasons) {
        auto* row = new QHBoxLayout;
        row->addWidget(label(reason));
        row->addStretch();
        auto* value = label(QStringLiteral("0"), QStringLiteral("cardCaption"));
        m_reasonLabels.append(value);
        row->addWidget(value);
        distributionLayout->addLayout(row);
        distributionLayout->addSpacing(10);
        auto* bar = new QProgressBar;
        bar->setRange(0, 100);
        bar->setValue(0);
        bar->setTextVisible(false);
        m_reasonBars.append(bar);
        distributionLayout->addWidget(bar);
        distributionLayout->addSpacing(22);
    }
    distributionLayout->addStretch();
    m_statsNote = label(QString(), QStringLiteral("tertiaryLabel"));
    m_statsNote->setWordWrap(true);
    distributionLayout->addWidget(m_statsNote);
    left->addWidget(distribution, 1);
    content->addLayout(left, 3);

    auto* right = new QVBoxLayout;
    right->setSpacing(0);
    sectionHeader(right, QStringLiteral("统计口径"));
    auto* semantics = card(QStringLiteral("semanticsPanel"));
    auto* semanticsLayout = new QVBoxLayout(semantics);
    semanticsLayout->setContentsMargins(24, 22, 24, 20);
    semanticsLayout->setSpacing(0);
    const QVector<QPair<QString, QString>> explanations = {
        {QStringLiteral("扫描 ≠ 命中"), QStringLiteral("扫描记录观察次数；命中记录符合任务条件的次数。")},
        {QStringLiteral("尝试 ≠ 成交"), (m_catalog ? QStringLiteral("任务配置不代表成交；成交与支出仅按实际结果记录。") : QStringLiteral("当前只有本地模拟，不产生实际交易和实际支出。"))},
        {QStringLiteral("未决结果单独记"), (m_catalog ? QStringLiteral("结果未明的动作不计入成功；没有观测时保持空白。") : QStringLiteral("接入执行模块后，结果未明的动作不计入成功。当前演示未决为 0。"))}};
    for (const auto& entry : explanations) {
        semanticsLayout->addWidget(label(entry.first, QStringLiteral("cardTitle")));
        semanticsLayout->addSpacing(4);
        auto* description = label(entry.second, QStringLiteral("cardCaption"));
        description->setWordWrap(true);
        semanticsLayout->addWidget(description);
        semanticsLayout->addSpacing(18);
    }
    semanticsLayout->addStretch();
    right->addWidget(semantics, 1);
    content->addLayout(right, 2);
    layout->addLayout(content);
    layout->addStretch(1);
    return page;
}

QWidget* MainWindow::buildLogs()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("logsPage"));
    auto* layout = pageLayout(page);
    auto* header = pageHeader(layout, titles[5], &m_logCount);
    m_logSearch = new QLineEdit;
    m_logSearch->setObjectName(QStringLiteral("logSearch"));
    m_logSearch->setPlaceholderText(QStringLiteral("搜索事件内容"));
    m_logSearch->setFixedWidth(280);
    m_logSearch->addAction(FluentTheme::glyphIcon(Glyph::Search, FluentTheme::secondary, FluentTheme::text), QLineEdit::TrailingPosition);
    auto* clearSearch = m_logSearch->addAction(FluentTheme::glyphIcon(Glyph::Clear, FluentTheme::secondary, FluentTheme::text), QLineEdit::TrailingPosition);
    clearSearch->setVisible(false);
    connect(clearSearch, &QAction::triggered, m_logSearch, &QLineEdit::clear);
    connect(m_logSearch, &QLineEdit::textChanged, clearSearch, [clearSearch](const QString& text) { clearSearch->setVisible(!text.isEmpty()); });
    header->addWidget(m_logSearch, 0, Qt::AlignVCenter);
    m_logLevel = new FluentComboBox;
    m_logLevel->setObjectName(QStringLiteral("logLevel"));
    m_logLevel->addItems({QStringLiteral("全部级别"), "INFO", "DEMO", "SUCCESS", "WARN", "ERROR"});
    m_logLevel->setFixedWidth(140);
    header->addWidget(m_logLevel, 0, Qt::AlignVCenter);
    auto* clear = button(QStringLiteral("清空日志"), QStringLiteral("clearLogsButton"), ButtonKind::Standard, Glyph::Delete);
    header->addWidget(clear, 0, Qt::AlignVCenter);
    layout->addSpacing(20);
    if (m_workspace) {
        layout->addWidget(buildWorkspaceRecords());
        layout->addSpacing(24);
        layout->addWidget(label(QStringLiteral("本地演示日志 · 与回放账本分开"), QStringLiteral("groupLabel")));
        layout->addSpacing(8);
    }
    auto* box = card(QStringLiteral("logPanel"));
    auto* boxLayout = new QVBoxLayout(box);
    boxLayout->setContentsMargins(12, 6, 12, 8);
    m_logsTable = table({QStringLiteral("时间"), QStringLiteral("级别"), QStringLiteral("内容")}, QStringLiteral("logsTable"), RowStyle::Lines);
    auto* columns = m_logsTable->horizontalHeader();
    columns->setStretchLastSection(false);
    columns->setSectionResizeMode(QHeaderView::Fixed);
    columns->setSectionResizeMode(2, QHeaderView::Stretch);
    m_logsTable->setColumnWidth(0, 96);
    m_logsTable->setColumnWidth(1, 124);
    m_logsTable->setMinimumHeight(360);
    boxLayout->addWidget(m_logsTable, 1);
    layout->addWidget(box, 1);
    connect(m_logSearch, &QLineEdit::textChanged, this, [this] { if (!m_refreshing) refreshLogs(); });
    connect(m_logLevel, &QComboBox::currentTextChanged, this, [this] { if (!m_refreshing) refreshLogs(); });
    connect(clear, &QPushButton::clicked, this, [this] {
        if (confirm(this, QStringLiteral("清空本地日志"), (m_catalog ? QStringLiteral("只清空当前本地日志，任务与配置不变。") : QStringLiteral("只清空当前本地演示日志，任务与配置不变。")), QStringLiteral("清空"))) {
            m_state->logs.clear();
            m_state->notifyChanged();
        }
    });
    return page;
}

QWidget* MainWindow::buildSettings()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("settingsPage"));
    auto* layout = pageLayout(page);
    pageHeader(layout, titles[6]);
    const auto group = [&](const QString& text, bool first = false) {
        layout->addSpacing(first ? 20 : 28);
        layout->addWidget(label(text, QStringLiteral("groupLabel")));
        layout->addSpacing(8);
    };
    const auto row = [&](QWidget* item) { layout->addWidget(item); layout->addSpacing(4); };
    group(QStringLiteral("显示"), true);
    auto* compact = new ToggleSwitch;
    compact->setObjectName(QStringLiteral("compactTablesCheck"));
    compact->setStateTextVisible(true);
    m_compactTables = compact;
    row(settingsCard(glyphLabel(Glyph::List), QStringLiteral("紧凑列表"), QStringLiteral("降低列表行高，一屏显示更多条目"), compact));
    auto* autoScroll = new ToggleSwitch;
    autoScroll->setObjectName(QStringLiteral("autoScrollLogsCheck"));
    autoScroll->setStateTextVisible(true);
    m_autoScrollLogs = autoScroll;
    row(settingsCard(glyphLabel(Glyph::History), QStringLiteral("日志自动定位"), QStringLiteral("新事件出现时，日志列表定位到最新一条"), autoScroll));

    group(QStringLiteral("本地配置"));
    m_configDirectory = new QLineEdit;
    m_configDirectory->setObjectName(QStringLiteral("configDirectoryEdit"));
    m_configDirectory->setReadOnly(true);
    m_configDirectory->setFixedWidth(460);
    row(settingsCard(glyphLabel(Glyph::Folder), QStringLiteral("配置文件"), QStringLiteral("关注、任务与运行参数保存在此文件；退出程序时自动保存"), m_configDirectory));
    layout->addSpacing(8);
    auto* actionRow = new QHBoxLayout;
    m_settingsMessage = label(QString(), QStringLiteral("settingsMessage"));
    m_settingsMessage->setWordWrap(true);
    actionRow->addWidget(m_settingsMessage, 1);
    auto* save = button(QStringLiteral("保存本地设置"), QStringLiteral("saveSettingsButton"), ButtonKind::Accent, Glyph::Save);
    actionRow->addWidget(save);
    layout->addLayout(actionRow);

    group(QStringLiteral("运行模式"));
    row(settingsCard(glyphLabel(Glyph::Pulse), (m_catalog ? QStringLiteral("真实皮肤目录") : QStringLiteral("前端独立演示")),
                     (m_catalog ? QStringLiteral("内置 S1–S11 已核对皮肤资料；追加赛季与皮肤独立保存，升级程序后继续保留。市场价格、磨损与历史等待实际采集。") : QStringLiteral("使用本地演示数据验证关注管理、任务配置、价格展示与统计交互；没有连接游戏进程，不读取实时市场，也不执行实际购买。")),
                     label((m_catalog ? QStringLiteral("真实目录") : QStringLiteral("本地预览")), QStringLiteral("settingsBadge")), QStringLiteral("runModeCard")));
    group(QStringLiteral("数据连接"));
    row(settingsCard(glyphLabel(Glyph::Link), QStringLiteral("游戏页面采集 · 价格识别与校验 · 自动操作与结果确认"),
                     QStringLiteral("界面层与这些模块保持分离；前端完成后可以逐个接入。"),
                     label(QStringLiteral("尚未接入"), QStringLiteral("settingsBadge"))));
    group(QStringLiteral("关于"));
    row(settingsCard(new AppMark(24), QStringLiteral("Relink Studio"),
                     (m_catalog ? QStringLiteral("版本 %1 · 皮肤目录与任务管理 · C++17 / Qt %2 Widgets") : QStringLiteral("版本 %1 · 本地演示前端 · C++17 / Qt %2 Widgets")).arg(QCoreApplication::applicationVersion(), QStringLiteral(QT_VERSION_STR)),
                     nullptr, QStringLiteral("aboutCard")));
    layout->addStretch();
    connect(save, &QPushButton::clicked, this, &MainWindow::saveSettings);
    connect(m_compactTables, &QCheckBox::toggled, this, &MainWindow::applyDensity);
    return page;
}

// Every entry of the original assistant's main screen, as Windows 11 settings cards.
QWidget* MainWindow::buildRunSettings()
{
    m_runSettingsPage = new RunSettingsPage(m_state, m_workspace ? buildWorkspaceSettings() : nullptr);
    connect(m_runSettingsPage, &RunSettingsPage::saveRequested, this, [this] {
        saveSettings();
        m_runSettingsPage->showSaveResult(m_settingsMessage->text());
    });
    connect(m_runSettingsPage, &RunSettingsPage::importPreviewRequested, this, [this] {
        if (m_catalog) importConfiguration(); else previewImportDemo();
    });
    connect(m_runSettingsPage, &RunSettingsPage::navigateRequested, this, &MainWindow::setPage);
    connect(m_runSettingsPage, &RunSettingsPage::helpRequested, this, &MainWindow::showHelp);
    return m_runSettingsPage;
}

void MainWindow::setPage(int index)
{
    if (index < 0 || index >= m_pages->count()) return;
    m_pages->setCurrentIndex(index);
    for (int i = 0; i < m_nav.size(); ++i) {
        if (!m_nav[i]) continue;
        QSignalBlocker guard(m_nav[i]);
        m_nav[i]->setChecked(i == index);
    }
    QTimer::singleShot(0, this, &MainWindow::fitWatchlistColumns);
}

QString MainWindow::currentPageName() const { return titles.value(m_pages->currentIndex()); }

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (m_favoriteSearch) {
        const int searchWidth = qBound(300, qRound(width() * 0.25), 520);
        m_favoriteSearch->setFixedWidth(searchWidth);
        m_favoriteSearch->move((width() - searchWidth) / 2, 8);
        m_favoriteSearch->raise();
    }
    if (m_inspector) m_inspector->setFixedWidth(width() < 1360 ? 300 : 320);
    QTimer::singleShot(0, this, &MainWindow::fitWatchlistColumns);
}

void MainWindow::fitWatchlistColumns()
{
    if (!m_favoritesTable) return;
    const int w = m_favoritesTable->viewport()->width();
    m_favoritesTable->setColumnWidth(0, 44);
    m_favoritesTable->setColumnWidth(2, qBound(64, qRound(w * .08), 92));
    m_favoritesTable->setColumnWidth(3, qBound(84, qRound(w * .11), 120));
    m_favoritesTable->setColumnWidth(4, qBound(100, qRound(w * .16), 180));
    m_favoritesTable->setColumnWidth(5, qBound(84, qRound(w * .11), 120));
    m_favoritesTable->setColumnWidth(6, qBound(92, qRound(w * .11), 124));
}

void MainWindow::refreshReplayProjection()
{
    if (!m_replayController || !m_replaySourceLabel) return;
    const auto view = relink::runtime::UiProjection::replay(m_replayController->snapshot());
    m_replaySourceLabel->setText(view.source);
    m_replayStateLabel->setText(view.runStateText);
    m_replayReasonLabel->setText(view.matchReason);
    m_replayProgressLabel->setText(view.progress);
    m_replayStartButton->setEnabled(view.canStart);
    m_replayPauseButton->setEnabled(view.canPause);
    m_replayResumeButton->setEnabled(view.canResume);
    m_replayStopButton->setEnabled(view.canStop);
    m_replayStateLabel->setProperty("status", view.runState);
    m_replayStateLabel->style()->unpolish(m_replayStateLabel);
    m_replayStateLabel->style()->polish(m_replayStateLabel);
}

void MainWindow::refreshAll()
{
    if (m_refreshing) return;
    m_refreshing = true;
    refreshOverview();
    refreshFavorites();
    m_taskPage->refresh();
    refreshPrices();
    refreshStats();
    refreshLogs();
    m_runSettingsPage->refresh();
    refreshReplayProjection();
    const bool running = m_state->simulationRunning;
    m_startButton->setEnabled(!running);
    m_pauseButton->setEnabled(running);
    m_taskPage->setSimulationAvailable(!m_catalog && !running);
    m_statusBadge->setText(QStringLiteral("<span style=\"color:%1;\">●</span>&nbsp;&nbsp;%2")
                               .arg((running ? FluentTheme::positive : FluentTheme::muted).name(),
                                    running ? QStringLiteral("模拟运行中") : QStringLiteral("本地演示")));
    m_statusBadge->setToolTip(running ? QStringLiteral("本地模拟正在推进；不连接游戏，不产生订单。")
                                      : QStringLiteral("演示数据 · 未连接游戏"));
    if (m_catalog) {
        m_statusBadge->setText(QStringLiteral("真实皮肤目录"));
        m_statusBadge->setToolTip(QStringLiteral("已核对目录 · 市场数据按需采集"));
    }
    QString path = m_state->configPath;
    if (path.isEmpty()) path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/config.json";
    m_configDirectory->setText(QDir::toNativeSeparators(path));
    m_configDirectory->setCursorPosition(0);
    m_refreshing = false;
    refreshWorkspace();
}

void MainWindow::refreshOverview()
{
    int followed = 0, enabled = 0;
    for (const auto& skin : m_state->skins) if (skin.followed) ++followed;
    for (const auto& task : m_state->tasks) if (task.enabled) ++enabled;
    m_overviewCards[0]->setValue(QString::number(followed), QStringLiteral("本地关注条目"));
    m_overviewCards[1]->setValue(QString::number(enabled), QStringLiteral("共 %1 条任务").arg(m_state->tasks.size()));
    m_overviewCards[2]->setValue(QString::number(m_state->simulatedScans), QStringLiteral("本次模拟累计"));
    m_overviewCards[3]->setValue(QString::number(m_state->simulatedSuccess), QStringLiteral("仅模拟结果"));
    const double reference = m_state->skins.isEmpty() ? 100 : m_state->skins.first().price;
    m_overviewChart->setSeries(m_catalog ? QVector<double>{} : series(reference));
    if (m_catalog) {
        m_overviewCards[2]->setValue(QStringLiteral("—"), QStringLiteral("尚无扫描记录"));
        m_overviewCards[3]->setValue(QStringLiteral("—"), QStringLiteral("尚无结果回读"));
    }
    m_overviewChart->setCaption(m_state->skins.isEmpty() ? (m_catalog ? QStringLiteral("已知价格") : QStringLiteral("演示价格")) : m_state->skins.first().name);
    m_overviewTaskCount->setText(QStringLiteral("%1 条任务").arg(m_state->tasks.size()));
    m_overviewTasks->setRowCount(qMin(4, int(m_state->tasks.size())));
    for (int i = 0; i < m_overviewTasks->rowCount(); ++i) {
        const auto& task = m_state->tasks[i];
        put(m_overviewTasks, i, 0, task.name);
        put(m_overviewTasks, i, 1, QString::number(task.quantity));
        putStatus(m_overviewTasks, i, 2, task.status, taskTone(task));
    }
    const int count = qMin(3, int(m_state->logs.size()));
    m_overviewLogs->setRowCount(count);
    for (int r = 0; r < count; ++r) {
        const auto& log = m_state->logs[r];
        put(m_overviewLogs, r, 0, log.time, FluentTheme::secondary);
        putStatus(m_overviewLogs, r, 1, log.level, levelTone(log.level));
        put(m_overviewLogs, r, 2, log.message)->setToolTip(log.message);
    }
    m_overviewLogs->setFixedHeight(qMax(1, count) * m_overviewLogs->verticalHeader()->defaultSectionSize() + 2);
}

QString MainWindow::selectedFavoriteId() const { return selectedId(m_favoritesTable); }

void MainWindow::refreshFavorites()
{
    const QString previous = selectedFavoriteId();
    {
        QSignalBlocker guard(m_rarityFilter);
        const QString rarity = m_rarityFilter->currentText();
        QStringList values;
        for (const auto& skin : m_state->skins) {
            const auto value = m_catalog ? menuColorName(skin.menuColor) : skin.rarity;
            if (!value.isEmpty() && !values.contains(value)) values.append(value);
        }
        m_rarityFilter->clear();
        m_rarityFilter->addItem((m_catalog ? QStringLiteral("全部颜色") : QStringLiteral("全部品级")));
        m_rarityFilter->addItems(values);
        m_rarityFilter->setCurrentIndex(qMax(0, m_rarityFilter->findText(rarity)));
    }
    auto matches = [&](const Skin& skin, const QString& key) {
        bool linked = false;
        for (const auto& task : m_state->tasks) if (task.skinId == skin.id) { linked = true; break; }
        return key == "all" || (key == "followed" && skin.followed) || (key == "unfollowed" && !skin.followed)
            || (key == "linked" && linked) || (key == "unlinked" && !linked) || key == skinCategory(skin.name);
    };
    for (auto* filter : m_catalogFilters) {
        const QString key = filter->property("filterKey").toString();
        int count = 0;
        for (const auto& skin : m_state->skins) if (matches(skin, key)) ++count;
        QSignalBlocker guard(filter);
        filter->setCount(count);
        filter->setChecked(key == m_catalogFilter);
    }
    QSignalBlocker guard(m_favoritesTable);
    m_favoritesTable->setRowCount(0);
    int selected = -1;
    const QString query = m_favoriteSearch->text().trimmed();
    for (const auto& skin : m_state->skins) {
        if (!matches(skin, m_catalogFilter)) continue;
        if (!query.isEmpty() && !skin.name.contains(query, Qt::CaseInsensitive) && !skin.series.contains(query, Qt::CaseInsensitive) && !skin.catalogProductId.contains(query, Qt::CaseInsensitive)
            && !skin.variant.contains(query, Qt::CaseInsensitive) && !skin.menuColor.contains(query) && !menuColorName(skin.menuColor).contains(query)) continue;
        if (m_rarityFilter->currentIndex() > 0 && (m_catalog ? menuColorName(skin.menuColor) : skin.rarity) != m_rarityFilter->currentText()) continue;
        const Task* linked = nullptr;
        bool enabled = false;
        for (const auto& task : m_state->tasks) {
            if (task.skinId != skin.id) continue;
            if (!linked) linked = &task;
            enabled |= task.enabled;
        }
        if (m_favoriteStateFilter->currentIndex() == 1 && !linked) continue;
        if (m_favoriteStateFilter->currentIndex() == 2 && linked) continue;
        const int row = m_favoritesTable->rowCount();
        m_favoritesTable->insertRow(row);
        if (skin.id == previous) selected = row;
        auto* star = new StarToggle;
        star->setObjectName("follow_" + skin.id);
        star->setChecked(skin.followed);
        star->setToolTip(skin.followed ? QStringLiteral("取消关注") : QStringLiteral("关注"));
        m_favoritesTable->setCellWidget(row, 0, centered(star));
        connect(star, &QCheckBox::toggled, this, [this, id = skin.id](bool checked) {
            if (m_refreshing) return;
            for (auto& value : m_state->skins) if (value.id == id) { value.followed = checked; break; }
            m_state->notifyChanged();
        });
        auto* identity = put(m_favoritesTable, row, 1, skin.name, QColor(), skin.id);
        identity->setData(KindRole, ItemCell);
        identity->setData(SubtitleRole, skin.series);
        identity->setData(ArtRole, -1);
        identity->setToolTip(skin.name + "\n" + skin.series + (m_catalog ? QStringLiteral(" · ") + menuColorName(skin.menuColor) + QStringLiteral(" · ") + skin.variant : QString()));
        put(m_favoritesTable, row, 2, skin.wearKnown ? QString::number(skin.wear, 'f', 3) : QStringLiteral("—"), FluentTheme::secondary);
        put(m_favoritesTable, row, 3, skin.priceKnown ? amount(skin.price) : QStringLiteral("—"));
        QVariantList samples;
        if (!m_catalog) for (double value : series(skin.price)) samples.append(value);
        auto* trend = put(m_favoritesTable, row, 4, QString());
        trend->setData(KindRole, SparkCell);
        trend->setData(SamplesRole, samples);
        put(m_favoritesTable, row, 5, linked ? amount(linked->maxPrice) : QStringLiteral("—"), linked ? FluentTheme::text : FluentTheme::muted);
        putStatus(m_favoritesTable, row, 6,
                  linked ? (enabled ? QStringLiteral("已启用") : QStringLiteral("已暂停")) : QStringLiteral("未配置"),
                  linked ? (enabled ? ToneSuccess : ToneCaution) : ToneInactive);
    }
    if (m_favoritesTable->rowCount() > 0) m_favoritesTable->selectRow(selected < 0 ? 0 : selected);
    m_favoritesCount->setText(QStringLiteral("%1 个条目").arg(m_favoritesTable->rowCount()));
    refreshFavoriteDetail();
}

void MainWindow::refreshFavoriteDetail()
{
    const auto* skin = findSkin(m_state, selectedFavoriteId());
    const bool valid = skin != nullptr;
    for (QWidget* control : {static_cast<QWidget*>(m_favoriteCreate), static_cast<QWidget*>(m_inspectorPrice),
                             static_cast<QWidget*>(m_inspectorWear), static_cast<QWidget*>(m_inspectorQuantity),
                             static_cast<QWidget*>(m_inspectorCondition)})
        control->setEnabled(valid);
    if (!skin) {
        m_inspectorSkinId.clear();
        m_favoriteTitle->setText(QStringLiteral("没有匹配条目"));
        m_favoriteMetadata->setText(QStringLiteral("调整搜索或筛选条件"));
        m_favoritePrice->setText(QStringLiteral("—"));
        m_favoriteChange->clear();
        m_favoriteArt->setArtwork(QPixmap());
        for (auto* fact : {m_factRarity, m_factCondition, m_factWear}) fact->setText(QStringLiteral("—"));
        m_favoriteTaskInfo->clear();
        m_favoriteChart->setSeries({});
        m_favoriteChartTitle->clear();
        return;
    }
    m_favoriteTitle->setText(skin->name);
    m_favoriteMetadata->setText(skin->series + (skin->variant.isEmpty() ? QString() : QStringLiteral(" · ") + skin->variant));
    m_favoriteArt->setArtwork(QPixmap());
    m_favoritePrice->setText(skin->priceKnown ? amount(skin->price) : QStringLiteral("—"));
    m_favoriteChange->setText(skin->changeKnown ? changeText(skin->change) : QString());
    // Falling prices read as favourable for a buyer, rising prices as unfavourable.
    const QColor changeColor = skin->change < 0 ? FluentTheme::positive : (skin->change > 0 ? FluentTheme::negative : FluentTheme::secondary);
    m_favoriteChange->setStyleSheet(QStringLiteral("color:%1;").arg(changeColor.name()));
    m_factRarity->setText(m_catalog ? menuColorName(skin->menuColor) : skin->rarity);
    m_factRarity->setToolTip(QStringLiteral("正式品质：") + skin->rarity);
    m_factCondition->setText(skin->condition);
    m_factWear->setText(skin->wearKnown ? QString::number(skin->wear, 'f', 3) : QStringLiteral("—"));
    m_favoriteChart->setSeries(m_catalog ? QVector<double>{} : series(skin->price));
    m_favoriteChartTitle->setText(skin->name);
    int tasks = 0;
    const Task* existing = nullptr;
    for (const auto& task : m_state->tasks) if (task.skinId == skin->id) { ++tasks; if (!existing) existing = &task; }
    if (m_inspectorSkinId != skin->id) {
        m_inspectorSkinId = skin->id;
        m_inspectorPrice->setValue(existing ? existing->maxPrice : skin->price);
        m_inspectorWear->setValue(existing ? existing->maxWear : 5);
        m_inspectorQuantity->setValue(existing ? existing->quantity : 1);
        const QString condition = existing ? existing->condition : (m_catalog ? QStringLiteral("不限") : skin->condition);
        if (m_inspectorCondition->findText(condition) < 0) m_inspectorCondition->addItem(condition);
        m_inspectorCondition->setCurrentText(condition);
    }
    m_favoriteTaskInfo->setText(QStringLiteral("已关联 %1 条任务").arg(tasks));
    if (m_catalog) m_favoriteCreate->setEnabled(m_inspectorPrice->value() > 0);
}

void MainWindow::createInspectorTask()
{
    const auto* skin = findSkin(m_state, selectedFavoriteId());
    if (!skin) return;
    m_inspectorPrice->interpretText();
    if (m_catalog && m_inspectorPrice->value() <= 0) return;
    m_inspectorWear->interpretText();
    m_inspectorQuantity->interpretText();
    Task task;
    task.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    task.name = skin->name.left(45) + QStringLiteral(" · 价格关注");
    task.skinId = skin->id;
    task.minPrice = 0;
    task.maxPrice = m_inspectorPrice->value();
    task.maxWear = m_inspectorWear->value();
    task.quantity = m_inspectorQuantity->value();
    task.condition = m_inspectorCondition->currentText();
    task.enabled = true;
    task.status = QStringLiteral("待启动");
    m_state->pauseSimulation();
    m_state->tasks.append(task);
    m_state->addLog("INFO", QStringLiteral("从关注工作区创建本地任务：") + task.name);
    m_state->notifyChanged();
    m_favoriteTaskInfo->setText(QStringLiteral("已创建 · 可在任务页编辑"));
}

void MainWindow::refreshPrices()
{
    const QString selected = m_priceSkin->currentData().toString();
    {
        QSignalBlocker guard(m_priceSkin);
        m_priceSkin->clear();
        for (const auto& skin : m_state->skins) m_priceSkin->addItem(skin.name, skin.id);
        m_priceSkin->setCurrentIndex(qMax(0, m_priceSkin->findData(selected)));
    }
    const auto* skin = findSkin(m_state, m_priceSkin->currentData().toString());
    if (!skin) {
        for (auto* metric : m_priceCards) metric->setValue(QStringLiteral("—"), QStringLiteral("暂无皮肤样本"));
        m_priceChart->setSeries({});
        m_priceHistory->setRowCount(0);
        return;
    }
    if (m_catalog) {
        m_priceCards[0]->setValue(skin->priceKnown ? amount(skin->price) : QStringLiteral("—"), QStringLiteral("无观测不估算"));
        for (int i = 1; i < m_priceCards.size(); ++i) m_priceCards[i]->setValue(QStringLiteral("—"), QStringLiteral("暂无历史采样"));
        m_priceChart->setSeries({});
        m_priceChart->setCaption(skin->name);
        m_priceHistory->setRowCount(0);
        return;
    }
    const auto values = series(skin->price);
    m_priceCards[0]->setValue(amount(skin->price), QStringLiteral("演示报价 · 非实际成交价"));
    m_priceCards[1]->setValue(amount(*std::min_element(values.begin(), values.end())), QStringLiteral("合成样本范围"));
    m_priceCards[2]->setValue(amount(*std::max_element(values.begin(), values.end())), QStringLiteral("合成样本范围"));
    m_priceChart->setSeries(values);
    m_priceChart->setCaption(skin->name + QStringLiteral(" · 演示数据"));
    m_priceHistory->setRowCount(6);
    for (int row = 0; row < 6; ++row) {
        const int index = values.size() - 1 - row;
        put(m_priceHistory, row, 0, QStringLiteral("样本 %1").arg(index + 1), FluentTheme::secondary);
        put(m_priceHistory, row, 1, skin->name);
        put(m_priceHistory, row, 2, amount(values[index]));
        put(m_priceHistory, row, 3, (m_catalog ? QStringLiteral("已知报价") : QStringLiteral("演示报价")), FluentTheme::secondary);
        put(m_priceHistory, row, 4, QStringLiteral("本地合成 · 非游戏采集"), FluentTheme::secondary);
    }
}

void MainWindow::refreshStats()
{
    const int scans = m_state->simulatedScans, matches = m_state->simulatedMatches, success = m_state->simulatedSuccess;
    m_statsCards[0]->setValue(QString::number(scans), (m_catalog ? QStringLiteral("尚无运行记录") : QStringLiteral("本次演示扫描条目")));
    m_statsCards[1]->setValue(QString::number(matches), QStringLiteral("符合模拟筛选条件"));
    m_statsCards[2]->setValue(QString::number(success), QStringLiteral("不是实际成交"));
    m_statsCards[3]->setValue(QStringLiteral("—"), QStringLiteral("未连接执行模块"));
    const QVector<int> values = {qMax(0, scans - matches), qMax(0, matches - success), success};
    for (int i = 0; i < values.size(); ++i) {
        const double fraction = scans > 0 ? values[i] * 100.0 / scans : 0;
        m_reasonLabels[i]->setText(QStringLiteral("%1 · %2%").arg(values[i]).arg(fraction, 0, 'f', 1));
        m_reasonBars[i]->setValue(qRound(fraction));
    }
    m_statsNote->setText(m_catalog ? QStringLiteral("尚无实际运行记录；不生成模拟成交或支出。") : QStringLiteral("模拟未决：0。统计只描述本地模拟引擎，不代表实际游戏结果。"));
    if (m_catalog) {
        for (auto* metric : m_statsCards) metric->setValue(QStringLiteral("—"), QStringLiteral("暂无实际记录"));
        for (auto* reason : m_reasonLabels) reason->setText(QStringLiteral("—"));
        for (auto* bar : m_reasonBars) bar->setValue(0);
    }
}

void MainWindow::refreshLogs()
{
    m_logsTable->setRowCount(0);
    const QString query = m_logSearch->text().trimmed();
    for (const auto& log : m_state->logs) {
        if (m_logLevel->currentIndex() > 0 && log.level != m_logLevel->currentText()) continue;
        if (!query.isEmpty() && !log.message.contains(query, Qt::CaseInsensitive) && !log.level.contains(query, Qt::CaseInsensitive)) continue;
        const int row = m_logsTable->rowCount();
        m_logsTable->insertRow(row);
        put(m_logsTable, row, 0, log.time, FluentTheme::secondary);
        putStatus(m_logsTable, row, 1, log.level, levelTone(log.level));
        put(m_logsTable, row, 2, log.message)->setToolTip(log.message);
    }
    m_logCount->setText(QStringLiteral("显示 %1 / %2 条").arg(m_logsTable->rowCount()).arg(m_state->logs.size()));
    if (m_autoScrollLogs && m_autoScrollLogs->isChecked()) m_logsTable->scrollToTop();
}

void MainWindow::showHelp()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("helpDialog"));
    dialog.setWindowTitle(QStringLiteral("帮助"));
    auto* outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* content = new QFrame;
    content->setObjectName(QStringLiteral("dialogContent"));
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(24, 22, 24, 22);
    body->setSpacing(0);
    body->addWidget(label(QStringLiteral("使用说明"), QStringLiteral("dialogTitle")));
    body->addSpacing(16);
    const QVector<QPair<QString, QString>> steps = {
        {QStringLiteral("关注"), QStringLiteral("筛选或搜索皮肤，在右侧填写最高价格、成色、最大磨损和数量后创建任务。")},
        {QStringLiteral("任务"), QStringLiteral("管理收藏任务：目标皮肤、成色、价格区间、最大磨损、限制量与启用开关。")},
        {QStringLiteral("运行"), QStringLiteral("设置购买延迟、连点模式、品级限购、挂机与自动收藏参数，以及运行快捷键和定时。")},
        {QStringLiteral("记录"), QStringLiteral("在日志与统计页查看运行事件和结果；导入、导出位于窗口右上角。")}};
    for (const auto& step : steps) {
        body->addWidget(label(step.first, QStringLiteral("cardTitle")));
        body->addSpacing(2);
        auto* text = label(step.second, QStringLiteral("cardCaption"));
        text->setWordWrap(true);
        body->addWidget(text);
        body->addSpacing(14);
    }
    body->addWidget(label(QStringLiteral("运行快捷键：%1（接入执行模块后生效）").arg(m_state->run.hotkey), QStringLiteral("cardCaption")));
    body->addSpacing(4);
    auto* demo = label((m_catalog ? QStringLiteral("通过“皮肤资料 / 追加赛季”维护目录。菜单颜色、极品 / 优品与任务成色独立保存；未知市场数据保持空白。") : QStringLiteral("当前版本是本地演示前端：不连接游戏，不读取实时市场，也不执行点击或购买。")), QStringLiteral("tertiaryLabel"));
    demo->setWordWrap(true);
    body->addWidget(demo);
    outer->addWidget(content, 1);
    auto* commands = new QFrame;
    commands->setObjectName(QStringLiteral("dialogCommands"));
    commands->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* commandRow = new QHBoxLayout(commands);
    commandRow->setContentsMargins(24, 20, 24, 20);
    commandRow->addStretch();
    auto* close = button(QStringLiteral("知道了"), QStringLiteral("helpCloseButton"), ButtonKind::Accent);
    close->setDefault(true);
    close->setMinimumWidth(120);
    commandRow->addWidget(close);
    outer->addWidget(commands);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    fitDialog(dialog, 560);
    dialog.exec();
}

void MainWindow::importConfiguration()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入只读预览"), QString(),
                                                      QStringLiteral("配置或 13 列文本 (*.json *.txt *.ini);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), file.errorString());
        return;
    }
    previewImportBytes(file.readAll(), QFileInfo(path).fileName());
}

void MainWindow::previewImportBytes(const QByteArray& bytes, const QString& sourceName)
{
    if (bytes.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("导入预览"), QStringLiteral("文件为空，无法生成预览。"));
        return;
    }
    const QByteArray trimmed = bytes.trimmed();
    QJsonObject preview;
    const bool jsonBom = bytes.startsWith("\xEF\xBB\xBF") || bytes.startsWith("\xFF\xFE");
    if (jsonBom || trimmed.startsWith('{')) {
        preview = relink::config::previewV1(bytes);
    } else {
        relink::config::ImportOptions options;
        options.container = sourceName.endsWith(QStringLiteral(".ini"), Qt::CaseInsensitive)
            ? QStringLiteral("ini") : QStringLiteral("text");
        preview = relink::config::preview13Columns(bytes, options);
    }
    showImportPreview(preview, sourceName);
}

void MainWindow::previewImportDemo()
{
    const QJsonObject sample{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("demo"), true},
        {QStringLiteral("source"), QStringLiteral("in-memory PR07 fixture")},
        {QStringLiteral("skins"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("demo-01")},
                        {QStringLiteral("name"), QStringLiteral("AUG 示例")},
                        {QStringLiteral("series"), QStringLiteral("S11")},
                        {QStringLiteral("condition"), QStringLiteral("S")},
                        {QStringLiteral("price"), 650.0}}
        }},
        {QStringLiteral("tasks"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("task-demo-01")},
                        {QStringLiteral("name"), QStringLiteral("示例关注任务")},
                        {QStringLiteral("skinId"), QStringLiteral("demo-01")},
                        {QStringLiteral("minPrice"), 100.0},
                        {QStringLiteral("maxPrice"), 680.0},
                        {QStringLiteral("maxWear"), 5.0},
                        {QStringLiteral("quantity"), 1},
                        {QStringLiteral("enabled"), true},
                        {QStringLiteral("condition"), QStringLiteral("S")}}
        }},
        {QStringLiteral("run_settings"), QJsonObject{{QStringLiteral("purchaseDelayMs"), 830}}}
    };
    previewImportBytes(QJsonDocument(sample).toJson(QJsonDocument::Compact), QStringLiteral("pr07-demo.json"));
}

void MainWindow::showImportPreview(const QJsonObject& preview, const QString& sourceName)
{
    using relink::runtime::UiProjection;
    const auto view = UiProjection::importPreview(preview);
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("importPreviewDialog"));
    dialog->setWindowTitle(QStringLiteral("导入预览 · 只读"));
    dialog->setModal(true);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMinimumSize(760, 520);

    auto* outer = new QVBoxLayout(dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* content = new QFrame;
    content->setObjectName(QStringLiteral("dialogContent"));
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(24, 22, 24, 18);
    body->setSpacing(8);
    body->addWidget(label(QStringLiteral("导入预览"), QStringLiteral("dialogTitle")));
    auto* source = label(QStringLiteral("%1 · %2 · %3").arg(sourceName, view.format, view.encoding), QStringLiteral("importPreviewSource"));
    source->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->addWidget(source);
    auto* hash = label(QStringLiteral("SHA-256  %1").arg(view.sourceHash), QStringLiteral("importPreviewHash"));
    hash->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->addWidget(hash);
    auto* status = label(view.statusText, QStringLiteral("importPreviewStatus"));
    status->setProperty("status", view.errorCount > 0 ? "negative" : (view.reviewRows > 0 ? "caution" : "positive"));
    body->addWidget(status);
    auto* policy = label(QStringLiteral("候选策略：enabled=false · review_required=true · 仅供审阅"), QStringLiteral("importPreviewPolicy"));
    policy->setWordWrap(true);
    body->addWidget(policy);

    auto* table = new QTableWidget;
    table->setObjectName(QStringLiteral("importPreviewTable"));
    table->setColumnCount(5);
    table->setHorizontalHeaderLabels({QStringLiteral("行"), QStringLiteral("状态"), QStringLiteral("候选"), QStringLiteral("数量"), QStringLiteral("诊断")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(false);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    const auto rows = preview.value(QStringLiteral("rows")).toArray();
    table->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const auto row = rows.at(i).toObject();
        const auto candidate = row.value(QStringLiteral("candidate")).toObject();
        const auto extensions = candidate.value(QStringLiteral("extensions")).toObject();
        const auto diagnostics = row.value(QStringLiteral("diagnostics")).toArray();
        QStringList codes;
        for (const auto& item : diagnostics) {
            const QString code = item.toObject().value(QStringLiteral("code")).toString();
            if (!code.isEmpty()) codes.append(code);
        }
        table->setItem(i, 0, new QTableWidgetItem(QString::number(row.value(QStringLiteral("line")).toInt(i + 1))));
        table->setItem(i, 1, new QTableWidgetItem(row.value(QStringLiteral("status")).toString()));
        QString candidateName = candidate.value(QStringLiteral("name")).toString();
        if (candidateName.isEmpty()) candidateName = extensions.value(QStringLiteral("x-product_name")).toString();
        table->setItem(i, 2, new QTableWidgetItem(candidateName));
        const auto quantity = candidate.value(QStringLiteral("quantity_candidate"));
        table->setItem(i, 3, new QTableWidgetItem(quantity.isNull() ? QStringLiteral("—") : quantity.toVariant().toString()));
        auto* diag = new QTableWidgetItem(codes.join(QStringLiteral(" · ")));
        diag->setToolTip(codes.join(QStringLiteral("\n")));
        table->setItem(i, 4, diag);
    }
    body->addWidget(table, 1);
    auto* note = label(view.readOnlyNote, QStringLiteral("importPreviewReadOnlyNote"));
    note->setWordWrap(true);
    body->addWidget(note);
    outer->addWidget(content, 1);

    auto* commands = new QFrame;
    commands->setObjectName(QStringLiteral("dialogCommands"));
    auto* commandRow = new QHBoxLayout(commands);
    commandRow->setContentsMargins(24, 16, 24, 16);
    commandRow->setSpacing(8);
    commandRow->addStretch();
    auto* apply = button(QStringLiteral("直接应用（只读）"), QStringLiteral("importPreviewApplyButton"));
    apply->setEnabled(false);
    apply->setToolTip(QStringLiteral("LegacyImportPreview 始终只读；审定另存为会生成独立方案，不会应用或修改本预览。"));
    if (m_workspace) {
        auto* review = button(QStringLiteral("审定另存为"), QStringLiteral("importPreviewReviewButton"));
        review->setEnabled(view.errorCount == 0 && view.invalidRows == 0 && m_workspace->projection().opened
                           && !m_workspace->projection().readOnly && !m_workspace->projection().active);
        commandRow->addWidget(review);
        connect(review, &QPushButton::clicked, this, [this, preview, sourceName] { showProfileReview(preview, sourceName); });
    }
    auto* close = button(QStringLiteral("关闭"), QStringLiteral("importPreviewCloseButton"), ButtonKind::Accent);
    close->setDefault(true);
    close->setMinimumWidth(112);
    commandRow->addWidget(apply);
    commandRow->addWidget(close);
    outer->addWidget(commands);
    connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
    dialog->open();
}

QWidget* MainWindow::buildWorkspaceSettings()
{
    auto* panel = card(QStringLiteral("workspacePanel"));
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(10);
    auto* heading = new QHBoxLayout;
    heading->addWidget(label(QStringLiteral("方案与回放"), QStringLiteral("sectionTitle")));
    heading->addStretch();
    m_workspaceMode = new FluentComboBox;
    m_workspaceMode->setObjectName(QStringLiteral("workspaceModeCombo"));
    m_workspaceMode->addItem(QStringLiteral("回放 · 合成数据"), QStringLiteral("Replay"));
    m_workspaceMode->addItem(QStringLiteral("本地演示 · 旧配置"), QStringLiteral("Demo"));
    m_workspaceMode->setMinimumWidth(180);
    heading->addWidget(m_workspaceMode);
    auto* control = button(QStringLiteral("回放控制"), QStringLiteral("workspaceOpenReplayButton"), ButtonKind::Subtle, Glyph::Play);
    heading->addWidget(control);
    connect(control, &QPushButton::clicked, this, [this] { setPage(0); });
    layout->addLayout(heading);
    auto* profiles = new QHBoxLayout;
    profiles->addWidget(label(QStringLiteral("已保存方案")));
    m_workspaceProfiles = new FluentComboBox;
    m_workspaceProfiles->setObjectName(QStringLiteral("workspaceProfileCombo"));
    m_workspaceProfiles->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_workspaceProfiles->setMinimumContentsLength(18);
    profiles->addWidget(m_workspaceProfiles, 1);
    m_workspaceFixture = button(QStringLiteral("选择内建回放样例"), QStringLiteral("workspaceFixtureButton"));
    profiles->addWidget(m_workspaceFixture);
    layout->addLayout(profiles);
    m_workspaceProfileMeta = label(QString(), QStringLiteral("workspaceProfileMeta"));
    m_workspaceProfileMeta->setWordWrap(true);
    m_workspaceProfileMeta->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_workspaceProfileMeta);
    m_workspaceReviewState = label(QString(), QStringLiteral("workspaceReviewState"));
    m_workspaceReviewState->setWordWrap(true);
    layout->addWidget(m_workspaceReviewState);
    auto* note = label(QStringLiteral("审定保存、当前选择与运行快照相互独立。审定方案保存后仍为未启用；内建样例用于验证合成回放流程。"), QStringLiteral("workspaceProfileNote"));
    note->setProperty("tertiary", true);
    note->setWordWrap(true);
    layout->addWidget(note);
    m_workspaceError = label(QString(), QStringLiteral("workspaceErrorLabel"));
    m_workspaceError->setWordWrap(true);
    m_workspaceError->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_workspaceError->setProperty("status", "negative");
    layout->addWidget(m_workspaceError);
    m_workspaceRecovery = label(QString(), QStringLiteral("workspaceRecoveryLabel"));
    m_workspaceRecovery->setWordWrap(true);
    m_workspaceRecovery->setProperty("status", "caution");
    layout->addWidget(m_workspaceRecovery);
    m_workspaceLedger = label(QString(), QStringLiteral("workspaceLedgerSummary"));
    m_workspaceLedger->setWordWrap(true);
    layout->addWidget(m_workspaceLedger);
    m_workspaceClock = label(QString(), QStringLiteral("workspaceClockLabel"));
    m_workspaceClock->setWordWrap(true);
    m_workspaceClock->setProperty("tertiary", true);
    layout->addWidget(m_workspaceClock);
    auto* listingHeading = label(QStringLiteral("回放卖单 · 观测价与模拟成交价分开记录"), QStringLiteral("groupLabel"));
    layout->addWidget(listingHeading);
    m_workspaceListings = table({QStringLiteral("商品 / 卖单"), QStringLiteral("观测价"), QStringLiteral("模拟成交价"),
                                 QStringLiteral("状态 / 原因"), QStringLiteral("来源 / 时点")},
                                QStringLiteral("workspaceListingsTable"), RowStyle::Lines);
    auto* columns = m_workspaceListings->horizontalHeader();
    columns->setStretchLastSection(false);
    columns->setSectionResizeMode(0, QHeaderView::Stretch);
    columns->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    columns->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    columns->setSectionResizeMode(3, QHeaderView::Stretch);
    columns->setSectionResizeMode(4, QHeaderView::Stretch);
    m_workspaceListings->setMinimumHeight(200);
    m_workspaceListings->setMaximumHeight(320);
    m_workspaceListings->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    layout->addWidget(m_workspaceListings);
    connect(m_workspaceMode, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_refreshing || index < 0) return;
        const auto mode = m_workspaceMode->itemData(index).toString();
        if (mode == m_workspace->projection().mode) return;
        if (!confirmWorkspaceTransition(QStringLiteral("切换模式"))) { refreshWorkspace(); return; }
        if (m_workspace->setMode(mode) && mode != QStringLiteral("Demo")) m_state->pauseSimulation();
        refreshWorkspace();
    });
    connect(m_workspaceProfiles, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_refreshing || index < 0) return;
        const auto id = m_workspaceProfiles->itemData(index).toString();
        if (id.isEmpty() || id == m_workspace->projection().selectedProfileId) return;
        if (!confirmWorkspaceTransition(QStringLiteral("切换方案"))) { refreshWorkspace(); return; }
        m_workspace->selectProfile(id);
        refreshWorkspace();
    });
    connect(m_workspaceFixture, &QPushButton::clicked, this, [this] {
        if (confirmWorkspaceTransition(QStringLiteral("选择内建回放样例"))) m_workspace->selectBuiltInFixture();
    });
    return panel;
}

QWidget* MainWindow::buildWorkspaceRecords()
{
    m_workspaceRecordsPanel = new WorkspaceRecordsPanel;
    connect(m_workspaceRecordsPanel, &WorkspaceRecordsPanel::runSelected, this, [this](const QString& runId) {
        if (m_refreshing) return;
        m_workspace->selectRun(runId);
        refreshWorkspace();
    });
    connect(m_workspaceRecordsPanel, &WorkspaceRecordsPanel::exportRequested, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出已提交回放记录"),
            QStringLiteral("replay-records.csv"), QStringLiteral("CSV (*.csv)"));
        if (!path.isEmpty()) exportWorkspaceRecordsTo(path);
    });
    return m_workspaceRecordsPanel;
}

bool MainWindow::exportWorkspaceRecordsTo(const QString& path)
{
    if (!m_workspace || path.isEmpty()) return false;
    const bool result = m_workspace->exportRecordsCsv(path);
    refreshWorkspace();
    if (result && m_workspaceRecordsPanel)
        m_workspaceRecordsPanel->showExportResult(path);
    return result;
}

void MainWindow::refreshWorkspace()
{
    if (!m_workspace || !m_workspaceProfiles || !m_workspaceRecordsPanel) return;
    const auto& view = m_workspace->projection();
    const QSignalBlocker profilesGuard(m_workspaceProfiles), modeGuard(m_workspaceMode);
    m_workspaceMode->setCurrentIndex(m_workspaceMode->findData(view.mode));
    m_workspaceMode->setEnabled(view.opened && !view.active && !view.dirty);
    m_workspaceProfiles->clear();
    m_workspaceProfiles->addItem(QStringLiteral("请选择方案 · 不自动启用"), QString());
    QString profileText = QStringLiteral("尚未选择方案；保存审定方案不会改变当前选择。");
    QString profileDetails;
    QString reviewText = QStringLiteral("预览只读 / 审定另存 / 手动选择");
    QString reviewDetails;
    for (const auto& profile : view.profiles) {
        m_workspaceProfiles->addItem(QStringLiteral("%1 · r%2%3").arg(profile.name).arg(profile.revision)
            .arg(profile.builtin ? QStringLiteral(" · 内建样例") : QStringLiteral(" · 已保存")), profile.id);
        const int index = m_workspaceProfiles->count() - 1;
        m_workspaceProfiles->setItemData(index, QStringLiteral("%1\n来源：%2\nSHA-256：%3\n%4")
            .arg(profile.id, profile.source, profile.sourceHash, profile.path), Qt::ToolTipRole);
        if (profile.id == view.selectedProfileId) {
            profileText = QStringLiteral("当前选择：%1 · 版本 %2 · %3\n来源：%4")
                .arg(profile.name).arg(profile.revision).arg(shortWorkspaceId(profile.id), workspaceText(profile.source));
            profileDetails = QStringLiteral("方案：%1\n标识：%2\n版本：%3\n来源：%4\nSHA-256：%5\n%6")
                .arg(profile.name, profile.id).arg(profile.revision)
                .arg(profile.source, profile.sourceHash.isEmpty() ? QStringLiteral("内建固定样例") : profile.sourceHash, profile.path);
            reviewText = QStringLiteral("审定状态：%1 · %2").arg(workspaceText(profile.reviewState),
                profile.activationRequired ? QStringLiteral("规则未启用，需另行启用") : QStringLiteral("合成回放可用"));
            reviewDetails = QStringLiteral("%1\nactivation_required=%2").arg(profile.reviewState,
                profile.activationRequired ? QStringLiteral("true") : QStringLiteral("false"));
        }
    }
    m_workspaceProfiles->setCurrentIndex(qMax(0, m_workspaceProfiles->findData(view.selectedProfileId)));
    m_workspaceProfiles->setEnabled(view.opened && !view.active && !view.dirty);
    m_workspaceFixture->setEnabled(view.opened && !view.active && !view.dirty);
    m_workspaceProfileMeta->setText(profileText);
    m_workspaceProfileMeta->setToolTip(profileDetails);
    m_workspaceReviewState->setText(reviewText);
    m_workspaceReviewState->setToolTip(reviewDetails);
    const QString error = !view.lastError.isEmpty() ? view.lastError : m_workspace->lastError();
    m_workspaceError->setVisible(!error.isEmpty());
    m_workspaceError->setText(QStringLiteral("操作未提交，保留上次已提交数据：%1").arg(error));
    const bool recovered = std::any_of(view.runs.cbegin(), view.runs.cend(), [&](const auto& run) {
        return run.id == view.selectedRunId && run.recovered;
    });
    const bool showRecovery = recovered || view.unknownCount > 0;
    m_workspaceRecovery->setVisible(showRecovery);
    m_workspaceRecovery->setText(QStringLiteral("%1结果未知：%2 · 仍占用预留：%3。未知结果既非成功也非失败；恢复后只读，不自动重发或继续。")
        .arg(recovered ? QStringLiteral("已载入异常退出后的恢复记录。") : QString())
        .arg(view.unknownCount).arg(view.reservationCount));
    m_workspaceLedger->setText(QStringLiteral("已提交账本：成功 %1 · 失败 %2 · 结果未知 %3 · 预留 %4 · %5 条记录%6")
        .arg(view.confirmedSuccess).arg(view.failedCount).arg(view.unknownCount).arg(view.reservationCount).arg(view.records.size())
        .arg(view.historyReadOnly ? QStringLiteral(" · 历史只读") : QString()));
    if (m_workspaceStatsCards.size() == 4) {
        const QVector<int> counts{view.confirmedSuccess, view.failedCount, view.unknownCount, view.reservationCount};
        for (int i = 0; i < counts.size(); ++i)
            m_workspaceStatsCards[i]->setValue(QString::number(counts[i]), view.historyReadOnly ? QStringLiteral("历史快照 · 只读") : QStringLiteral("仅已提交数据"));
        m_workspaceStatsSummary->setText(QStringLiteral("匹配 %1 · 收藏 %2 · 已发起 %3 · 不匹配 %4 · 待审 %5\n模式 %6 · 运行 %7 · 版本 %8；未知结果不计入成功或失败，观测价不计入成交。")
            .arg(view.matchedCount).arg(view.collectedCount).arg(view.dispatchedCount).arg(view.noMatchCount).arg(view.needsReviewCount)
            .arg(workspaceText(view.mode), shortWorkspaceId(view.selectedRunId)).arg(view.runProfileRevision));
        m_workspaceStatsSummary->setToolTip(QStringLiteral("运行：%1\n模式：%2").arg(view.selectedRunId, view.mode));
    }
    m_workspaceClock->setText(QStringLiteral("模式 %1 · 来源 %2 · 时钟 %3 · 逻辑时间 %4 ms · 图像文件写入 %5")
        .arg(workspaceText(view.mode), view.source.isEmpty() ? QStringLiteral("尚未运行") : workspaceText(view.source),
             shortWorkspaceId(view.clockDomainId))
        .arg(view.nowMonoMs).arg(view.imageFileWriteCount));
    m_workspaceClock->setToolTip(QStringLiteral("模式：%1\n来源：%2\n时钟：%3\n运行：%4")
        .arg(view.mode, view.source, view.clockDomainId, view.selectedRunId));
    m_workspaceListings->setRowCount(view.listings.size());
    auto shown = [](const QString& value) { return value.isEmpty() ? QStringLiteral("—") : value; };
    for (int i = 0; i < view.listings.size(); ++i) {
        const auto& row = view.listings[i];
        put(m_workspaceListings, i, 0, QStringLiteral("%1 / %2").arg(row.productName, row.listingId))->setData(Qt::UserRole, row.listingId);
        put(m_workspaceListings, i, 1, shown(row.observedPrice));
        put(m_workspaceListings, i, 2, shown(row.confirmedPrice));
        QString reason = QStringLiteral("%1 · %2").arg(workspaceText(row.status), workspaceText(row.reason));
        if (row.stale && row.reason != QStringLiteral("OBSERVATION_STALE")) reason += QStringLiteral(" · 观测已过期");
        if (!row.missingFields.isEmpty()) {
            QStringList fields;
            for (const auto& field : row.missingFields) fields.append(workspaceText(field));
            reason += QStringLiteral(" · 缺失：") + fields.join(QStringLiteral("、"));
        }
        put(m_workspaceListings, i, 3, reason);
        put(m_workspaceListings, i, 4, QStringLiteral("%1 · %2 ms · %3").arg(workspaceText(row.source)).arg(row.observedMonoMs).arg(shortWorkspaceId(row.clockDomainId)));
        for (int col = 0; col < m_workspaceListings->columnCount(); ++col)
            m_workspaceListings->item(i, col)->setToolTip(QStringLiteral("商品 %1\n卖单 %2\n观察 %3\n状态 %4\n原因 %5\n来源 %6 · 时钟 %7 · %8 ms\n缺失字段 %9")
                .arg(row.productId, row.listingId, row.observationId, row.status, row.reason, row.source, row.clockDomainId)
                .arg(row.observedMonoMs).arg(row.missingFields.join(QStringLiteral(", "))));
    }
    m_workspaceRecordsPanel->refresh(view, error);
    if (m_replaySourceLabel) {
        m_replaySourceLabel->setText(QStringLiteral("%1 · %2").arg(workspaceText(view.mode), workspaceText(view.source)));
        m_replaySourceLabel->setToolTip(QStringLiteral("%1 · %2").arg(view.mode, view.source));
        m_replayStateLabel->setText(workspaceText(view.runState));
        m_replayStateLabel->setToolTip(view.runState);
        m_replayStateLabel->setProperty("status", view.runState);
        m_replayReasonLabel->setText(!error.isEmpty() ? QStringLiteral("操作失败：") + error
            : QStringLiteral("已提交：成功 %1 / 结果未知 %2 / 预留 %3；未提交操作不计入账本。%4")
                .arg(view.confirmedSuccess).arg(view.unknownCount).arg(view.reservationCount)
                .arg(view.selectedProfileId.isEmpty() ? QStringLiteral("请先在运行页显式选择回放样例。") : QString()));
        m_replayProgressLabel->setText(QStringLiteral("步骤 %1/%2 · %3 ms · 运行 %4 · 版本 %5%6")
            .arg(view.fixtureStep).arg(view.fixtureStepCount).arg(view.nowMonoMs).arg(shortWorkspaceId(view.selectedRunId))
            .arg(view.runProfileRevision).arg(view.historyReadOnly ? QStringLiteral(" · 历史只读") : QString()));
        m_replayProgressLabel->setToolTip(QStringLiteral("运行：%1\n固定方案：%2\n时钟：%3")
            .arg(view.selectedRunId, view.runProfileId, view.clockDomainId));
        m_replayStartButton->setEnabled(view.canStart && view.mode == QStringLiteral("Replay"));
        m_replayPauseButton->setEnabled(view.canPause);
        m_replayResumeButton->setEnabled(view.canResume);
        m_replayStopButton->setEnabled(view.canStop);
        m_replayStepButton->setEnabled(view.canStep);
        m_replayAdvanceButton->setEnabled(view.canStep);
    }
    const bool demo = view.mode == QStringLiteral("Demo");
    m_startButton->setEnabled(demo && !view.active && !m_state->simulationRunning);
    m_pauseButton->setEnabled(demo && m_state->simulationRunning);
    m_taskPage->setSimulationAvailable(demo && !view.active && !m_state->simulationRunning);
    if (!demo) {
        m_statusBadge->setText(QStringLiteral("●  回放 · %1").arg(workspaceText(view.runState)));
        m_statusBadge->setToolTip(QStringLiteral("合成回放 / 已提交账本 · 未连接游戏\n%1").arg(view.runState));
    }
}

bool MainWindow::configurationDirty() const
{
    return m_savedConfiguration != QJsonDocument(encodeV1Config(m_state->skins, m_state->tasks, m_state->run)).toJson(QJsonDocument::Compact);
}

bool MainWindow::confirmWorkspaceTransition(const QString& action)
{
    if (!configurationDirty()) return true;
    QMessageBox prompt(QMessageBox::Question, QStringLiteral("保留未保存的演示配置"),
        QStringLiteral("本地演示配置有未保存改动。%1不会把它们应用到回放方案；继续后草稿仍然保留。")
            .arg(action), QMessageBox::NoButton, this);
    prompt.setObjectName(QStringLiteral("workspaceDirtyDialog"));
    auto* back = prompt.addButton(QStringLiteral("返回编辑"), QMessageBox::RejectRole);
    auto* proceed = prompt.addButton(QStringLiteral("保留草稿并继续"), QMessageBox::AcceptRole);
    prompt.setDefaultButton(back);
    prompt.exec();
    return prompt.clickedButton() == proceed;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    setProperty("skipAutomaticConfigSave", false);
    bool discardConfiguration = false;
    if (m_reviewDialog) {
        m_reviewDialog->raise();
        m_reviewDialog->activateWindow();
        event->ignore();
        return;
    }
    if (m_workspace && configurationDirty()) {
        QMessageBox prompt(QMessageBox::Question, QStringLiteral("保存配置后退出？"),
            QStringLiteral("本地演示配置有未保存改动。"), QMessageBox::NoButton, this);
        prompt.setObjectName(QStringLiteral("workspaceCloseDirtyDialog"));
        auto* save = prompt.addButton(QStringLiteral("保存并退出"), QMessageBox::AcceptRole);
        auto* discard = prompt.addButton(QStringLiteral("放弃改动并退出"), QMessageBox::DestructiveRole);
        auto* back = prompt.addButton(QStringLiteral("返回"), QMessageBox::RejectRole);
        prompt.setDefaultButton(back);
        prompt.exec();
        if (prompt.clickedButton() == save) {
            saveSettings();
            if (configurationDirty()) { event->ignore(); return; }
        } else if (prompt.clickedButton() == discard) {
            discardConfiguration = true;
        } else { event->ignore(); return; }
    }
    if (m_workspace && m_workspace->projection().active && !m_workspace->stop()) {
        event->ignore();
        refreshWorkspace();
        return;
    }
    setProperty("skipAutomaticConfigSave", discardConfiguration);
    QMainWindow::closeEvent(event);
}

void MainWindow::showProfileReview(const QJsonObject& preview, const QString& sourceName)
{
    if (!m_workspace) return;
    if (m_reviewDialog) { m_reviewDialog->raise(); return; }
    auto* dialog = new ReviewDialog(this);
    m_reviewDialog = dialog;
    dialog->setObjectName(QStringLiteral("profileReviewDialog"));
    dialog->setWindowTitle(QStringLiteral("审定另存为 · 不启用规则"));
    dialog->setModal(true);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMinimumWidth(620);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(12);
    layout->addWidget(label(QStringLiteral("审定另存为"), QStringLiteral("dialogTitle")));
    auto* intro = label(QStringLiteral("来源：%1\n逐项核对导入预览后确认。原始预览不变；原子保存独立 ConfigV2，规则保持未启用。")
        .arg(sourceName), QStringLiteral("profileReviewIntro"));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit;
    name->setObjectName(QStringLiteral("profileReviewName"));
    name->setPlaceholderText(QStringLiteral("例如：已核对的关注方案"));
    auto* quantum = new QLineEdit(QStringLiteral("0.01"));
    quantum->setObjectName(QStringLiteral("profileReviewQuantum"));
    auto* unit = new QLineEdit(QStringLiteral("显示单位"));
    unit->setObjectName(QStringLiteral("profileReviewUnit"));
    form->addRow(QStringLiteral("方案名称"), name);
    form->addRow(QStringLiteral("价格最小单位（十进制文本）"), quantum);
    form->addRow(QStringLiteral("单位显示名"), unit);
    layout->addLayout(form);
    QVector<QCheckBox*> confirmations;
    const QStringList ids{QStringLiteral("profileReviewProducts"), QStringLiteral("profileReviewPriceRange"),
        QStringLiteral("profileReviewTaxonomy"), QStringLiteral("profileReviewUnknownColumns"), QStringLiteral("profileReviewQuantityUnbound")};
    const QStringList texts{QStringLiteral("我已核对商品名称与对应关系"), QStringLiteral("我已核对价格范围、显示单位和最小单位"),
        QStringLiteral("我已核对品级与成色分类"), QStringLiteral("保留无法解释的原始列，不自行推断其含义"),
        QStringLiteral("数量保持未绑定，不把候选数量用于自动执行")};
    for (int i = 0; i < ids.size(); ++i) {
        auto* check = new QCheckBox(texts[i]);
        check->setObjectName(ids[i]);
        confirmations.append(check);
        layout->addWidget(check);
    }
    auto* status = label(QStringLiteral("请填写名称、最小单位，并完成全部 5 项确认。"), QStringLiteral("profileReviewStatus"));
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);
    auto* commands = new QHBoxLayout;
    commands->addStretch();
    auto* cancel = button(QStringLiteral("取消"), QStringLiteral("profileReviewCancelButton"));
    auto* save = button(QStringLiteral("保存审定方案"), QStringLiteral("profileReviewSaveButton"), ButtonKind::Accent, Glyph::Save);
    save->setEnabled(false);
    commands->addWidget(cancel);
    commands->addWidget(save);
    layout->addLayout(commands);
    const auto edited = [this, dialog, name, quantum, confirmations, save] {
        dialog->dirty = true;
        m_workspace->setReviewDirty(true);
        bool complete = !name->text().trimmed().isEmpty() && !quantum->text().trimmed().isEmpty();
        for (const auto* check : confirmations) complete = complete && check->isChecked();
        save->setEnabled(complete);
    };
    connect(name, &QLineEdit::textChanged, dialog, edited);
    connect(quantum, &QLineEdit::textChanged, dialog, edited);
    connect(unit, &QLineEdit::textChanged, dialog, edited);
    for (auto* check : confirmations) connect(check, &QCheckBox::toggled, dialog, edited);
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(dialog, &QDialog::finished, this, [this](int) {
        m_workspace->discardReviewDraft();
        m_reviewDialog = nullptr;
    });
    connect(save, &QPushButton::clicked, dialog, [this, dialog, preview, name, quantum, unit, confirmations, status] {
        relink::config::ReviewChoices choices;
        choices.profileName = name->text().trimmed();
        choices.quantum = quantum->text().trimmed();
        choices.unitDisplayName = unit->text().trimmed();
        choices.confirmProducts = confirmations[0]->isChecked();
        choices.confirmPriceRange = confirmations[1]->isChecked();
        choices.confirmTaxonomy = confirmations[2]->isChecked();
        choices.preserveUnknownColumns = confirmations[3]->isChecked();
        choices.keepQuantityUnbound = confirmations[4]->isChecked();
        if (!m_workspace->saveReviewedPreview(preview, choices)) {
            status->setText(QStringLiteral("保存未提交，草稿已保留：%1").arg(m_workspace->lastError()));
            return;
        }
        dialog->dirty = false;
        dialog->accept();
    });
    dialog->open();
}

void MainWindow::exportConfiguration()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出本地配置"), "relink-config.json", QStringLiteral("JSON 配置 (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    if (!m_state->saveTo(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    m_state->addLog("INFO", QStringLiteral("已导出本地配置"));
    m_state->notifyChanged();
}

void MainWindow::exportPrices()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出已知报价"), "relink-prices.csv", QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty()) return;
    QString error;
    if (!m_state->exportPricesCsv(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    m_state->addLog("INFO", QStringLiteral("已导出报价 CSV，未知值留空"));
    m_state->notifyChanged();
}

void MainWindow::saveSettings()
{
    QSettings preferences;
    preferences.setValue("ui/autoScrollLogs", m_autoScrollLogs->isChecked());
    preferences.setValue("ui/compactTables", m_compactTables->isChecked());
    preferences.sync();
    QString path = m_state->configPath;
    if (path.isEmpty()) path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/config.json";
    QDir().mkpath(QFileInfo(path).absolutePath());
    QString error;
    if (m_state->saveTo(path, &error)) {
        m_savedConfiguration = QJsonDocument(encodeV1Config(m_state->skins, m_state->tasks, m_state->run)).toJson(QJsonDocument::Compact);
        m_settingsMessage->setText(QStringLiteral("设置与本地配置已保存"));
        m_state->configPath = path;
        m_state->addLog("INFO", QStringLiteral("已保存界面设置与本地配置"));
        m_state->notifyChanged();
    } else {
        m_settingsMessage->setText(QStringLiteral("配置保存失败：") + error);
    }
}

void MainWindow::applyDensity()
{
    const bool compact = m_compactTables && m_compactTables->isChecked();
    for (auto* item : findChildren<QTableWidget*>()) {
        int size = compact ? 36 : 44;
        if (item == m_favoritesTable) size = compact ? 52 : 64;
        item->verticalHeader()->setDefaultSectionSize(size);
    }
    if (m_taskPage) m_taskPage->setCompact(compact);
    if (m_overviewLogs)
        m_overviewLogs->setFixedHeight(qMax(1, m_overviewLogs->rowCount()) * m_overviewLogs->verticalHeader()->defaultSectionSize() + 2);
}


void MainWindow::manageSkinCatalog()
{
    if (!m_catalog) return;
    auto* dialog = new SkinCatalogDialog(m_catalog, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModal(true);
    connect(dialog, &SkinCatalogDialog::catalogChanged, this, [this] {
        QString error;
        if (!relink::application::applyCatalogConfiguration(*m_state, m_catalog->catalog(), nullptr, &error)) {
            QMessageBox::warning(this, QStringLiteral("目录同步失败"), error);
            return;
        }
        m_state->notifyChanged();
    });
    dialog->open();
}
