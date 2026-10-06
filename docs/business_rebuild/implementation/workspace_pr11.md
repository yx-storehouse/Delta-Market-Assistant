# PR11 · 方案审定、持久化回放与账本 UI

更新时间：2026-10-06（Asia/Shanghai）。
状态：PR11 已完成；本轮 13/13 CTest、400 条工作区服务断言、58 条工作区 UI 断言、22 条发布存储自检及独立副本回退复验通过。精确日志与退出码见 [实施进度](../09_m1_progress.md)，不沿用 PR10 的通过数。

本票把 PR08 方案文件和 PR10 SQLite 账本接到现有 Win11 白灰桌面界面。它完成的是**方案审定保存、离线合成回放、已提交事实展示与历史记录**，不是捕获/OCR/真实交易接入；用户原运行参数仍只随配置保存。

## 1. 用户可以从哪里使用

| 入口 | 操作 | 本票语义 |
|---|---|---|
| 运行页的方案选择 | 选择已审定方案或显式选择内置合成回放 | 保存不等于选择，选择不等于启用规则 |
| 原导入预览 | 查看原始列、来源、诊断，进入单独审定窗口 | 原“应用”仍禁用；LegacyImportPreview 保持只读 |
| 审定窗口 | 填方案名/精度并完成必要确认后保存 | 原子写入 ConfigV2，保留 source hash 与 revision |
| 回放控制 | 开始、暂停、继续、单步、快速推进、停止 | 仅 Replay 执行内置合成流程，不产生外部动作 |
| 卖单/记录/统计 | 查看来源、时钟、缺失、过期、确认结果与错误 | 展示已提交事件/账本，不展示未提交的成功 |
| 历史运行 | 选择某次 run | 使用该 run 的方案快照与版本，不套当前方案 |
| CSV 导出 | 导出当前选中历史/运行的记录 | 标注 mode、profile ID/revision，防公式注入 |

界面保持 #F3F3F3 背景、#F9F9F9 内容层、白卡片、灰边框、黑灰文字与近黑强调，不使用蓝色。不自动打开用户前台窗口；程序固定交付路径仍为：

~~~text
C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe
~~~

## 2. 从哪些文件继续开发

| 文件 | 责任 |
|---|---|
| [workspace_controller.h](../../../src/application/workspace/workspace_controller.h) | WorkspaceController、ProfileRow/RunRow/ListingRow/RecordRow、WorkspaceProjection |
| [workspace_controller.cpp](../../../src/application/workspace/workspace_controller.cpp) | 方案保存、切换保护、运行控制、事务提交和投影 |
| [store_types.h](../../../src/ledger/store_types.h)、[sqlite_event_store.h](../../../src/ledger/sqlite_event_store.h) | 存储类型及已提交事件/run/恢复审计查询 |
| [mainwindow.cpp](../../../src/mainwindow.cpp) | 将 widgets 绑定到 controller/projection，不在 UI 内写 SQL |
| [main.cpp](../../../src/main.cpp) | 生命周期、默认数据目录、工作区选项与离屏隔离 |
| [workspace_tests.cpp](../../../tests/application/workspace_tests.cpp) | 真实临时 DB、审定、切换、提交失败、重启、历史和 CSV 测试 |
| [workspace_ui_self_test.cpp](../../../src/application/workspace/workspace_ui_self_test.cpp) | 离屏真实控件操作、错误显示、审定保存和恢复投影 |

PR10 原始实现记录见 [sqlite_store_pr10.md](sqlite_store_pr10.md)。本票不修改数据库 user_version，沿用 v2；新增只读查询接口供 controller 获取已提交 runs、events 和 recovery_audit。冻结目标 schema/backlog 保留。

## 3. 工作区路径与生命周期

普通启动采用 QStandardPaths::AppLocalDataLocation 下的 business 子目录：

~~~text
business/
  workspace.sqlite
  profiles/
    profile-<24位十六进制ID>.json
~~~

工作区与旧 schema v1 演示配置分别保存。C++ 对象在创建线程中持有 SQLite 连接，关闭会回滚未提交工作。多个写实例继续受 PR10 单写者锁约束，界面显示打开错误而不是静默新建另一个空账本。

可通过命令行显式指定：

~~~powershell
& 'C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe' --workspace-dir 'D:\DeltaMarketData'
~~~

上述命令供用户自行启动，自动验收不运行前台命令。诊断已有工作区时可同时使用 --workspace-read-only；只读模式不迁移或写入，不运行回放，不保存方案。

离屏自测默认使用 QTemporaryDir，既不连接正常用户账本，也不覆盖正常用户方案。明确传入 --workspace-dir 才使用调用者指定的测试目录。测试截图是显式离屏 UI 验收产物，不是业务采集路径落图。

## 4. 方案审定与切换保护

输入继续为 LegacyImportPreview 和 ReviewChoices。审定确认包含商品、价格区间、taxonomy、未知列保留和数量不自动绑定；名称及 quantum 需有效。quantum 必须是正的精确十进制，不接受零、负数或指数形式，保存与重新读取均检查相同格式。source.sha256 必须为 64 位小写十六进制，来源 format/encoding 必须在已支持范围；profile ID 必须与去空白名称及源 hash 的确定性计算一致。

保存过程：

~~~text
只读预览
  → 校验 ReviewChoices / 精确 quantum
  → ProfileStore materialize / 旧目标验证
  → QSaveFile 原子提交
  → 重新读取方案列表
  → 显示已保存版本；保持用户原选择
~~~

- 同名、同 source hash 的稳定 ID 再次保存递增 revision；源 hash 不丢失。
- 所有导入规则保持 enabled=false、activation_required=true，审定不授权真实运行。
- “保存成功”“当前选择”“当前运行快照”是三个独立状态；不将保存成功自动变成启用。
- 保存前先验证已存在方案目录，不让坏 catalogue 导致“文件已覆盖、UI 却报失败”；保存失败保留原文件、当前选择与已显示业务数据，错误通过 lastError/errorOccurred 展示。
- 未保存审定内容标记 dirty；切换方案、运行模式或开始前要求显式保存/丢弃。审定对话框的取消、Escape 和标题栏关闭采用同一条保留/丢弃确认路径。
- 运行中和暂停中均禁止替换方案、切模式、切历史或保存新方案；用户先停止运行。
- 启动时重新检查已选方案，不把被外部改写的非法 ConfigV2 当作有效配置。
- 历史 run 的 Start 事件保存完整 profile_snapshot；后续方案升到 revision 2，原 run 仍显示 revision 1。

已保存方案当前只把首条禁用规则代入合成输入做解释性评估，不创建 attempt 或 reservation；它不是完整方案的全规则批处理。内置合成 fixture 才能生成模拟派发/回执，用于校验账本展示。两者均没有真实市场输入。

旧 schema v1 演示配置另有 dirty 状态，不等于审定草稿。切换工作区模式/方案时提示“返回编辑”或“保留草稿并继续”，继续不会把演示参数应用到 ConfigV2。关闭主窗口时提供保存、放弃或返回；保存失败或停止活动运行失败时窗口保留，放弃退出不会被 aboutToQuit 自动保存覆盖。审定窗口仍打开时先处理审定窗口。

## 5. 运行控制与八步合成 fixture

WorkspaceController 提供 open/close/refresh、saveReviewedPreview、selectProfile/selectBuiltInFixture/selectRun、setMode/setReviewDirty/discardReviewDraft、start/pause/resume/step/advance/stop 和 exportRecordsCsv。

Replay 模式支持下列内置人工数据；Demo 沿用原演示引擎，不能通过 workspace.start() 冒充持久化回放。

| 步骤 | 合成输入 | 已提交后的含义 |
|---|---|---|
| 1 | 同一商品的卖单 A 观察 | 显示 Match 与观察价 |
| 2 | A 的模拟派发事实 | 建立预留，成功仍为 0 |
| 3 | A 的确认成功回执 | 终态入账，显示确认价，与观察价分开 |
| 4 | 同商品卖单 B 的观察与派发 | B 为独立卖单，不覆盖 A |
| 5 | 卖单 C 的过期观察 | stale=true，保留时钟与原因 |
| 6 | 卖单 D 缺价格 | 显示 missingFields，不当作零价格 |
| 7 | 卖单 E 超出价格区间 | 显示 NoMatch 与原因 |
| 8 | 卖单 F 的模拟失败回执 | 显示确认失败；B 留 Unknown/预留，结束合成回放 |

完整无中断 fixture 的预期分类为成功 1、失败 1、Unknown 1、占用预留 1；收藏成功为 0，不捏造没有发生的收藏事实。每条 ListingRow 保存 listing ID + observation ID，不以 product ID 作为覆盖键。

暂停保留运行但阻止模式/方案变更；单步可以在 Paused 状态推进一条回放而保持 Paused。暂停/停止将可能已派发的事实保留为 Unknown；继续时不将 Unknown 自动改为成功，也不自动重发。真实重开后只能查看历史，不把恢复集重新激活为运行。

advance() 批量推进时合并 changed() 通知，避免每个内部步骤都触发整个 UI 重绘；每笔关键状态仍分别事务提交，不以合并 UI 刷新替代账本持久化。

## 6. 已提交投影与错误显示

每个回放步骤先在候选运行状态中计算事件/attempt/回执，随后事务提交；只有 commit 成功才替换运行状态与投影。

~~~text
候选运行状态
  → appendEvent / reserveAttempt / recordDispatch / applyReceipt / commitLedger
  → store.commit 成功
  → committedRuns / eventsForRun / snapshot / recoveryAudit
  → WorkspaceProjection
  → UI labels / tables / chart / counters
~~~

- 成功、失败、Unknown 和 reservation 从已提交账本派生。
- Match、NoMatch、NeedsReview 与模拟派发从已提交事件派生。
- 确认价需要事件中的 attempt_id/transaction_id 与 Success 账本匹配，不能由任意名为 ReceiptConfirmed 的事件伪造。
- 真实 DB_BUSY、COMMIT 失败等错误保留旧计数、旧记录和当前步号；调用方显示错误，不提前显示成功或跳过步骤。
- 存储回滚完成且故障解除后，用户可以重新执行同一步；回放身份与账本约束避免重复成功记录。
- 切换 Demo/Replay 时清理当前卖单/记录/计数/选中 run，不删除历史 runs。
- 选择历史记录使用该 run 的 mode、profile ID/revision 与快照，不以当前模式或当前方案重写来源。

未知与成功/失败明确区分；缺失字段、过期时间、时钟域、源模式和观察价/确认价分别显示。M2 后续引入真实观察时，仍必须保留这些来源与新鲜度字段。

## 7. CSV 边界

导出当前已显示的已提交记录，使用 QSaveFile 原子保存。列包含运行/事件身份、mode、源 profile ID/revision、来源、时钟、单调时间、listing/observation 身份、观察价、模拟确认价和原因。

csvCell() 对前导 =、+、-、@，前导空白后的公式符号，以及前导 tab/CR/LF 加 apostrophe；随后执行 CSV 双引号包裹与内部引号转义。普通中文、逗号、引号和字段内部换行保留内容。

历史 run 的 profile revision 来自 Start 快照，不从当前方案文件推断。导出失败显示错误且不替换既有目标；导出不改变运行、账本或选中方案。导出路径规范化后禁止覆盖 workspace.sqlite、其 -wal/-shm/-journal/.writer.lock 旁文件及 profiles 目录中的文件，返回 CSV_TARGET_PROTECTED；Windows 路径比较不区分大小写。

| 错误 | 触发条件 | 保留状态与下一步 |
|---|---|---|
| REVIEW_DIRTY | 未保存审定草稿时切换/开始 | 保留草稿与当前选择，先保存或明确丢弃 |
| RUN_ACTIVE | 运行或暂停中切方案/模式/历史/审定保存 | 保留当前运行快照，先停止 |
| DB_READ_ONLY | 只读工作区尝试保存或开始 | 保留只读数据，使用可写工作区后再操作 |
| QUANTUM_INVALID / PROFILE_INVALID | 精度、来源、稳定 ID 或禁用状态校验失败 | 不开始运行或覆盖旧方案，修正审定输入/外部文件 |
| PROFILE_LOAD_FAILED / PROFILE_SAVE_FAILED | 方案读取或原子写入失败 | 保留原文件与选择，界面显示错误 |
| DB_BUSY / COMMIT 失败 | 真实数据库锁或提交失败 | 保留最后已提交投影与步骤，解除故障后重试同一步 |
| CSV_TARGET_PROTECTED / CSV_WRITE_FAILED | 导出目标为工作区存储文件或写入失败 | 保护工作区数据，改选普通导出路径 |

## 8. 验证矩阵与当前结果

| 验证组 | 方法 | 本轮结果 |
|---|---|---|
| 审定/文件/revision | 真实临时目录和 ConfigV2 重开；不完整审定/错误目标反例 | 通过；包含来源/稳定 ID/精度校验与原文件保留 |
| dirty/active/mode | controller 保护与离屏配置切换/关闭交互，核对投影、草稿与选择 | 通过；UI 覆盖切模式保留草稿及关闭取消/丢弃 |
| 控制与分类 | 八步 fixture、暂停单步、Unknown 保留 | 通过；完整 fixture 分类与 UI 控件结果一致 |
| 已提交来源 | 独立 SQLite 只读连接对照 events/attempts/ledger | 通过；无成功账本的伪回执不显示确认价 |
| busy/COMMIT 失败 | 外部真实写锁/读锁，不使用假错误返回 | 通过；保留已提交计数/记录/步号 |
| 重开与历史 | 关闭/重开真实工作区，检查 Unknown 与原 profile 快照 | 通过；不自动继续，历史 revision 不随现文件变化 |
| CSV | 公式/控制字符反例、存储目标覆盖保护与真实导出 | 通过；DB/sidecars/profiles 受保护 |
| 离屏控件 | 原 UI 自测 + workspace 控件、审定、错误与恢复场景 | 58 条 workspace UI 断言，failures=0；UI_SELF_TEST=PASS |
| 发布/回退 | 固定目录、包内依赖、自测、另副本恢复旧发布 | PACKAGE=PASS；22 条存储自检；回退/恢复 exit=0 |

应用测试入口：

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Package
~~~

最终构建与打包实际使用组合命令 `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test -Package`。构建记录见 artifacts/m1_pr11_transaction/build_result.json 与 logs/final_build.stdout/.stderr；六阶段发布/基线/回退命令见 package_commands.json，均 exit=0。单独 workspace_tests.exe 的 400 条断言另将 QT_PLUGIN_PATH 指到发布目录；发布 EXE 在清理后的 package-only PATH 下自检，不依赖 SDK。

精确通过数、stdout、退出码、哈希与回退状态见 [09_m1_progress.md](../09_m1_progress.md) 和 artifacts/m1_pr11_transaction/VERIFICATION.txt。文档 validator 只做规格/链接/基线检查，不替代 CTest。本轮基线使用 m1_pr11_baseline.json，历史基线保留。

## 9. 后续 PR12

PR12 对现在的持久化工作区完成整套端到端与发布验收，包括旧配置兼容、数据目录隔离、方案快照、只读诊断、重复启动、回退保留新数据库、异常关闭、包内 driver 和全部 UI 状态。真实捕获/OCR 仍为 M2，延迟/限购未决语义和外部动作不因本票 UI 接入而视为完成。

可直接复用的下一轮任务见 [NEXT_IMPLEMENTATION.md](../NEXT_IMPLEMENTATION.md)。技术依据沿用 [PR08 配置提交](profile_store_pr08.md)、[PR10 存储与官方资料](sqlite_store_pr10.md) 与 [UI 集成契约](integration/ui_integration.md)；当前测试源码和日志是本票实现结论的依据。
