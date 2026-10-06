#include "run_settings_page.h"
#include "domain.h"
#include "fluenttheme.h"
#include "widgets.h"
#include "ui/presentation/ui_helpers.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTime>
#include <QVBoxLayout>

namespace relink::ui {
RunSettingsPage::RunSettingsPage(AppState* state, QWidget* workspacePanel, QWidget* parent)
    : QWidget(parent), m_state(state), m_workspacePresent(workspacePanel != nullptr)
{
    auto* page = this;
    page->setObjectName(QStringLiteral("runSettingsPage"));
    auto* layout = pageLayout(page);
    QLabel* profileCaption = nullptr;
    auto* header = pageHeader(layout, QStringLiteral("运行设置"), &profileCaption);
    m_saveMessage = label(QString(), QStringLiteral("settingsMessage"));
    auto* save = button(QStringLiteral("保存参数"), QStringLiteral("saveRunSettingsButton"), ButtonKind::Accent, Glyph::Save);
    header->addWidget(m_saveMessage, 0, Qt::AlignVCenter);
    header->addSpacing(8);
    header->addWidget(save, 0, Qt::AlignVCenter);
    m_runBinders.append([this, profileCaption] {
        profileCaption->setText((m_workspacePresent ? QStringLiteral("本地演示参数 %1") : QStringLiteral("方案 %1")).arg(m_state->run.profile));
    });
    connect(save, &QPushButton::clicked, this, &RunSettingsPage::saveRequested);
    if (workspacePanel) {
        layout->addSpacing(20);
        layout->addWidget(workspacePanel);
        layout->addSpacing(20);
    }

    const auto group = [&](const QString& text, bool first = false) {
        layout->addSpacing(first ? 20 : 28);
        layout->addWidget(label(text, QStringLiteral("groupLabel")));
        layout->addSpacing(8);
    };
    const auto row = [&](QWidget* item) { layout->addWidget(item); layout->addSpacing(4); };
    // PR07: import is intentionally a preview-only operation.  The button is
    // deterministic and uses an in-memory fixture so the self-test can verify
    // the dialog without opening a file chooser or touching the workspace.
    auto* importPreviewButton = button(QStringLiteral("打开只读预览"), QStringLiteral("runImportPreviewButton"), ButtonKind::Standard, Glyph::Import);
    connect(importPreviewButton, &QPushButton::clicked, this, &RunSettingsPage::importPreviewRequested);
    row(settingsCard(glyphLabel(Glyph::Import), QStringLiteral("导入预览"),
                     QStringLiteral("先检查行状态与诊断；预览始终只读，完整审定后可另存方案，保存不启用规则。"),
                     importPreviewButton));
    const auto text = [](const QString& value) { return label(value); };
    const auto strip = [](std::initializer_list<QWidget*> parts) {
        auto* host = new QWidget;
        auto* line = new QHBoxLayout(host);
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(8);
        for (auto* part : parts) line->addWidget(part);
        return host;
    };
    // Each control writes its field immediately; binders push the model back
    // into the controls after an import, a reset or a save.
    const auto toggle = [&](const QString& name, bool RunSettings::* field) {
        auto* item = new ToggleSwitch;
        item->setObjectName(name);
        item->setStateTextVisible(true);
        connect(item, &QCheckBox::toggled, this, [this, field](bool on) { m_state->run.*field = on; });
        m_runBinders.append([this, item, field] { QSignalBlocker guard(item); item->setChecked(m_state->run.*field); });
        return item;
    };
    const auto integer = [&](const QString& name, int RunSettings::* field, int low, int high, const QString& suffix, int width) {
        auto* item = new FluentSpinBox;
        item->setObjectName(name);
        item->setRange(low, high);
        item->setSuffix(suffix);
        item->setFixedWidth(width);
        connect(item, &QSpinBox::valueChanged, this, [this, field](int value) { m_state->run.*field = value; });
        m_runBinders.append([this, item, field] {
            if (item->hasFocus()) return;
            QSignalBlocker guard(item);
            item->setValue(m_state->run.*field);
        });
        return item;
    };
    const auto step = [&](const QString& name, double RunSettings::* field) {
        auto* item = new FluentDoubleSpinBox;
        item->setObjectName(name);
        item->setRange(0, 1000);
        item->setDecimals(1);
        item->setSingleStep(0.5);
        item->setSuffix(QStringLiteral(" ms"));
        item->setFixedWidth(116);
        connect(item, &QDoubleSpinBox::valueChanged, this, [this, field](double value) { m_state->run.*field = value; });
        m_runBinders.append([this, item, field] {
            if (item->hasFocus()) return;
            QSignalBlocker guard(item);
            item->setValue(m_state->run.*field);
        });
        return item;
    };

    group(QStringLiteral("运行"), true);
    auto* profile = new QLineEdit;
    profile->setObjectName(QStringLiteral("runProfileEdit"));
    profile->setMaxLength(40);
    profile->setFixedWidth(240);
    connect(profile, &QLineEdit::editingFinished, this, [this, profile] {
        const QString value = profile->text().trimmed();
        if (value.isEmpty()) {
            profile->setText(m_state->run.profile);
            return;
        }
        m_state->run.profile = value;
        refresh();
    });
    m_runBinders.append([this, profile] { if (!profile->hasFocus()) profile->setText(m_state->run.profile); });
    row(settingsCard(glyphLabel(Glyph::Tag), QStringLiteral("配置方案"), QStringLiteral("对应原程序顶部的方案标签；导出、导入时随参数一起保存"), profile));
    auto* hotkey = new FluentComboBox;
    hotkey->setObjectName(QStringLiteral("runHotkeyCombo"));
    for (int key = 1; key <= 12; ++key) hotkey->addItem(QStringLiteral("F%1").arg(key));
    hotkey->setFixedWidth(104);
    connect(hotkey, &QComboBox::currentTextChanged, this, [this](const QString& key) { if (!key.isEmpty()) m_state->run.hotkey = key; });
    m_runBinders.append([this, hotkey] { QSignalBlocker guard(hotkey); hotkey->setCurrentText(m_state->run.hotkey); });
    row(settingsCard(glyphLabel(Glyph::Keyboard), QStringLiteral("运行快捷键"), QStringLiteral("按下后开始或停止运行；接入执行模块后生效"), hotkey));
    auto* scheduleStart = new FluentTimeEdit;
    scheduleStart->setObjectName(QStringLiteral("runScheduleStart"));
    auto* scheduleStop = new FluentTimeEdit;
    scheduleStop->setObjectName(QStringLiteral("runScheduleStop"));
    for (auto* edit : {scheduleStart, scheduleStop}) {
        edit->setDisplayFormat(QStringLiteral("HH:mm"));
        edit->setFixedWidth(100);
    }
    connect(scheduleStart, &QTimeEdit::timeChanged, this, [this](const QTime& time) { m_state->run.scheduleStart = time.toString(QStringLiteral("HH:mm")); });
    connect(scheduleStop, &QTimeEdit::timeChanged, this, [this](const QTime& time) { m_state->run.scheduleStop = time.toString(QStringLiteral("HH:mm")); });
    auto* schedule = toggle(QStringLiteral("runScheduleToggle"), &RunSettings::scheduleEnabled);
    connect(schedule, &QCheckBox::toggled, scheduleStart, &QWidget::setEnabled);
    connect(schedule, &QCheckBox::toggled, scheduleStop, &QWidget::setEnabled);
    m_runBinders.append([this, scheduleStart, scheduleStop] {
        for (auto* edit : {scheduleStart, scheduleStop}) {
            edit->setEnabled(m_state->run.scheduleEnabled);
            if (edit->hasFocus()) continue;
            QSignalBlocker guard(edit);
            edit->setTime(QTime::fromString(edit == scheduleStart ? m_state->run.scheduleStart : m_state->run.scheduleStop, QStringLiteral("HH:mm")));
        }
    });
    row(settingsCard(glyphLabel(Glyph::Clock), QStringLiteral("定时运行"), QStringLiteral("在设定时间自动开始和停止；接入执行模块后生效"),
                     strip({text(QStringLiteral("开始")), scheduleStart, text(QStringLiteral("结束")), scheduleStop, schedule})));

    group(QStringLiteral("购买延迟"));
    row(settingsCard(glyphLabel(Glyph::Stopwatch), QStringLiteral("购买延迟"), QStringLiteral("每次购买操作之间的等待时间"),
                     integer(QStringLiteral("runPurchaseDelay"), &RunSettings::purchaseDelayMs, 0, 60000, QStringLiteral(" ms"), 140)));
    row(settingsCard(nullptr, QStringLiteral("动态延迟"), QStringLiteral("开启动态延迟调整（跳过抽奖页：开启）。原程序标注为不建议"),
                     toggle(QStringLiteral("runDynamicDelay"), &RunSettings::dynamicDelay), QString(), true));
    row(settingsCard(nullptr, QStringLiteral("队列已满时减延迟"), QStringLiteral("每触发设定次数，购买延迟减少一次"),
                     strip({text(QStringLiteral("触发")), integer(QStringLiteral("runQueueTrigger"), &RunSettings::queueFullTrigger, 1, 9999, QStringLiteral(" 次"), 104),
                            text(QStringLiteral("减少")), step(QStringLiteral("runQueueStep"), &RunSettings::queueFullStepMs)}), QString(), true));
    row(settingsCard(nullptr, QStringLiteral("公示期加延迟"), QStringLiteral("处于公示期时，每触发设定次数，购买延迟增加一次"),
                     strip({text(QStringLiteral("触发")), integer(QStringLiteral("runPublicityTrigger"), &RunSettings::publicityTrigger, 1, 9999, QStringLiteral(" 次"), 104),
                            text(QStringLiteral("增加")), step(QStringLiteral("runPublicityStep"), &RunSettings::publicityStepMs)}), QString(), true));

    group(QStringLiteral("连点模式"));
    auto* burst = toggle(QStringLiteral("runBurstClick"), &RunSettings::burstClick);
    auto* interval = integer(QStringLiteral("runClickInterval"), &RunSettings::clickIntervalMs, 1, 10000, QStringLiteral(" ms"), 140);
    connect(burst, &QCheckBox::toggled, interval, &QWidget::setEnabled);
    m_runBinders.append([this, interval] { interval->setEnabled(m_state->run.burstClick); });
    row(settingsCard(glyphLabel(Glyph::Mouse), QStringLiteral("连续点击"), QStringLiteral("开启连点模式，按设定间隔连续点击购买"), burst));
    row(settingsCard(nullptr, QStringLiteral("点击间隔"), QStringLiteral("两次点击之间的间隔"), interval, QString(), true));

    group(QStringLiteral("购买量限制"));
    row(settingsCard(glyphLabel(Glyph::Filter), QStringLiteral("按品级限制购买数量"), QStringLiteral("橙色、紫色、蓝色品级分别计数"),
                     strip({text(QStringLiteral("橙色")), integer(QStringLiteral("runLimitOrange"), &RunSettings::limitOrange, 0, 9999, QString(), 96),
                            text(QStringLiteral("紫色")), integer(QStringLiteral("runLimitPurple"), &RunSettings::limitPurple, 0, 9999, QString(), 96),
                            text(QStringLiteral("蓝色")), integer(QStringLiteral("runLimitBlue"), &RunSettings::limitBlue, 0, 9999, QString(), 96)})));

    group(QStringLiteral("挂机设置"));
    row(settingsCard(glyphLabel(Glyph::Sync), QStringLiteral("刷新界面"), QStringLiteral("挂机时定时刷新市场界面。原程序标注为不推荐"),
                     toggle(QStringLiteral("runRefreshPage"), &RunSettings::refreshPage)));
    row(settingsCard(glyphLabel(Glyph::SkipTo), QStringLiteral("跳过抽奖页"), QStringLiteral("挂机时自动跳过抽奖页"),
                     toggle(QStringLiteral("runSkipLottery"), &RunSettings::skipLotteryPage)));
    row(settingsCard(glyphLabel(Glyph::SkipTo), QStringLiteral("跳过成功页"), QStringLiteral("购买成功后自动跳过结果页"),
                     toggle(QStringLiteral("runSkipSuccess"), &RunSettings::skipSuccessPage)));

    group(QStringLiteral("自动收藏"));
    row(settingsCard(glyphLabel(Glyph::Star), QStringLiteral("自动收藏模式"), QStringLiteral("按收藏任务配置自动收藏符合条件的条目"),
                     toggle(QStringLiteral("runAutoCollect"), &RunSettings::autoCollect)));
    row(settingsCard(glyphLabel(Glyph::Display), QStringLiteral("收藏状态 OSD"), QStringLiteral("在游戏画面上显示收藏状态浮层"),
                     toggle(QStringLiteral("runCollectOsd"), &RunSettings::collectOsd)));
    m_runTaskSummary = label(QString(), QStringLiteral("cardCaption"));
    auto* manageTasks = link(QStringLiteral("管理任务"), QStringLiteral("runManageTasksLink"));
    connect(manageTasks, &QPushButton::clicked, this, [this] { emit navigateRequested(2); });
    row(settingsCard(glyphLabel(Glyph::Tasks), QStringLiteral("收藏任务配置"), QStringLiteral("皮肤、成色、最大磨损、价格区间与限制量"),
                     strip({m_runTaskSummary, manageTasks})));

    group(QStringLiteral("优化与记录"));
    row(settingsCard(glyphLabel(Glyph::Speed), QStringLiteral("优化"), QStringLiteral("原程序顶部的「优化」入口；具体优化项目待确认后接入"),
                     label(QStringLiteral("待确认"), QStringLiteral("settingsBadge"))));
    auto* records = link(QStringLiteral("查看日志"), QStringLiteral("runRecordsLink"));
    connect(records, &QPushButton::clicked, this, [this] { emit navigateRequested(5); });
    row(settingsCard(glyphLabel(Glyph::History), QStringLiteral("运行记录"), QStringLiteral("日志与统计页记录每次运行的事件和结果"), records));
    auto* help = link(QStringLiteral("打开帮助"), QStringLiteral("runHelpLink"));
    connect(help, &QPushButton::clicked, this, &RunSettingsPage::helpRequested);
    row(settingsCard(glyphLabel(Glyph::Help), QStringLiteral("帮助"), QStringLiteral("使用流程、快捷键与当前版本说明"), help));
    layout->addSpacing(16);
    auto* note = label(QStringLiteral("以上参数随配置一起保存、导出和导入。当前版本不连接游戏，不会执行点击或购买。"), QStringLiteral("tertiaryLabel"));
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addStretch();

}

void RunSettingsPage::refresh()
{
    for (const auto& bind : m_runBinders) bind();
    int enabled = 0;
    for (const auto& task : m_state->tasks) if (task.enabled) ++enabled;
    if (m_runTaskSummary)
        m_runTaskSummary->setText(QStringLiteral("%1 条任务 · %2 条启用").arg(m_state->tasks.size()).arg(enabled));
}

void RunSettingsPage::showSaveResult(const QString& message)
{
    m_saveMessage->setText(message);
}
} // namespace relink::ui
