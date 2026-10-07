# M1 实施进度与分阶段基线

更新时间：2026-10-07（Asia/Shanghai）

## 最新：市场页校准与原关注预检

已修复系统OCR字框的TextAngle映射，新增同帧局部文字增强与有界近邻标签匹配。曼德尔、典藏首页、空关注、总筛选已实机核对；最终同一前台批次7个匹配检查点、9次采集、4次导航，前置/恢复各一次。26/26应用测试，OCR75、市场70、启动250、大厅37、焦点27条断言及批次10项测试通过。游戏停在总筛选页，尚未选择条件或执行购买。详见 [本轮记录](implementation/runtime/market_page_calibration_m2.md)；下一段是S10–S14状态读取与真实运行时接入，不覆盖下面历史基线。

## 最新：整段前台批次修复

用户指出逐动作切屏后，已实现调用方持有的前台批次：中间诊断不切换，末尾一次恢复。25/25应用测试，Python批次6项测试；真实连续两帧显示enter=1、leave=1、子诊断切换请求=0。人工导航已观察到曼德尔砖市场页，但该页分类仍Unknown；不能将焦点验证写成业务识别通过。详细见 [整段批次记录](implementation/runtime/foreground_batch_m2.md)。

## 当前新增：大厅实机校准

真实大厅2560×1440/144DPI已用两个独立帧验证；修复Windows OCR将开始按钮读成“开始游观”的局部情况。新诊断 `--expected-page` 区分截图/OCR成功与页面匹配成功，错误预期页实测退出1。游戏截图只在内存，检查后恢复IDE；没有游戏点击。应用CTest24/24、大厅37条断言，原启动250条保持；发布回归及副本回滚通过。详见 [大厅校准记录](implementation/runtime/lobby_live_calibration_m2.md) 和 `artifacts/m2_lobby_calibration/VERIFICATION.txt`。下一步由用户展示仓库，不跳到普通物资交易行。下方是前序阶段历史记录。

## 当前新增：原启动页面分类与只读观察器

原入口的多锚点页类、关注预检、空关注回首页再筛选、已有列表接续已实现为独立 `relink_startup_observation` 模块。27条历史OCR投影及5条包内分支检查已加入，应用CTest为23组；本轮未采集当前游戏、未切前台、未发输入。下一步用户准备后做当前画面校准。详见 [启动观察器](implementation/runtime/startup_observer_m2.md) 与 `artifacts/m2_startup_observer_transaction/VERIFICATION.txt`。原39步导航/交易整体尚未完成。

## 2026-10-07 新增：真实采集基础层与原启动流程重核

- 最新入口：[10 · BBZPS 首次启动逐步复刻](10_bbzps_first_startup_reconstruction.md)。用户要求按原业务复制，不重写业务路径。原始日志重新全量扫描，100次运行入口（97次全自动皮肤/3次发送测试），1,192条定位证据重新读取验证；不是冷启动100次。
- 新增独立 `relink_windows_capture`：显式窗口/PID/创建时间身份，DPI、前台/遮挡、窗口变化检查，DXGI一步触发的内存帧。真实游戏3帧已通过：2560×1440、144DPI；159–220ms是首次检查的单帧采集耗时，不是端到端识别延迟。已记录一次焦点切换失败，并通过创建线程消息队列修复后成功复验。
- `relink_windows_ocr` 使用独立隐藏系统PowerShell进程和Windows.Media.Ocr，通过paging-file mapping读取像素。已用合成“MARKET 123456”实际调用Windows OCR并核对文字，不再是hash fixture。尚未把真实枪皮字段/原页面识别宣称为已验证。
- 发布EXE新增显式 `--live-capture-check` 诊断入口；正常UI/配置/回放行为不变，未接入游戏点击或交易。最新应用回归为21组；新增原流程文档测试独立执行，不把文本断言当真实业务验收。
- 用户纠正业务入口后，优先细拆原启动分派/关注预检/筛选/商品回读/关注接续；暂停扩展臆测的普通交易行业务。详细模块边界见 [M2实现记录](implementation/runtime/live_capture_m2.md)。
- 新事务证据：`artifacts/m2_live_capture_transaction/VERIFICATION.txt`；旧PR12B记录保持原样。下面保留历史实施信息。

## PR12B 阶段历史结论

M1 已完成 PR01–PR11、PR12A 与 PR12B 的服务、历史、观察、协议/lease 和通用视觉传输基础层。当前源码已有 19/19 CTest 记录：工作区 432、UI 模块 90、观察 828、协议 646、进程内链路 279、Win32 共享帧 65、隐藏合成进程传输 206 条断言通过；独立负向包验收记录为 41/41。新增 transport 后的最终发布、哈希与回退结果以本轮构建后更新的 VERIFICATION.txt 为准，不沿用旧包结果。

本轮业务来源仍为 synthetic/replay。纯状态层 relink_runtime 与独立 relink_vision_transport 分离；后者已实际验证 Win32 paging-file mapping 和隐藏合成 fixture child，但未链接桌面程序。child 不是 OCR worker，不随发布包交付。当前仍没有 WGC/DXGI 游戏窗口来源、真实 ROI 和 OCR provider 校准，也没有系统输入或交易。像素仅走内存，imageFileWriteCount=0。下一步需要真实游戏窗口/帧参与识别业务时暂停总结；历史测试数量保留在各自章节。

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
| PR11 | 完成 | WorkspaceController、审定保存、持久化合成回放、已提交投影、历史快照、恢复提示、CSV 与错误交互 |
| PR12A | 完成 | 页面/编辑器/诊断入口解耦，独立 UI 模块与回归；服务层整理已由 PR12B 完成 |
| PR12B | 源码与应用测试记录完成，发布复验独立记录 | 服务边界、内存观察、协议/lease、Win32 共享帧与隐藏合成 worker transport；19/19 CTest，最终发布/回退证据单独记录 |

## PR12A 历史实施范围与验证

详见 [页面模块化实现记录](implementation/ui_modularization_pr12a.md)。本节数量和“本轮”均指 PR12A 当时记录，不是当前 PR12B 的测试数量。

- 独立 `TaskPage`、`RunSettingsPage`、`WorkspaceRecordsPanel`、`TaskEditorDialog`；控件和回调由所属页面持有，主窗口只保留组合引用。
- 共享呈现工具不重复复制；WorkspaceProjection DTO 独立头文件，记录面板不依赖 controller/SQL。
- `main.cpp` 1,037 → 164 行，`mainwindow.cpp` 3,032 → 1,912 行。只是第一轮抽离，剩余页面/服务职责仍待整理。
- `relink_ui` 是静态模块，不引入后台服务/插件系统；独立页面测试不链接 SQLite。完整离屏自检仍在发布程序中，不等同于包体或内存优化。
- 发布脚本显式等待包内存储自检进程，并检查该进程的 ExitCode，避免依赖遗留 `$LASTEXITCODE`。

| 本轮实际验收 | 结果 |
|---|---|
| 完整 CTest | 14/14 通过 |
| 独立 UI 模块测试 | 90 断言，0 失败 |
| 原完整 UI 检查 | 206 项通过 |
| 工作区服务 / UI | 400 / 58 断言通过 |
| 包内存储自检 | 22 断言通过 |
| 基线 / 修改 / 副本回退 / 恢复 | exit 0；固定发布保留新版，新数据库和方案哨兵保留 |
| 抽取页面视觉保真 | 任务页、任务编辑器、运行页及其完整页、关注页、价格页共 6 张离屏图像与基线逐像素相同 |

发布程序继续在固定解压目录。精确命令/输入/输出/退出码/哈希见 `artifacts/m1_pr12a_transaction/VERIFICATION.txt`；图像比较见同目录 `visual_comparison.json`。自动化截图只是显式测试产物，业务 `imageFileWriteCount=0` 保持。历史 baseline/schema/fixture/backlog 不覆盖。

## PR11 历史实施范围

详细接口、数据路径、八步 fixture 与 UI 行为见 [持久化工作区实现记录](implementation/workspace_pr11.md)。

- 增加 `WorkspaceController` 与 `WorkspaceProjection`，controller 在创建线程持有 SQLite；MainWindow 消费投影，不在 widgets 内执行 SQL。数据库仍为 schema v2。
- 原始导入预览仍只读，独立审定窗口收集方案名、精度与必要确认后通过 ProfileStore/QSaveFile 保存 ConfigV2。保存不自动选择方案，选择不启用规则；enabled=false、activation_required=true 保持不变。
- 方案列表展示 ID/revision/来源/审定状态。每次 Start 保存完整 `profile_snapshot`；历史运行使用当时方案 JSON/revision，不受后续方案文件更新影响。
- Replay 控制接入开始、暂停、继续、单步、快速推进和停止；明确选择内置八步 fixture 后才有模拟派发/回执。暂停和停止保留 Unknown/预留，重开只读查看历史，不自动重发或继续。
- 成功/失败/Unknown/预留来自已提交账本；Match/NoMatch/NeedsReview 和派发来自已提交事件。事务失败保留旧计数、记录与步骤，并显示结构化错误。
- 同商品多个卖单按 listing/observation 身份区分；观察价与已确认模拟价分开，缺字段、stale、时钟和来源可见；单独的 ReceiptConfirmed 形状事件不构成确认。
- 历史选择、记录页、统计与价格图接入工作区投影；CSV 包含 mode、profile ID/revision、来源、时钟与记录身份，并转义公式型字段。
- 审定草稿、旧演示配置和已保存方案分离；脏配置/切模式/切方案/关闭窗口交互保留用户明确选择。错误保存不会把旧数据覆盖为半成品。
- 普通启动与离屏测试使用不同数据路径；离屏默认 QTemporaryDir，显式 --workspace-dir 才使用指定测试工作区。业务 `imageFileWriteCount=0`，显式 UI 验收截图单独保存，不是捕获/OCR 落盘。

## PR10 历史实施范围

详细接口、恢复规则与历史 DDL 差异见 [SqliteEventStore 实现记录](implementation/sqlite_store_pr10.md)。

- 引入独立 `relink_sqlite` 目标与 `Qt6::Sql`；`SqliteEventStore` 实现现有 `IEventStore`，配置仍由 PR08 的 QSaveFile 独立保存。
- 数据库采用 `user_version=2`；空库初始化、v1→v2 事务迁移、future/corrupt/unrecognized 拒绝，以及一致性备份均通过明确接口处理。
- 写操作在显式事务中暂存；SQLite 查询只返回已提交快照。业务/SQL 写失败回滚整笔事务并锁存 aborted，显式 `rollback()` 后才继续。
- 重开时，明确未派发的 Prepared 取消并释放预留；可能已派发的未决记录转 Unknown 并保留预留，写入恢复审计，不自动重发。
- `commitLedger()` 对 Unknown/ambiguous 回执返回 `RECEIPT_UNCONFIRMED`；终态与已确认回执不一致返回 `RECEIPT_OUTCOME_MISMATCH`。同一保护同步到内存仓库，避免将未确认回执直接记成成功/失败。
- PR10 当时新增 `--storage-self-test`，仅使用临时数据库自测驱动、提交、重开恢复与备份；当时正常前台数据流尚未接 SQLite。PR11 已由 WorkspaceController 接通正常工作区，当前行为见上方 PR11 记录。
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

以下为 PR08 已记录结果，不是当前 PR12B 的新测试输出：

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

### PR11 历史验证

PR11 当时状态：**实测通过**。本节“本轮”和“最终”均指 PR11。最终命令为 `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test -Package`，实际构建目录 `build_relocated`。`artifacts/m1_pr11_transaction/build_result.json` 保存命令、工作目录、stdout/stderr 和 exit=0；原始构建日志为 `logs/final_build.stdout` 与 `logs/final_build.stderr`。早期 `build_test_2.log` 与 49 条 UI 联调记录保留为历史，不作为 PR11 最终通过数。

| 检查 | 本轮结果 | 实际证据 |
|---|---|---|
| 最终完整构建与 CTest | 13/13 通过，exit=0 | build_result.json；logs/final_build.stdout |
| 工作区服务与真实存储故障 | 400 条断言，failures=0，exit=0 | package_commands.json 的 WORKSPACE_TESTS；真实临时 DB、外部写锁与提交锁 |
| 发布目录持久化 UI | 58 条断言，failures=0，exit=0 | package_commands.json 的 MODIFIED_UI；同时 UI_SELF_TEST=PASS |
| 发布 QtSql/QSQLITE | 22 条存储自检，exit=0；PACKAGE=PASS | build_result.json 与 package_commands.json 的 MODIFIED |
| 原程序基线与恢复副本 | BASELINE/ROLLBACK/RESTORED 均 exit=0；原版哈希一致 | package_commands.json；rollback_test；新数据库保留 |
| 文档/契约/本轮基线 | 独立于应用测试记录 | 文档脚本与 m1_pr11_baseline.json；最终结果见 VERIFICATION.txt |

工作区测试验证审定/source/quantum/稳定 ID、禁用方案、切换与快照、真实锁/COMMIT 失败、重开 Unknown、CSV 注入转义与保护 DB/方案文件不被导出覆盖。UI 自测实际操作控件，覆盖同商品多卖单、中文状态、观察价/确认价、审定保存、真实锁错误、历史重开、脏演示配置的切模式选择与关闭取消/丢弃，不只是静态检查标签。

本轮关键原始输出：

~~~text
100% tests passed out of 13
WORKSPACE_TESTS=PASS; assertions=400; failures=0; real_database=true; real_lock_failures=true; external_actions=0
WORKSPACE_UI_SELF_TEST=PASS; assertions=58; failures=0; persistent_store=true
UI_SELF_TEST=PASS; offscreen=true; game_connected=false; system_input_sent=false
STORAGE_SELF_TEST=PASS; driver=QSQLITE; temporary_data=true; system_input_sent=false; assertions=22
ROLLBACK_RESTORED=PASS; new_databases_preserved=true
PR11_PACKAGE_VERIFICATION=PASS
~~~

发布程序与旧版恢复程序使用清理后的 package-only PATH 离屏运行，不依赖 SDK；单独运行 build_relocated/workspace_tests.exe 时，另将 QT_PLUGIN_PATH 指到发布目录以解析 QSQLITE。精确命令、输入、完整输出和退出码保存在 package_commands.json 与 VERIFICATION.txt。`UI_WORKSPACE_NO_CAPTURE_IMAGE_WRITES=PASS` 表明业务图像文件计数为零；UI 验收快照是显式测试产物，不是业务捕获。

最终发布 EXE 的 SHA-256 为 `fed34a7250247684d0daaa7507ff4df40ac23cfcfd36566b4bf3570fb19deb2d`。旧版基线与回退副本均为 `d19688ccbda8786246c6d8927fc49ac5484f7b8acc42d5e3256fd84250d1e3a6`，恢复后旧程序离屏自检通过。回退只操作独立 rollback_test 副本，固定发布目录保留新版。

四项事务工件位于 `artifacts/m1_pr11_transaction/`：`MODIFIED_FILE.exe`、`DIFF_FILE`、`VERIFICATION.txt`、可执行 `ROLLBACK.sh`；回退恢复程序文件而保留新账本数据。PR11 单票验收完成，不等于 PR12 整套端到端里程碑或 M2 捕获/OCR 已完成。

### PR10 历史验证

本节保留 PR10 的历史记录；其中“本轮”指 PR10，不是当前 PR12B。

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
- `artifacts/business_rebuild_docs/m1_pr10_baseline.json`：历史 PR10 源码、测试、当时状态文档与发布目录基线，保留不覆盖。
- `artifacts/business_rebuild_docs/m1_pr11_baseline.json`：历史 PR11 源码、测试、当时入口文档与发布目录基线；保留用于审计。
- `artifacts/business_rebuild_docs/m1_pr12b_baseline.json`：PR12B 源码、测试、当前文档与发布目录基线；本轮命令证据见 `artifacts/m1_pr12b_transaction/VERIFICATION.txt`。

每一轮基线都只记录实际变更范围，不覆盖历史快照；文档验证同时检查项目基线漂移是否被最新 M1 基线明确覆盖。

## 下一阶段

| 阶段 | 状态 | 下一步 |
|---|---|---|
| PR12B / 本轮非游戏验收 | 实现与应用回归已通过 | 工作区/界面/存储/观察/协议/进程内集成/独立 OS 传输；固定发布、参数化交付及独立副本回退由本轮最终报告确认 |
| M2 | 通用 transport 已验证，真实视觉业务待联调 | 复用观察、共享帧与进程接口；真实游戏窗口/frame source、ROI 和 OCR provider 校准尚未完成 |

下一轮任务见 [NEXT_IMPLEMENTATION.md](NEXT_IMPLEMENTATION.md)。已完成的非游戏模块不重复重做；每次发布仍重跑对应回归并刷新证据。真实图像识别需要游戏画面时才进入单独的业务联调，合成观察、配置、账本、恢复及通用传输与真实视觉验收分别记录。

## PR12B 本轮证据（2026-10-07）

服务职责分离为 `ProfileCatalog`、`ReplayScenario`、`HistoryQueryService`、`RecordsExporter`，控制器保留协调和生命周期。步骤触发式观察层通过 `IObservationSource`、`IObservationRecognizer` 分离来源与识别；当前测试来源只提供合成内存帧。接口和限制见 [内存观察适配器](implementation/runtime/observation_adapter_pr12b.md)。

| 本轮实际运行 | 结果 |
|---|---|
| 全量 CTest | 19/19 通过 |
| 工作区服务测试 | 432 条断言，0 失败 |
| 独立 UI 模块测试 | 90 条断言，0 失败 |
| 原 UI / 工作区 UI 自检 | 206 / 58 项通过 |
| 存储自检 | 22 条断言通过；本轮包内复验见最终事务报告 |
| 观察适配器 | 828 条合成内存帧断言通过；未使用游戏画面 |
| worker 协议与 lease | 646 条断言通过；外部进程和图像文件写入均为 0 |
| 进程内图像链路 | 279 条断言通过；合成内存帧/协议/lease 集成，未启动真实 worker |
| Win32 共享帧 | 65 条断言通过；真实 paging-file mapping、只读读取和容量/身份检查，像素为合成数据 |
| 隐藏合成进程传输 | 206 条断言通过；实际启动 fixture child，验证协议往返/取消/超时/回收；不是 OCR，child 不随桌面包交付 |
| 独立负向包验收 | 41/41；tiny fixture 验证清单及交付工具的拒绝路径，不等于最终发布包通过 |
| 打包 / 修改 / 回退 / 恢复副本 | 2026-10-07 最新源码重新构建、19/19 CTest 与固定目录打包通过；修改版/恢复副本自检和回退均 exit=0；新 DB/profile sentinel 保留 |
| 参数化交付验收 | 最终交付目录清单、配置/工作区保持及配置回退通过，exit=0；证据 `artifacts/m1_pr12b_delivery_final/VERIFICATION.txt`，与 41 条负向测试分开记录 |
| 视觉保持 | 任务页、编辑器、运行页、完整运行页、关注页、价格页共 6 张离屏图像与基线像素一致 |

命令、输入、字面 stdout/stderr、退出状态和哈希以本轮构建后更新的 `artifacts/m1_pr12b_transaction/VERIFICATION.txt` 为准。同目录保留 `MODIFIED_FILE.exe`、`DIFF_FILE`、`ROLLBACK.sh`；回退只在独立副本验收。

本轮保持 `game_connected=false`、`system_input_sent=false`、`image_file_write_count=0`。离屏 PNG 仅是显式 UI 测试产物；真实游戏进程、桌面采集、OCR、输入注入、购买和交易未运行，不属于本轮 PASS 的含义。

worker 的具体实现、资源上限和回收语义见 [协议与 lease 实现](implementation/runtime/worker_protocol_pr12b.md)，合成内存帧与协议的串联见 [进程内图像链路](implementation/runtime/vision_pipeline_pr12b.md)，真实 mapping/隐藏合成进程边界见 [独立传输层](implementation/runtime/vision_transport_pr12b.md)。发布验收额外覆盖 `--snapshot-dir` 单独运行时配置/工作区逐字节保持、默认临时工作区清理、清单精确匹配、链接/重解析点拒绝，以及实际执行回退脚本恢复另一份已修改程序。
