# 08 · 开发开工标准与首条纵切片

> 这是在业务说明之后给开发者看的工程入口。它回答：文档细到什么程度才足以支撑写代码、第一批代码写什么、什么还不能碰。当前结论：**M1 纯规则→回放→现有UI解释纵切片可以开工；M2捕获/OCR与M3/M4真实动作尚未达到生产Ready。**

## 1. 文档达到“可开工”的最低闭环

不是文档页数，而是以下信息同时闭合：

1. **对象闭合**：每个输入/输出有类型、ID、生命周期、必填/可空、单位、范围和未知语义。
2. **规则闭合**：输入校验、Review优先级、Match/NoMatch/NeedsReview、原因码和边界比较都能写成纯函数。
3. **状态闭合**：每个事件有前置状态、guard、效果、下一个状态、超时/乱序/迟到行为；Pause/Stop不靠文字解释。
4. **副作用闭合**：每个副作用有命令入口、事务边界、幂等键、失败回滚和重启恢复；图像默认零文件落盘规则可验证。
5. **交互闭合**：现有UI按钮对应应用命令和投影字段，不能在槽函数里藏业务循环。
6. **测试闭合**：每张开发票至少有一个固定输入、字段级预期、失败分支、命令计划和“不证明什么”。
7. **交付闭合**：实际build目录、Qt/MinGW工具链、QtSql/driver差距、固定EXE路径、回退副本和证据格式均写清楚。

缺一个闭环就只能称“设计说明”，不能称“可直接开工”。本轮的闭合证据在 `implementation/domain`、`implementation/runtime`、`implementation/integration` 和 `implementation/readiness`。

## 2. 当前等级与允许动作

| 范围 | 等级 | 现在能做什么 | 现在不能做什么 |
|---|---|---|---|
| M1 domain / replay / UI解释 | Ready to implement | 写C++值对象、纯匹配、FakeClock回放、InMemory账本、投影到现有页 | 不把演示成功当真实成交 |
| M1 v1导入 / profile | 可分票开工 | 只读预览、人工确认、v2新路径、QSaveFile | 不猜13列末尾语义/单位/动态延迟 |
| M2 捕获/OCR/零图片文件 | 契约Ready，运行实测未Ready | 写FakeCapture/worker协议测试和探针 | 不宣称识别率、FPS、后台捕获兼容或零IO已实测 |
| M3 双阶段策略 | 语义部分阻塞 | 用合成倒计时/额度回放验证状态机 | 不激活旧公式、真实限购、市场时段或卖单ID猜测 |
| M4 外部动作/发布 | Not ready | 先对自有测试窗口接FakeAction、准备发布脚本 | 不连接原样本、不执行真实点击/购买 |

## 3. 首条最小纵切片（PR01→PR05）

```text
固定合成Profile + 两条卖单观测
  → 领域校验/精确十进制
  → Match / NoMatch / NeedsReview + 原因列表
  → FakeClock / ReplayReducer
  → Pause/Resume/Stop/旧代次结果丢弃
  → event projection 到关注列表、任务页、记录页
```

明确不含：截图、OCR、真实捕获、系统时钟修改、临时图片、QtSql、真实输入。成功标准是“同一输入两次回放产生同一状态/事件序列，UI显示来源与原因”，不是“自动买到”。

开发顺序和每票输入/输出/回退见 [12张任务票](implementation/readiness/backlog.md)；详细Given/When/Then见 [测试矩阵](implementation/readiness/test_matrix.md)。

## 4. 五层契约的职责

| 层 | 机器契约 | 关键不变量 |
|---|---|---|
| Domain | `domain/core.schema.json`、`config-v2.schema.json`、`RULE_ENGINE.md` | 精确值、Review优先、Match不授权、未知单位不激活 |
| Runtime | `runtime/state.schema.json`、`observation.schema.json`、`worker.schema.json` | `cancel_epoch`、freshness、1处理+1待处理、Unknown保留预留 |
| Storage | `integration/storage_contract.md`、`storage_schema.sql` | ID幂等、同ID不同payload冲突、事务后才更新UI |
| UI | `integration/ui_integration.md` | 命令/投影分离、模式显式、旧UI入口保留 |
| Readiness | `readiness/backlog.json`、`test_matrix.json` | 票据可回退、结果可复验、计划不冒充通过 |

## 5. 开工前必须跑的文档检查

```powershell
python -I -X utf8 docs\business_rebuild\scripts\validate_contracts.py
python -I -X utf8 docs\business_rebuild\implementation\readiness\validate_readiness.py
python -I -X utf8 docs\business_rebuild\scripts\validate_docs.py
```

这些命令只检查 schema、fixture、引用、链接、计划和基线；输出中的`APPLICATION_TESTS_RUN=0`、`BUSINESS_ENGINE_TESTS_EXECUTED=0`、`LIVE_CAPTURE_TESTS_EXECUTED=0`必须保留，不能改成开发已完成。

## 6. 第一批真实代码的建议边界

```text
src/business/             value_types, models, validator, matcher
src/application/runtime/  reducer, coordinator, fake_clock, replay_source
src/ledger/                event_store, in_memory_event_store
src/application/ui/        projection, command_bindings
tests/business/            value/rule/import/replay/store tests
tests/runtime/             replay/lease/protocol contract tests
```

PR01先建立独立QtCore静态目标和测试入口，不改旧演示逻辑；PR02/03写领域纯函数；PR04写回放；PR05接UI投影。QtSql、OpenCV、OCR和WGC/DXGI都不应在首个PR引入。

## 7. 仍需产品/实验确认的决策

D01/D02/D03/D04/D05/D07/D09/D10/D11/D13仍然显式保留。它们不阻塞M1值对象、纯规则、回放和UI解释，但阻塞相应真实策略或生产捕获。`优化`入口D08保留，不在本轮发明功能。任何新证据都按“证据ID→决定→受影响票→回归用例”更新，不暗改规则。
