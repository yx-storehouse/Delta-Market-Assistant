# 下一轮开发交接：PR12 端到端回归与固定目录交付

更新时间：2026-10-06（Asia/Shanghai）。PR11 已把方案审定、持久化回放、已提交账本、恢复与历史记录接入 Win11 UI。实现及本轮实际验证见 [PR11 工作区](implementation/workspace_pr11.md) 和 [实施进度](09_m1_progress.md)。下一轮完成 PR12 整套回归与交付验收，不重复实施 PR01–PR11，不把合成回放说成真实市场输入。

## 可直接复用的开发提示词

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

## PR12 完成后的选择

PR12 的交付回归完成后，再单独规划 M2 的步骤触发、内存捕获与识别适配器。M2 应保留 fake/replay、来源与时间信息、零默认图像文件写入和可取消生命周期；不能由已有模拟回放推断真实 OCR 已接通。旧延迟/限购的未决语义和“优化”入口仍按决策表处理。
