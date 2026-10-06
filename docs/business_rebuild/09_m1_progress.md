# M1 实施进度与分阶段基线

更新时间：2026-10-06（Asia/Shanghai）

## 当前结论

M1 已经完成 PR01–PR10，其中 PR08 完成 ProfileStore / ConfigV2 审定提交，PR10 完成 SQLite 结构化账本持久化；当前交付继续保持“观察、回放、解释优先”的边界：不启动、不加载、不连接 BBZPS，不执行真实窗口捕获、OCR、鼠标键盘输入、市场动作或购买。导入预览固定为 `committable=false`，账本事务由显式 `commit()` / `rollback()` 控制。

PR10 本轮通过 12/12 CTest、316 条 SQLite 断言、22 条发布存储自检，以及真实发布副本的回退/恢复复验。它增加持久化仓库、版本迁移、事务与受控进程退出恢复，不自动把后端接到前台；PR11 的方案/账本/记录 UI 工作流与 PR12 完整里程碑仍未完成。界面保留现有 Win11 白灰配色，没有重做布局。

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
| PR10 | 完成 | SqliteEventStore、v1→v2 迁移、提交快照、回执门槛、进程恢复、一致备份与包内 QSQLITE 验证 |

## PR10 本轮实施范围

详细接口、恢复规则与历史 DDL 差异见 [SqliteEventStore 实现记录](implementation/sqlite_store_pr10.md)。

- 引入独立 `relink_sqlite` 目标与 `Qt6::Sql`；`SqliteEventStore` 实现现有 `IEventStore`，配置仍由 PR08 的 QSaveFile 独立保存。
- 数据库采用 `user_version=2`；空库初始化、v1→v2 事务迁移、future/corrupt/unrecognized 拒绝，以及一致性备份均通过明确接口处理。
- 写操作在显式事务中暂存；SQLite 查询只返回已提交快照。业务/SQL 写失败回滚整笔事务并锁存 aborted，显式 `rollback()` 后才继续。
- 重开时，明确未派发的 Prepared 取消并释放预留；可能已派发的未决记录转 Unknown 并保留预留，写入恢复审计，不自动重发。
- `commitLedger()` 对 Unknown/ambiguous 回执返回 `RECEIPT_UNCONFIRMED`；终态与已确认回执不一致返回 `RECEIPT_OUTCOME_MISMATCH`。同一保护同步到内存仓库，避免将未确认回执直接记成成功/失败。
- 程序新增 `--storage-self-test`，仅使用临时数据库自测驱动、提交、重开恢复与备份；正常前台数据流未接 SQLite。
- 发布目录增加 `Qt6Sql.dll`、`sqldrivers/qsqlite.dll` 和 `qt.conf`，驱动目录与 `platforms` 同级；实际发布验证结果见下节。

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

### PR08 历史验证

以下为上一阶段已记录结果，不是 PR10 的新测试输出：

```text
domain_tests                 PASS
business_tests               PASS
business_value_tests         PASS
business_rule_tests          PASS
business_migration_tests     PASS
profile_store_tests         PASS
runtime_tests                PASS
ui_projection_tests          PASS
ledger_tests                 PASS
ui_offscreen                 PASS
CTest                        10/10 PASS
```

### PR10 本轮验证

离屏人工复核发现并修复了历史主窗口乱码：330 处字符串、3 条注释恢复为正常中文，右上角状态 HTML 一并修正；字符串/注释之外的代码以及全部 ASCII 标识保持不变。新增 `UI_NAVIGATION_TEXT_UTF8`、`UI_ACTION_TEXT_UTF8`、`UI_NO_MOJIBAKE_LABELS` 三项运行时回归，均通过。Win11 白灰布局和功能入口不变；最终关注页、运行页截图已重新查看。

文案修复后的最终完整构建与打包命令为 `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test -Package`，记录于 `logs/final_build.log`；结果仍为 12/12、22 条存储自检通过。发布和旧版恢复复验记录已更新到最终程序版本。

当前状态：**本轮实测通过**。构建目录为 `build_relocated`；完整证据位于 `artifacts/m1_pr10_transaction/VERIFICATION.txt`、`package_commands.json` 和 `logs/`。其中四个子进程使用受控退出验证持久化窗口，不等同于真实断电试验。

| 检查 | 当前结果 | 复现入口 |
|---|---|---|
| 全量应用测试 | 12/12 通过，exit=0 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test`；`logs/modified_test.log` |
| SQLite 持久化与恢复测试 | 316 条断言，failures=0，exit=0 | `logs/sqlite_ledger_tests.log`；真实 DB 与 4 个子进程 |
| 打包与发布驱动自测 | PACKAGE=PASS；22 条存储自检，exit=0 | `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Package`；`logs/package.log` |
| 发布程序离屏 UI | UI_SELF_TEST=PASS，exit=0 | `package_commands.json` 的 MODIFIED_UI；offscreen=true |
| 旧发布副本回退与复验 | ROLLBACK、RESTORED 均 exit=0 | `logs/package_verification.log`；恢复旧文件哈希，保留新数据库 |
| 文档/契约/新基线 | 结果由最终验证工件单独记录 | 三个文档校验入口；它们不执行应用测试 |

本轮关键原始输出：

```text
100% tests passed out of 12
SQLITE_LEDGER_TESTS=PASS; assertions=316; failures=0; real_database=true; crash_processes=4; external_actions=0
CRASH_HOT_JOURNAL_BYTES=62976; header=d9d505f920a163d7
STORAGE_SELF_TEST=PASS; driver=QSQLITE; temporary_data=true; system_input_sent=false; assertions=22
PR10_PACKAGE_VERIFICATION=PASS
```

固定发布目录仍为：

```text
C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe
```

离屏自测要求：`QT_QPA_PLATFORM=offscreen --self-test`，不得启动可见窗口；必须观察 `game_connected=false`、`system_input_sent=false` 与 `imageFileWriteCount=0`。

## 分阶段基线

- `artifacts/business_rebuild_docs/project_baseline.json`：原始项目基线，保留不覆盖。
- `artifacts/business_rebuild_docs/m1_pr01_pr04_baseline.json`：PR01–PR04 历史基线，保留用于审计。
- `artifacts/business_rebuild_docs/m1_pr05_baseline.json`：PR05 回放/UI 基线，保留用于审计。
- `artifacts/business_rebuild_docs/m1_pr06_pr09_baseline.json`：PR06/PR09 历史实现、测试与发布目录基线，保留用于审计。
- `artifacts/business_rebuild_docs/m1_pr08_baseline.json`：PR08 历史实现基线，保留用于审计。
- `artifacts/business_rebuild_docs/m1_pr10_baseline.json`：本轮 PR10 源码、测试、当前状态文档与发布目录基线；不覆盖上述历史文件。最终校验以本轮验证工件为准。

每一轮基线都只记录实际变更范围，不覆盖历史快照；文档验证同时检查项目基线漂移是否被最新 M1 基线明确覆盖。

## 下一阶段

| PR | 状态 | 下一步 |
|---|---|---|
| PR10 | 完成 | 后端持久化与恢复已验证；不重复实施，不冒充已完成 PR11 |
| PR11 | 计划中 | 将回放、账本、方案和记录接入 Win11 UI 投影 |
| PR12 | 计划中 | 全量回归、离屏打包、解压目录复核与固定路径交付 |

下一阶段仍不得把预览候选直接提升为可执行配置；任何真实窗口捕获、OCR、输入、市场动作和购买都保持在后续独立审批边界之外。
