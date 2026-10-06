# 集成契约入口

更新时间：2026-10-06（Asia/Shanghai）。本目录保留领域、运行时、存储和 UI 的设计契约。PR09 内存仓库、PR10 SQLite 仓库及 PR11 持久化工作区 UI 已落地；本轮实测见 [实施进度](../../09_m1_progress.md)，当前行为详见 [PR11 工作区记录](../workspace_pr11.md)。冻结 DDL/示例仍不是生产测试结果。

| 文件 | 责任 | 当前解释 |
|---|---|---|
| [ui_integration.md](ui_integration.md) | MainWindow/AppState 的 controller/projection、UI 命令与金额编辑 | 冻结设计输入；PR11 的实际接口/工作流另见实现记录 |
| [storage_contract.md](storage_contract.md) | IEventStore、事务、幂等、未决预留与恢复 | 保留初始目标；文首映射 PR10/PR11 实际接口 |
| [storage_schema.sql](storage_schema.sql) | 完整 SQLite 目标 DDL | 冻结规格，源码未直接执行该文件 |
| [storage_contract.sample.json](storage_contract.sample.json) | 结构化事务与幂等示例 | 规格示例，非实测结果 |

## 当前接入路径

~~~text
WorkspaceController 候选运行状态 / 纯规则解释
  → IEventStore 暂存事件 / attempt / 回执 / 预留
  → 显式 commit 成功
  → committedRuns / eventsForRun / snapshot / recoveryAudit
  → WorkspaceProjection
  → MainWindow 控件 / 记录表 / 统计 / 价格图
~~~

- Match 不等于允许外部动作；已保存方案仍为 enabled=false、activation_required=true，解释性评估不建立 attempt/reservation。
- 内置八步 fixture 才创建模拟派发/回执；所有观察和结果均为合成数据，不产生真实输入或购买。
- Pause/Stop 不抹除可能已发出的事实；Unknown 保留预留，重开不自动重发或恢复运行。
- SQLite 的查询投影只读取已提交快照；提交失败保留旧计数/记录/步骤并显示错误。
- 确认价必须关联已提交 Success 账本，观察价与模拟确认价分开显示；商品 ID 不充当卖单唯一键。
- ConfigV2 继续由 PR08 ProfileStore/QSaveFile 管理；保存、当前选择和运行快照独立，历史不套用后续 revision。
- 图像帧、OCR 全文和逐帧日志不进入账本；当前没有捕获/OCR，业务图像写入计数保持零。

## 工作区与集成检查

1. 默认工作区为 QStandardPaths::AppLocalDataLocation/business，包含 workspace.sqlite 和 profiles/；旧 schema v1 演示配置独立保存。
2. --workspace-dir 显式覆盖路径；--workspace-read-only 只读诊断已有工作区，不写方案、不迁移、不启动回放。
3. WorkspaceController 在其创建线程持有连接，关闭回滚未提交工作；真实写锁/COMMIT 错误通过投影显示，不切换到空库假装成功。
4. 离屏自测默认使用临时工作区，与正常用户数据隔离；指定测试目录时由调用方选择 --workspace-dir。
5. 分别运行文档校验与 CTest；从发布目录实际加载 Qt6Sql.dll、sqldrivers/qsqlite.dll，不用 SDK 存在 DLL 代替发布验收。
6. PR12 继续整套旧配置兼容、端到端、package-only PATH、异常关闭和发布回退测试；保留历史基线，不改写冻结矩阵的 planned_not_run。
