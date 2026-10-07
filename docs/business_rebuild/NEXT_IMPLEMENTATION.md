# 下一轮开发交接：M2 真实画面采集与识别边界

> **取代下方旧实机安排：** 先读 `implementation/runtime/foreground_batch_m2.md`。用户要求智能体自行导航，整段操作完成再回IDE，不允许每次动作切屏。使用 `run_foreground_batch.py` 一次执行完整有限计划，子诊断 `--focus-policy caller-owned`；不要循环调用单次探针。当前已到曼德尔砖页（F4市场入口），页类仍Unknown，先后台补锚点诊断再整段验证。25组应用测试及连续两帧焦点实测已通过。

> **最新：大厅实机校准通过。** 阅读 `implementation/runtime/lobby_live_calibration_m2.md`。用户已展示大厅，Windows OCR局部误字修复后，两次独立帧通过；当前24组CTest。已请用户手动点仓库，收到到页消息后重解析窗口身份并执行只读页类校准。复用 `SkinPageClassifier` / `StartupObserver`，不重做原流程；尚未验证其它页和完整导航。CLI `--expected-page` 不把截图成功当页类通过，每轮结束恢复IDE前台。

## 2026-10-07 用户纠正后的最高优先级

先阅读 `10_bbzps_first_startup_reconstruction.md` 和 `evidence/startup_flow_evidence.json`。
按BBZPS已证实的原顺序复刻业务：当前页分派/恢复→典藏外观→已有关注预检→空队列回筛选，或者已有列表直接接续；再实现赛季/拥有/品阶、批次查找、标题复核、成色/公示/排序、价格/磨损联合匹配、收藏回执、关注排序/校时/倒计时/确认/结果处理。
不要要求用户进入普通物资交易行；不要把顶部导航文字当当前业务页；不要省略启动关注预检或强制每次从头收藏。

```text
继续开发时复用 src/application/vision/target_window.*、dxgi_observation_source.*、
windows_ocr.* 及已有 ObservationAdapter/SharedFrameMemory。
当前21组应用测试通过；真实游戏采集已测，Windows OCR已用合成英文文字验证。
原流程细节以S01–S39与入口分支表为实施规格，缺少证据的动作/公式不自行补造。
保持Win11白灰UI，保存配置不触发点击/购买；业务参数仍独立于诊断。
联调只短暂切游戏，完成后恢复并验证Mirasim为前台。
不运行BBZPS，不使用其DLL；不保存运行截图或OCR全文；不修改用户系统时间。
先完成原页面识别与只读启动路径回放，再按原顺序接入各步；不另造简化流程。
固定解压路径仍为 C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe。
```

下文为PR12B结束时的历史交接，不能覆盖上述新进展和用户纠正。

更新时间：2026-10-07（Asia/Shanghai）。当前源码已有 19/19 CTest 记录：工作区 432、UI 模块 90、观察 828、协议 646、进程内链路 279、Win32 共享帧 65、隐藏合成进程传输 206 条断言通过；负向包验收已有 41/41 记录。新增 transport 后的最终发布/回退记录须以本轮构建后更新的 `artifacts/m1_pr12b_transaction/VERIFICATION.txt` 和 `artifacts/m1_pr12b_delivery_final/VERIFICATION.txt` 为准；不复用先前版本结果。

**通用内存、协议、lease 和进程传输已完成；下一步是真实游戏窗口/frame source、ROI 与 OCR provider 校准。** relink_vision_transport 已实际验证 Win32 共享内存和隐藏合成 fixture child，但未链接桌面程序；child 不是 OCR worker，也不随包发布。产品仍为 synthetic/replay。只有真正需要游戏画面验证识别业务时才暂停总结，不把通用传输通过称为生产视觉链路完成。

## 1. 已完成的可复用边界

- `ProfileCatalog`、`ReplayScenario`、`HistoryQueryService`、`RecordsExporter` 已从通用控制器中分离。
- `ObservationAdapter` 已实现 one-shot / bounded-watch、帧预算、节流、超时、取消、迟到回调和 lease 生命周期。
- 内存帧包含 session/clock/step/cancel epoch/viewport generation；新鲜度、ROI、stride/像素容量均有合成测试。
- `IObservationSource` 与 `IObservationRecognizer` 为来源和识别保留独立接口；`InMemoryReplaySource` 是当前唯一经过测试的帧来源。
- WorkerProtocol 与 NdjsonFramer 实现消息结构、分帧、握手、关联、取消、释放与排空；LeasePool 实现双槽生命周期、代际身份与崩溃隔离。
- SharedFrameMemory / ReadOnlyFrameMemory 和 WorkerProcess 属于独立传输层，已通过真实 Win32 mapping、隐藏合成子进程及异常退出/取消回收测试；不重复重写。
- schema v2、ConfigV2/QSaveFile、历史 profile snapshot、Unknown/reservation、Win11 白灰 UI 与既有入口保持。

接口说明见 [内存观察](implementation/runtime/observation_adapter_pr12b.md)、[协议与 lease](implementation/runtime/worker_protocol_pr12b.md)、[进程内链路](implementation/runtime/vision_pipeline_pr12b.md) 和 [共享帧/隐藏进程传输](implementation/runtime/vision_transport_pr12b.md)。

## 2. 直接复用：M2 开发提示词

~~~text
工作目录：C:\Users\Administrator\Desktop\price。
项目：Delta Market Assistant（三角洲市场助手），构建目标仍为 RelinkStudio。
先读 AGENTS.md、SESSION_START.md、09_m1_progress.md、
implementation/runtime/observation_adapter_pr12b.md、worker_protocol_pr12b.md、vision_transport_pr12b.md
和当前 transaction/VERIFICATION.txt。
检查 git status、现有源文件与产物；保留所有现有修改，不重复 PR12A/PR12B。

已完成：服务/工作区事务/历史/CSV、Win11 页面、内存观察、协议/lease、真实 Win32 共享帧、隐藏合成 worker transport。
完整 CTest 19/19；共享帧 65、进程传输 206、负向包验收已有 41/41 记录。核对本轮最终发布证据，不使用旧包成绩。
当前未完成：真实游戏窗口/frame source、DPI/遮挡/最小化/重建、ROI、OCR provider/模型校准及真实画面业务验证。

M2 工作必须复用 ObservationDemand / FrameEnvelope / IObservationSource /
IObservationRecognizer / WorkerProtocol / LeasePool，不将捕获代码和 SQL 塞进页面。
采集只由业务步骤触发；one-shot 或显式有界 watch；空闲时不循环读屏。
像素只走内存，不通过截图路径、PNG/BMP 临时文件或反复落盘传给识别器。
配置、必要账本和审计仍正常持久化，不为减少 IO 跳过事务。

真实画面联调需确认窗口标识、客户区/物理像素、DPI、viewport generation、
遮挡/最小化/窗口重建、source timestamp 与 cancellation 边界。
识别需确定页状态、商品/卖单身份、价格、成色等 ROI、缺失值/低置信度规则，
再建立真实样本的准确率与误识别拒绝矩阵；合成测试不替代真实画面结果。
保持现有回放/账本只接收结构化结果；观察价格不得冒充确认成交价。
保留 fake/replay 回归，真实画面才需要游戏联调，其余开发/测试继续推进。

不执行 BBZPS 或加载其中的 DLL/脚本/插件，不把原样本放入发布包。
不自动打开可见窗口，不占用用户前台；不把识别接入自动购买。
保留 Win11 白灰、近黑强调、无蓝色和现有功能入口。
固定解压交付：C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe。
更新 DLL、platforms、sqldrivers、qt.conf；构建、离屏验证、独立副本回退并记录真实证据。
~~~

## 3. 本轮复验入口

以下入口重跑的是合成/离屏验证，不代表真实游戏捕获或 OCR：

~~~powershell
python -I -X utf8 tests\release\verify_pr12b_package.py baseline
python -I -X utf8 tests\release\verify_pr12b_package.py build
python -I -X utf8 tests\release\verify_pr12b_package.py modified --build-dir .\build_relocated
python -I -X utf8 tests\release\verify_pr12b_package.py finalize
~~~

历史事务目录不得覆盖成另一个阶段的基线；未来修改建立新的事务目录。回退在独立副本完成，固定发布目录保留新版本。文档/schema 校验与应用测试分别记录。

## 4. 原 PR12 全套验收清单（历史规格）

以下保留原始非游戏端到端验收清单，作为维护回归依据，不再作为待开始任务。PR12B 实际完成结果只以本轮事务证据和当前实施进度为准；冻结 backlog/test_matrix 不批量改写为 PASS。真实画面采集和 OCR 始终单列 M2。

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
