# 集成契约入口

更新时间：2026-10-06（Asia/Shanghai）。本目录保留领域、运行时、存储和现有 UI 的设计契约；PR09 内存仓库与 PR10 SQLite 仓库均已完成并验证，PR11 常规 UI 接入仍待开发。实际实现和验证记录看 [PR10 记录](../sqlite_store_pr10.md) 与 [实施进度](../../09_m1_progress.md)。本目录的冻结 DDL/示例仍不是生产测试结果。

| 文件 | 责任 | 当前解释 |
|---|---|---|
| [ui_integration.md](ui_integration.md) | MainWindow/AppState 的 controller/projection、UI 命令与金额编辑 | 设计输入；PR05/PR07 已接入部分回放/预览，PR11 继续方案与账本 |
| [storage_contract.md](storage_contract.md) | IEventStore、事务、幂等、未决预留与恢复 | 保留初始目标；PR10 差异在文首列明 |
| [storage_schema.sql](storage_schema.sql) | 完整 SQLite 目标 DDL | 冻结规格，PR10 未直接执行该文件 |
| [storage_contract.sample.json](storage_contract.sample.json) | 结构化事务与幂等示例 | 规格示例，非实测结果 |

## 接入顺序

~~~text
domain Decision
  → runtime Run / Attempt reducer
  → IEventStore 暂存事实 / 回执 / 预留
  → 显式 commit 成功
  → 已提交 LedgerSnapshot
  → UI projection
~~~

- Match 不等于允许外部动作；当前 Replay/Fake 仍不产生真实输入或购买。
- Pause/Stop 不抹除已发出的事实；迟到回执可以入账，但不恢复运行。
- SQLite 的查询投影只读取已提交快照，不把当前事务里的暂存成功显示到 UI。
- 图像帧、OCR 全文和逐帧日志不进入账本；正常业务不落截图。
- ConfigV2 继续由 PR08 ProfileStore/QSaveFile 管理；PR10 不自动切换 AppState。
- PR10 后端可用不等于 PR11 已接好前台，不能据此宣称方案列表、记录页或恢复提示已经完成。

## 集成前检查

1. 核对 src/ledger/store_types.h 与 sqlite_event_store.h 的实际类型和返回值，不照抄历史伪代码。
2. 分别运行文档校验与 CTest；记录源码 revision、实际 buildDir、命令、退出码和产物哈希。
3. 校验 Qt6::Sql 链接与发布目录 Qt6Sql.dll、sqldrivers/qsqlite.dll；SQL driver 与 platforms 是同级目录。
4. 从发布目录实际加载 QSQLITE 并测试，不能用 SDK 中存在 DLL 代替发布验证。
5. 实施 PR11 时再选择 app data 路径与 controller 生命周期；不得把测试临时数据库当用户数据库。
