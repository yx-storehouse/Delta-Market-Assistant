# 下一轮开发交接：PR11 方案与账本 UI 投影

更新时间：2026-10-06（Asia/Shanghai）。PR01–PR10 已完成；本轮实测为 12/12 CTest、316 条 SQLite 断言和 22 条发布存储自检，真实发布回退与恢复程序的离屏复验均通过。实现与证据见 [SQLite 仓库](implementation/sqlite_store_pr10.md) 和 [实施进度](09_m1_progress.md)。下列提示词继续 PR11，不把后端完成当作常规 UI 已接入。

## 可直接复用的开发提示词

~~~text
工作目录：C:\Users\Administrator\Desktop\price。
项目名称：Delta Market Assistant（三角洲市场助手）；当前构建目标和 EXE 文件名仍为 RelinkStudio。

先读取 AGENTS.md、SESSION_START.md、docs/business_rebuild/README.md、
09_m1_progress.md、implementation/README.md、implementation/profile_store_pr08.md、
implementation/sqlite_store_pr10.md、implementation/integration/ui_integration.md，
以及 implementation/readiness/backlog.md 中 PR11/PR12 的冻结验收要求。
先检查 git status -sb、当前源码与最近验证日志，不重复实施 PR01–PR10。

前置检查：
1. PR10 的 SQLite 测试、全量 CTest、发布 QSQLITE 和离屏自测必须有真实结果。
   若当前源码与最近验收版本不同，先回归核对，不用历史 PASS 代替本轮测试。
2. 读取实际 IEventStore、SqliteEventStore、ProfileStore 接口，保持冻结规格与实现差异可追踪。
3. PR08 ConfigV2 仍由 QSaveFile 独立管理，不因 PR10 有 SQLite 而迁移配置格式。

本轮目标：完成 PR11 方案、审定状态、回放、已提交账本和记录的 UI 投影。
1. 保留现有 Win11 / 微软应用商店浅色白灰设计，不使用蓝色，不重做布局。
   保留运行/任务页、顶部居中搜索、关注列表、右侧条件编辑和底部价格图。
2. 在 controller/service 层连接存储；界面不直接执行 SQL 或持有数据库事务。
   确定可写数据库路径、连接所属线程、生命周期和关闭时 rollback 行为。
3. 方案列表展示 profile ID/revision、来源与审定状态；明确区分预览、已保存与当前选择。
   committable=false 的 LegacyImportPreview 始终是只读输入；仅完整 ReviewChoices
   才能进入 ProfileStore 审定保存。保存不会自动启用规则或发送外部动作。
4. 成功数、Unknown、预留和记录从已提交快照产生；commit 失败保留旧快照并显示错误。
   不把 recordDispatch、applyReceipt 或 commitLedger 的暂存成功显示为已入账。
5. PR10 重开会取消明确未派发的 Prepared，并将可能已派发的未决事实转 Unknown；
   UI 显示恢复结果和仍占用的预留，绝不自动重发动作或自动继续运行。
6. 同一商品的不同卖单不互相覆盖；观测价与确认成交价明确分开。
   缺字段、过期观察、来源、模式和时钟信息保持可见；Unknown 不是失败也不是成功。
7. 完善停止/切换模式/脏配置交互、存储错误提示、只读恢复记录与 CSV 公式转义。
   回放快速推进合并刷新；不写逐帧图像或 OCR 全文日志。
8. 新增 projection/controller 与离屏 UI 回归，覆盖提交失败、重启恢复、切换方案、
   只读预览、显示计数一致和导出转义。若新增 UI 行为未完成，不宣称 PR11 完成。

执行边界：
- 不运行、加载或打包 BBZPS 中的 EXE、DLL、脚本和插件。
- Replay/Fake 保持独立；不接入真实窗口捕获、OCR、键鼠输入、购买或交易。
- 后续采集仍遵守 ADR10/E12：步骤触发、像素只走内存、默认零截图文件写入。
- 配置和必要账本正常落盘，与“图像不落盘”要求分开；不为减少 IO 跳过关键事务。
- 优化入口保持待定义，不替用户补造产品语义。
- 不覆盖历史 baseline、schema、fixture、backlog 的冻结结果。
- 不自动打开前台窗口。

验收与交付：
执行 build.ps1 -Test、build.ps1 -Package，并从发布目录离屏验证 QtSql/QSQLITE、
UI_SELF_TEST、game_connected=false、system_input_sent=false 与图像零写入断言。
保留 DLL、platforms、sqldrivers，更新已经解压的固定程序路径：
C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe。
提供实际命令、结果、退出码、源码/产物哈希、回退验证与未完成项。
文档工具结果与应用 CTest 分开报告；不声称 PR12 或 M2/M3/M4 已完成。
~~~

## PR12 留待完整回归

单次开发都会构建并更新固定发布目录，但这不等于 PR12 的完整里程碑已完成。PR12 仍需在 PR11 后核验端到端投影、旧配置兼容、包内依赖、原始样本排除、恢复/回退与整套验收记录。
