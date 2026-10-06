# 下一轮开发交接：PR12B 服务边界与历史读取收敛

更新时间：2026-10-06（Asia/Shanghai）。PR11 已接通方案审定、持久化回放和历史记录；PR12A 在不扩展业务语义的前提下拆分页面、任务编辑器和离屏自检入口。结构说明见 [PR12A UI 模块化](implementation/ui_modularization_pr12a.md)，本轮实际验收以 [实施进度](09_m1_progress.md) 与对应事务日志为准。

**下一步是 PR12B，不是直接接 OCR。** PR12A / PR12B 是原 PR12 验收之前的小步结构整理，不取代、不改写 [冻结 PR12 backlog](implementation/readiness/backlog.md) 的端到端验收要求，也不把局部重构称为 M1 全部完成。

## 1. 直接复用：PR12B 开发提示词

~~~text
工作目录：C:\Users\Administrator\Desktop\price。
项目名称：Delta Market Assistant（三角洲市场助手）；构建目标/EXE 仍为 RelinkStudio。

先读取 AGENTS.md、SESSION_START.md、docs/business_rebuild/09_m1_progress.md、
implementation/ui_modularization_pr12a.md、implementation/workspace_pr11.md、
implementation/sqlite_store_pr10.md、implementation/profile_store_pr08.md 和冻结 PR12 backlog。
检查 git status -sb、源码、已发布程序和最新验证日志；保留用户已有未提交修改。
不要重新实施 PR01–PR11 或重做 PR12A 已拆出的页面与 UI 自检入口。

本轮目标：PR12B 服务边界与历史读取收敛，保持现有 UI/CLI/业务结果。
1. 建立独立源码/发布 baseline，先重跑当前行为。阅读 WorkspaceController::Impl、
   refresh/commit 路径、SqliteEventStore 查询与现有 workspace_tests。
   以实际调用链列出责任，不按文件行数机械拆 cpp。
2. 从 WorkspaceController 逐项提取方案目录/审定保存、回放会话与内建场景、
   历史读取、已提交投影、CSV 导出；controller 留协调和稳定的 UI 接口。
   每次只移动一组清晰责任，保持 QObject 生命周期和 SQLite 连接线程归属。
   继续使用 Qt Widgets 模块化单体，不引入微服务、通用插件框架或 DI 容器。
3. 复用现有 IEventStore 写契约，并设计窄的历史查询边界；不要假定 IEventStore
   已经包含 committedRuns/eventsForRun/recoveryAudit 等接口。
   保持提交成功后才公布投影，失败不推进步号、不显示成功、不丢失旧计数。
4. 历史列表先读摘要，选中 run 才加载详细事件/账本/审计，明确分页或增量策略。
   用多 run/多 event 的实际临时数据库验证读取范围，并验证切换/刷新/恢复语义。
   不以“用了异步”替代查询边界，也不跨线程共享当前 SQLite 连接。
   数据量/耗时阈值先实测再记录，不宣称没有基准的性能提升。
5. 把旧 AppState/Demo 配置入口限制在明确适配边界；新功能使用业务 DTO/投影。
   内置八步 fixture 从通用协调中独立，但保持模式、事件、身份、顺序和演示结果。
   已保存方案仍只作首条禁用规则的解释性合成评估；不变成全规则真实执行。
6. 对 profile revision/完整运行快照、dirty/active/read-only 保护、观察价/确认价、
   未提交失败、恢复 Unknown/保留预留、CSV 转义/受保护路径分别补回归。
   页面继续只消费投影/调用现有控制入口，不把 SQL 或文件写入搬进 UI。
7. 每个子步骤均全量构建/CTest、固定目录打包、package-only PATH 离屏验收。
   新增测试数量按实际日志记录；旧 PASS 和文档校验不替代本轮应用测试。
   保持 --self-test、--snapshot-dir、--storage-self-test 及输出字段兼容。
8. 发布前保留旧目录/哈希，在另一副本验证恢复并重跑旧程序自检；
   不回退固定交付目录，不删除或降级新方案/账本，固定目录保留本轮修改。
   更新文档和可复用下一阶段提示词，明确 PR12 全面验收仍未由重构自动完成。

全程保持：
- Win11/微软商店浅色白灰、无蓝色；保持布局、objectName、原参数与功能入口。
- 保存不等于选择、更不等于启用：enabled=false、activation_required=true。
- SQLite schema v2/历史运行快照保持兼容，ProfileStore/QSaveFile 独立于事件账本。
- 仅 Demo/合成 Replay，不运行/加载/打包 BBZPS，不接真实捕获、OCR、键鼠或交易。
- 未来图像链路为步骤触发、内存像素、默认无截图/临时图像文件写入；必要账本正常持久化。
- 不定义“优化”入口，不自动打开可见窗口或占用用户前台。

交付已经解压的程序：
C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe
保留 DLL、platforms、sqldrivers 和 qt.conf。
记录基线/修改/回退的实际命令、输入、字面输出、退出码、哈希及恢复后行为。
本轮 MODIFIED_FILE、DIFF_FILE、VERIFICATION.txt、ROLLBACK.sh 均须可重新读取。
~~~

## 2. PR12B 的完成边界

| 目标 | 应有证据 | 不以什么替代 |
|---|---|---|
| 控制器变薄 | 服务接口、持有关系、职责及单元测试 | 仅拆文件但继续共享大状态 |
| 查询增长受控 | 多历史运行的读取计数/范围与正确性回归 | 只改成异步或凭包体大小判断性能 |
| 行为一致 | 原 UI/工作区/存储测试和新增服务测试 | 旧 PR11/PR12A 成绩 |
| 历史与恢复兼容 | run 快照/revision、Unknown 和 reservation 检查 | 从当前方案重新构造旧运行 |
| 可回退 | 独立发布副本恢复及数据库保留结果 | 仅生成回退脚本文本 |

## 3. 服务整理之后：原 PR12 完整交付验收

以下保留原 PR12 验收提示词。完成 PR12B 后再执行；其中历史测试数量须以当时源码与真实日志为准。

~~~text
工作目录：C:\Users\Administrator\Desktop\price。
项目名称：Delta Market Assistant（三角洲市场助手）；构建目标和 EXE 文件名仍为 RelinkStudio。

先读取 AGENTS.md、SESSION_START.md、docs/business_rebuild/README.md、
09_m1_progress.md、implementation/README.md、implementation/profile_store_pr08.md、
implementation/sqlite_store_pr10.md、implementation/workspace_pr11.md，
以及 implementation/readiness/backlog.md 中 PR12 的冻结验收要求。
先检查 git status -sb、当前源码、已发布程序与最新验证日志，保留用户现有未提交改动。

本轮目标：完成 PR12 端到端、兼容性、发布依赖、数据隔离和真实副本回退回归。
1. 读取 tests/verify_delivery.py、build.ps1、CMakeLists.txt 与 PR11 自测入口。
   参数化实际 buildDir、应用版本、可执行文件路径与断言数量；不要硬编码历史 PR10 数量。
   每项必须对照本轮日志，不以旧 PASS 代替新源码测试。
2. 全量构建与 CTest；逐项复验导入预览 → 必要审定 → 原子保存 → 显式选择 →
   回放 → 已提交记录 → 关闭/重开 → 历史选择 → CSV 导出。
   保存后仍 enabled=false、activation_required=true；已保存方案不创建 attempt/reservation。
   显式选择内置八步 fixture 才生成模拟派发/回执，所有数据标明合成来源。
3. 补齐并复验 schema v1 原配置兼容、审定 revision、完整运行快照冻结，
   同商品多卖单、观察价/确认价、missing/stale/clock/source 可见和 CSV 公式转义。
   收藏成功计数只来自真实存在的已提交收藏事实；fixture 没有该事实时保持零。
4. 覆盖 dirty 审定取消/保留、旧演示配置保存/丢弃/取消、停止后切模式、
   暂停禁切方案、关闭时停止失败保留窗口，以及保存/COMMIT 失败不显示成功。
   真正运行控件与临时数据库，不只测字符串或用假失败返回替代关键事务故障。
5. 覆盖默认 AppLocalDataLocation/business、--workspace-dir 与 --workspace-read-only，
   确保离屏验收默认临时工作区，绝不污染真实用户配置/方案/账本。
   复验第二写实例冲突、只读不写入、损坏/未来 schema 诊断和错误恢复。
6. 真实重开与受控子进程退出后，Prepared 明确未发者取消，可能已发者 Unknown/保留预留；
   不自动重发、不自动继续回放。历史 run 保持当时完整 profile JSON/revision。
   不把受控进程退出称为物理断电试验。
7. 打包后以 package-only PATH 和显式离屏平台运行原 UI、workspace UI 与 storage 自测；
   核验 Qt6Sql.dll、sqldrivers/qsqlite.dll、platforms、qt.conf 和资源清单。
   核验发布目录不包含原始样本、SDK、缓存、用户数据库或私钥。
8. 发布前保存旧发布目录与哈希，独立副本执行回退并重新运行旧程序离屏自检；
   保留新工作区数据库/方案，不在固定发布目录执行回退。
   核验最终固定 EXE 和必要 DLL 均为本轮构建，四项事务工件可重开且记录完整。
9. 对照 PR12 backlog 和现有验收规格逐项记录证据、缺口与结论；
   不将 schema/fixture 文档校验或局部自测替代端到端通过。
   冻结 baseline/schema/fixture/backlog 不覆盖；本轮建立独立 PR12 baseline。

继续保持：
- Win11 / 微软应用商店浅色白灰，不使用蓝色，不重做现有布局与功能入口。
- WorkspaceController 管理事务与投影，MainWindow 不直接 SQL；配置仍独立 QSaveFile。
- 不运行、加载或打包 BBZPS；不接入真实捕获、OCR、键鼠输入、购买或交易。
- 后续图像路径仍为步骤触发、像素只走内存、默认零截图文件写入；
  配置和必要账本正常持久化，不为减少 IO 跳过关键事务。
- 优化入口保持待定义；不自动打开前台窗口或占用用户前台。

验收与交付：
执行 build.ps1 -Test、build.ps1 -Package，并实际从发布目录离屏测试。
核验 UI_SELF_TEST、WORKSPACE_UI_SELF_TEST、STORAGE_SELF_TEST、game_connected=false、
system_input_sent=false 和 imageFileWriteCount=0；记录本轮实际数量而非预设通过数量。
更新已经解压的固定程序路径并保留 DLL、platforms 与 sqldrivers：
C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe。
交付实际命令、输入、字面输出、退出码、源码/产物哈希、回退后行为和未完成项。
文档校验与应用 CTest 分开报告，更新当前入口及下一轮提示词。
只在全部 PR12 条目有对应证据后称 PR12 完成；M2/M3/M4 仍是独立阶段。
~~~

## 4. PR12 完成后再进入 M2

PR12 的交付回归完成后，再单独规划 M2 的步骤触发、内存捕获与识别适配器。M2 应保留 fake/replay、来源与时间信息、零默认图像文件写入和可取消生命周期；不能由已有模拟回放推断真实 OCR 已接通。旧延迟/限购的未决语义和“优化”入口仍按决策表处理。
