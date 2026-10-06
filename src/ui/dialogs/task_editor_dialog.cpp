#include "task_editor_dialog.h"
#include "ui/presentation/ui_helpers.h"
#include "widgets.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>

namespace relink::ui {
TaskEditorDialog::TaskEditorDialog(const AppState* state, const Task* task,
                                   const QString& skinId, QWidget* parent)
    : QDialog(parent)
{
    const bool editing = task != nullptr;
    const Task existing = task ? *task : Task{};
    this->setObjectName(QStringLiteral("taskEditorDialog"));
    this->setWindowTitle(editing ? QStringLiteral("编辑模拟任务") : QStringLiteral("新增模拟任务"));
    this->setMinimumWidth(540);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    // ContentDialog layout: white content area above a Mica command bar.
    auto* content = new QFrame;
    content->setObjectName(QStringLiteral("dialogContent"));
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(24, 22, 24, 20);
    body->setSpacing(0);
    body->addWidget(label(editing ? QStringLiteral("调整任务条件") : QStringLiteral("创建一条模拟任务"), QStringLiteral("dialogTitle")));
    body->addSpacing(6);
    body->addWidget(label(QStringLiteral("这些规则只用于本地演示，不会触发实际购买。"), QStringLiteral("cardCaption")));
    body->addSpacing(20);
    auto* name = new QLineEdit;
    name->setObjectName(QStringLiteral("taskName"));
    name->setMaxLength(60);
    name->setPlaceholderText(QStringLiteral("例如：天命低价关注"));
    if (editing) name->setText(existing.name);
    auto* skin = new FluentComboBox;
    skin->setObjectName(QStringLiteral("taskSkin"));
    for (const auto& item : state->skins) skin->addItem(skinChoice(item), item.id);
    const int index = skin->findData(editing ? existing.skinId : skinId);
    if (index >= 0) skin->setCurrentIndex(index);
    const auto* defaultSkin = findSkin(state, skin->currentData().toString());
    auto* condition = new FluentComboBox;
    condition->setObjectName(QStringLiteral("taskCondition"));
    condition->addItems(conditionOptions(state));
    const QString conditionValue = editing ? existing.condition : (defaultSkin ? defaultSkin->condition : QStringLiteral("不限"));
    if (condition->findText(conditionValue) < 0) condition->addItem(conditionValue);
    condition->setCurrentText(conditionValue);
    auto* min = new FluentDoubleSpinBox;
    min->setObjectName(QStringLiteral("taskMinPrice"));
    min->setRange(0, 999999999);
    min->setDecimals(2);
    min->setValue(editing ? existing.minPrice : 0);
    auto* max = new FluentDoubleSpinBox;
    max->setObjectName(QStringLiteral("taskMaxPrice"));
    max->setRange(0, 999999999);
    max->setDecimals(2);
    max->setValue(editing ? existing.maxPrice : (defaultSkin ? defaultSkin->price : 800));
    auto* wear = new FluentDoubleSpinBox;
    wear->setObjectName(QStringLiteral("taskWear"));
    wear->setRange(0, 100);
    wear->setDecimals(6);
    wear->setValue(editing ? existing.maxWear : 5);
    auto* quantity = new FluentSpinBox;
    quantity->setObjectName(QStringLiteral("taskQuantity"));
    quantity->setRange(1, 9999);
    quantity->setValue(editing ? existing.quantity : 1);
    auto* enabled = new ToggleSwitch;
    enabled->setObjectName(QStringLiteral("taskEnabledCheck"));
    enabled->setStateTextVisible(true);
    enabled->setChecked(editing ? existing.enabled : true);
    auto* form = new QGridLayout;
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(4);
    const auto field = [&](const QString& title, QWidget* control, int row, int column, int span = 1) {
        form->addWidget(label(title, QStringLiteral("fieldLabel")), row, column, 1, span);
        form->addWidget(control, row + 1, column, 1, span);
    };
    field(QStringLiteral("任务名称"), name, 0, 0, 2);
    form->setRowMinimumHeight(2, 12);
    field(QStringLiteral("目标皮肤"), skin, 3, 0);
    field(QStringLiteral("成色"), condition, 3, 1);
    form->setRowMinimumHeight(5, 12);
    field(QStringLiteral("最低价格"), min, 6, 0);
    field(QStringLiteral("最高价格"), max, 6, 1);
    form->setRowMinimumHeight(8, 12);
    field(QStringLiteral("最大磨损"), wear, 9, 0);
    field(QStringLiteral("数量上限"), quantity, 9, 1);
    form->setColumnStretch(0, 1);
    form->setColumnStretch(1, 1);
    body->addLayout(form);
    body->addSpacing(16);
    auto* enableRow = new QHBoxLayout;
    enableRow->addWidget(label(QStringLiteral("启用此任务")));
    enableRow->addStretch();
    enableRow->addWidget(enabled);
    body->addLayout(enableRow);
    body->addSpacing(8);
    auto* validation = label(QString(), QStringLiteral("taskValidationLabel"));
    validation->setWordWrap(true);
    body->addWidget(validation);
    outer->addWidget(content);
    auto* commands = new QFrame;
    commands->setObjectName(QStringLiteral("dialogCommands"));
    auto* commandRow = new QHBoxLayout(commands);
    commandRow->setContentsMargins(24, 20, 24, 20);
    commandRow->setSpacing(8);
    commands->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* save = button(QStringLiteral("保存任务"), QStringLiteral("taskSaveButton"), ButtonKind::Accent);
    save->setDefault(true);
    auto* cancel = button(QStringLiteral("取消"), QStringLiteral("taskCancelButton"));
    commandRow->addWidget(save, 1);
    commandRow->addWidget(cancel, 1);
    outer->addWidget(commands);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this,
        [this, name, skin, min, max, wear, quantity, condition, enabled, validation, editing, existing] {
        if (name->text().trimmed().isEmpty()) { validation->setText(QStringLiteral("请输入任务名称。")); name->setFocus(); return; }
        if (skin->currentIndex() < 0) { validation->setText(QStringLiteral("请选择目标皮肤。")); return; }
        if (min->value() > max->value()) { validation->setText(QStringLiteral("最低价格需要小于或等于最高价格。")); return; }
        m_task.id = editing ? existing.id : QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_task.name = name->text().trimmed();
        m_task.skinId = skin->currentData().toString();
        m_task.minPrice = min->value();
        m_task.maxPrice = max->value();
        m_task.maxWear = wear->value();
        m_task.quantity = quantity->value();
        m_task.condition = condition->currentText();
        m_task.enabled = enabled->isChecked();
        m_task.status = m_task.enabled ? QStringLiteral("待启动") : QStringLiteral("演示已暂停");
        accept();
    });
}
} // namespace relink::ui
