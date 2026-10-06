# M1 实施进度与双基线

更新时间：2026-10-06（Asia/Shanghai）

## 当前结论

M1 已经完成 PR01–PR09，其中 PR08 已完成 ProfileStore / ConfigV2 审定提交；当前交付继续保持“观察、回放、解释优先”的边界：不启动、不加载、不连接 BBZPS，不执行真实窗口捕获、OCR、鼠标键盘输入、市场动作或购买。PR06、PR07、PR08 与 PR09 均在内存或显式原子提交边界内运行，导入预览固定为 `committable=false`，账本事务由显式 `commit()` / `rollback()` 控制。

## 已完成 PR

| PR | 状态 | 实际结果 |
|---|---|---|
| PR01 | 完成 | QtCore 领域库、测试目标与 CMake 集成 |
| PR02 | 完成 | 精确十进制值、商品观察、任务规则与结构校验 |
| PR03 | 完成 | Matcher 决策、原因码、优先级与 `eligible_for_action=false` |
| PR04 | 完成 | ReplayReducer、RunCoordinator、取消/暂停/停止、Unknown 与回放语义 |
| PR05 | 完成 | Overview 接入 Replay 来源/状态/匹配说明/计数，以及开始、暂停、继续、停止控制 |
| PR06 | 完成 | schema v1 只读适配、稳定 ID、catalog 映射、运行参数校验、迁移诊断与原始字段留存 |
| PR07 | 完成 | 13 列导入预览接入只读 UI projection，Run 页入口、预览表格、诊断状态与禁用应用按钮 |
| PR08 | 完成 | ProfileStore、ConfigV2、ReviewChoices 审定与 QSaveFile 原子提交 |
| PR09 | 完成 | InMemoryEventStore、事件幂等、attempt/quota reservation、receipt、ledger、快照、恢复与显式事务回滚 |

## PR06 验收范围

- `previewV1()` 只生成 `LegacyImportPreview`，不触碰 `AppState`，不产生可执行配置，始终返回 `committable=false`。
- schema v1 只接受 `schema_version=1` 与 `demo=true`；未知字段脱敏保留，源内容保留 SHA-256、编码、原始 token 与稳定 ID 映射。
- 条件、数量、taxonomy、商品与价格都以候选值和 review 诊断输出；候选 `enabled=false`、`review_required=true`。
- `run_settings` 与旧解析器规则对齐：profile、F1–F12、延迟/触发范围、步长精度、限购范围、布尔类型和 `HH:mm` 均执行校验；非法值回退默认值并标记 `source=invalid` 与 `RUN_SETTING_INVALID`。
- `preview13Columns()` 支持 text/ini 容器、CRLF/LF、13 列原位保留、千分位十进制、价格反转、重复行、未知尾列和精度诊断；不保存临时图片或导入文件副本。

## PR09 验收范围

- 事件按 `event_id` 幂等；同一 `run_id + seq` 的不同事件返回冲突，JSON 对象键顺序不影响等价 payload 判定。
- reservation 显式绑定 quota scope、数量和 reservation ID；额度不足、scope 未解析、非法 reservation 均拒绝并保留结构化错误码。
- attempt 状态按 Prepared → Dispatching → Sent → AwaitingReceipt 单调推进；Unknown receipt 保留 reservation，Success 消耗 reservation，Failed 释放 reservation。
- receipt identity 与 payload 冲突、transaction 重复/冲突、attempt 多 transaction、dispatch/proof 回退均有可重复测试。
- `snapshot()` 输出成功、失败、Unknown、占用数量与待恢复数量；`recoverUnresolved(sessionId)` 按 run/session 映射筛选，不触发真实外部动作。
- 工作状态通过业务操作变更；显式 `store.commit()` 建立回滚检查点，`store.rollback()` 恢复最近一次检查点。

## 验证结果

```text
domain_tests                 PASS
business_tests               PASS
business_value_tests         PASS
business_rule_tests          PASS
business_migration_tests     PASS
profile_store_tests         PASS
runtime_tests                PASS
ledger_tests                 PASS
ui_offscreen                 PASS
CTest                        10/10 PASS
```

固定发布目录仍为：

```text
C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe
```

离屏自测要求：`QT_QPA_PLATFORM=offscreen --self-test`，不得启动可见窗口；必须观察 `game_connected=false`、`system_input_sent=false` 与 `imageFileWriteCount=0`。

## 双基线

- `artifacts/business_rebuild_docs/project_baseline.json`：原始项目基线，保留不覆盖。
- `artifacts/business_rebuild_docs/m1_pr01_pr04_baseline.json`：PR01–PR04 历史基线，保留用于审计。
- `artifacts/business_rebuild_docs/m1_pr05_baseline.json`：PR05 回放/UI 基线，保留用于审计。
- `artifacts/business_rebuild_docs/m1_pr06_pr09_baseline.json`：本轮 PR06/PR09 当前实现、测试与发布目录基线，由 `validate_docs.py` 优先读取。；PR08 当前基线见 `artifacts/business_rebuild_docs/m1_pr08_baseline.json`。

每一轮基线都只记录实际变更范围，不覆盖历史快照；文档验证同时检查项目基线漂移是否被最新 M1 基线明确覆盖。

## 下一阶段

| PR | 状态 | 下一步 |
|---|---|---|
| PR07 | 完成 | 将 13 列导入预览接入只读 UI projection；Run 页展示预览入口、行状态、诊断和只读约束 |
| PR10 | 计划中 | QtSql/SQLite 持久化、schema 迁移与崩溃恢复核验 |
| PR11 | 计划中 | 将回放、账本、方案和记录接入 Win11 UI 投影 |
| PR12 | 计划中 | 全量回归、离屏打包、解压目录复核与固定路径交付 |

下一阶段仍不得把预览候选直接提升为可执行配置；任何真实窗口捕获、OCR、输入、市场动作和购买都保持在后续独立审批边界之外。
