# Relink Studio — new session entry

## 最新：市场页坐标与原关注预检实机通过（2026-10-07）

- 原规则Unknown的根因之一是Windows OCR `TextAngle`未处理，已按原图中心顺时针变换四角；曼德尔−6.1°、典藏+10.4°是本轮实际值。不是为通过测试篡改页面别名。
- 增加同帧局部OCR：导航/关注入口2x灰度对比度，空状态3x反色；全部内存处理。局部空结果不擦除原全页词；局部精确近邻拼接避开3D背景文字干扰。
- 最终发布包实机完成：空关注两次→首页→我的关注两次→首页→总筛选。7个匹配检查点、9次采集（2次Unknown重读全部保留）、4次导航；整段enter=1/leave=1、子诊断切屏=0，已恢复Mirasim。
- 游戏现在在总筛选页。没有勾选筛选项、增删关注、购买或移动物品。下一步S10–S14读取赛季/拥有/品阶状态；之后才是商品名/成色/价格绑定。
- 先读 `docs/business_rebuild/implementation/runtime/market_page_calibration_m2.md`。事务 `artifacts/m2_market_calibration/VERIFICATION.txt`；26组CTest、OCR75、市场70、原启动250断言、批次10项测试通过。交付仍为固定解压路径。
- `run_foreground_batch.py` 只对预期页Unknown做有界新帧重读（最多3），不重放点击、不重抢用户切走的前台。所有实机使用完整批次，禁止循环单步探针。
- 当前是页面校准与人工联调脚本导航，不是完整前端自动业务；默认StartupObserver的一秒门槛尚未改动，实际多ROI诊断可能超过它，下一步接异步运行时时必须明确页类新鲜度，不能冒充已接通。

下方为此前阶段记录；“曼德尔仍Unknown”由上方实测更新取代。

## 最高优先级纠正：禁止逐动作切屏（2026-10-07）

- 用户明确指出闪屏：一段任务开始才切到游戏，中间连续操作/识别，整段结束或异常时才恢复IDE一次。用户也要求由智能体点击页面导航，不再逐页让用户代点。
- 已实现 `ForegroundPolicy` 与 `tests/manual/run_foreground_batch.py`；子诊断使用 `--focus-policy caller-owned`，不激活/不恢复。原逐动作脚本已转接批次。25/25 CTest、前台策略测试、Python协调器6项通过；一段真实2帧检查仅1次enter/1次leave，两个子诊断焦点请求均为0。
- 最新入口 `docs/business_rebuild/implementation/runtime/foreground_batch_m2.md`。游戏目前停在曼德尔砖市场页，已经观察到“典藏外观”页签；从大厅底部F4市场进入，不是顶部普通交易行。分类器对当前曼德尔页仍Unknown，下一步先后台补最小诊断再在一个完整批次校准，不能假报页面通过。
- 本轮用户授权后有3次人工导航点击，无购买、库存移动、关注修改；初始大厅只读阶段和后来导航阶段分别记账。最后新批次只读检查已恢复Mirasim前台。
- 交付路径不变，事务 `artifacts/m2_lobby_calibration/VERIFICATION.txt`。大厅实测对应的旧二进制保存在 `LOBBY_CALIBRATED_FILE.exe`，不要把旧实测哈希改标为新焦点修复版。

以下历史记录若说“每次检查都回IDE”或“等待用户逐页点击”，均由上方新约定取代。

## 最新实机节点：大厅校准通过（2026-10-07）

- 用户已准备大厅；本轮实机2560×1440、144DPI。原规则因Windows OCR将“开始游戏”读成“开始游观”而Unknown；已做provider+固定区域+仓库/行前备战独立锚点的局部兼容。
- 修复发布包两次独立帧均判为lobby；故意expected-page=warehouse时退出1，不把采集成功当页类成功。所有尝试含一次初始前台变化失败均保留，均恢复IDE；游戏图片未落盘，无游戏输入。
- 应用CTest24/24；大厅专项37、原启动250断言；包内另有1条实机文字投影回放。六张UI基线不变，发布与回滚已验收。
- 先读 `docs/business_rebuild/implementation/runtime/lobby_live_calibration_m2.md`。新事务 `artifacts/m2_lobby_calibration/VERIFICATION.txt`；程序固定路径不变。
- 已请用户手动点顶部“仓库”并回报到页；收到准备消息后用 `tests/manual/live_startup_probe.py --expected-page warehouse --record <新的artifacts内json路径>` 做下一页只读检查。每次重解析PID/HWND，不复用本轮编号；结束检查IDE前台。尚未验证仓库/曼德尔/典藏/商品字段，更没有自动导航。

以下为此前阶段记录。

## 最新开发节点：原启动只读观察器已接入（2026-10-07）

- 用户问实机何时需要：本轮先完成离线开发与回归，没有切换游戏前台或采集游戏画面；下一步需要用户准备后做当前版本页面校准。
- 已有 `SkinPageClassifier` / `StartupObserver` 独立QtCore模块；按原顺序处理关注预检、返回首页再筛选、已有列表接续。只有检查点输出，无采集/导航/购买命令。
- 27条历史OCR锚点投影已嵌入发布程序，`--startup-observer-self-test` 离线检查27页/5分支；不是当前游戏图片识别准确率。应用CTest现为23组。
- 原日志“大战场/应用外观”在部分记录里实际为视频设置页；已保留旧标签但不将其作为页类真值。
- 先读 `docs/business_rebuild/implementation/runtime/startup_observer_m2.md`；本轮事务 `artifacts/m2_startup_observer_transaction/VERIFICATION.txt`。
- 实机准备：游戏停大厅，保留现有分辨率/DPI及关注条目；用户准备后逐页只读校准，结束后恢复IDE前台。不要擅自去普通物资交易行或清空关注。

下方保留前一阶段记录。

## 当前入口：先复刻 BBZPS 原启动流程（2026-10-07）

用户最新纠正：仔细拆解BBZPS首次启动全部流程，按原业务直接复刻，不自行重写业务路径。
先读 `docs/business_rebuild/10_bbzps_first_startup_reconstruction.md`：39个步骤、各入口分派、日志逐行证据，特别是“先检查我的关注”“已有皮肤列表直接接续”，不是普通物资交易行。

- 已全量重查15日志/1,774,992,038字节；97次全自动皮肤和3次发送测试入口；1192条定位重新校验。冷启动UI函数序列尚有明确缺口，不伪称全还原。
- M2新增 `relink_windows_capture` 与 `relink_windows_ocr`；真实游戏DXGI内存3帧通过，Windows OCR已实际识别合成文字。真实枪皮字段和39步导航尚未连接游戏。
- 当前应用CTest 21/21；窗口104、OCR52条断言；原流程证据契约12项；原UI和账本回归保持。新证据在 `artifacts/m2_live_capture_transaction/VERIFICATION.txt`。
- 用户已允许短暂前置游戏做画面验证，每轮结束恢复并校验Mirasim为前台。用户转而要求深拆原流程后，先完成原业务规范，不再临时引导到猜测的页面。
- 固定解压交付 `dist/RelinkStudio/RelinkStudio.exe`，保留DLL、platforms、sqldrivers、vision/windows_ocr_worker.ps1、qt.conf；UI白灰不变，参数仍只保存，未接入点击购买。
- 继续时复用已写的采集/窗口/共享内存接口，不重新做PR12B；先按原流程实现页类与只读启动路径回放。

下文为PR12B历史交接记录，不覆盖以上新进展。

## PR12B 历史开发节点与 M2 非游戏基础层

续接日期：2026-10-07（Asia/Shanghai）。用户要求继续全部非游戏开发和验收，只在需要真实游戏画面验证识别业务时暂停总结。

- PR01–PR11 和 PR12A 已完成；PR12B 增加四个独立服务，历史列表先读摘要、选中运行才读详细事件/账本/审计。
- `src/ui/pages/`、`src/ui/dialogs/`、`src/diagnostics/` 保持页面、编辑器和离屏验收边界；`relink_ui` 不依赖 SQLite。
- `src/application/runtime/observation/` 实现步骤触发、有界内存帧、请求关联、新鲜度、取消和资源回收；来源仍为合成回放。
- `src/application/vision/` 已实现协议/lease，以及独立 `relink_vision_transport` 的 Win32 共享帧和隐藏合成子进程；fixture child 不是 OCR worker，不随桌面包发布，桌面程序未链接传输模块。
- 当前源码已有完整 CTest 19/19 记录；工作区 432、观察 828、协议 646、进程内链路 279、共享帧 65、隐藏合成进程传输 206 条断言通过；独立负向包验收记录为 41/41。最终发布、哈希与副本回退以本轮构建后更新的 `VERIFICATION.txt` 为准，不沿用旧包证据。
- 快照单独运行不得改写配置/工作区。`tests/verify_delivery.py` 参数化 build/release/output，使用新目录验证全部包文件清单、工作区字节保持和真实回退脚本。
- 优先读 `docs/business_rebuild/09_m1_progress.md`、`implementation/runtime/observation_adapter_pr12b.md`、`implementation/runtime/worker_protocol_pr12b.md`、`implementation/runtime/vision_transport_pr12b.md` 和 `NEXT_IMPLEMENTATION.md`；不重复重做 PR12A/PR12B。下一步需要真实游戏窗口/帧、ROI 与 OCR provider 校准，通用内存/进程传输已验证。
- 保存仍不等于启用：`enabled=false`、`activation_required=true`；真实画面识别不等于确认成交，更不自动接入购买。
- 构建发布：`build.ps1 -Test -Package`；发布验收：`python -I -X utf8 tests/release/verify_pr12b_package.py modified --build-dir build_relocated`。
- 固定交付为 `dist/RelinkStudio/RelinkStudio.exe`，保留 DLL、`platforms`、`sqldrivers`、`qt.conf`；只离屏验收，不弹出可见窗口。

## 当前 UI 版本：0.6 Windows 11 浅色 · 微软商店布局

用户喜欢微软商店 / Win11 的界面，2026-10-06 明确要求“不要蓝色”“按 Win11 同款白灰配色”（系统为浅色模式），取代 10-05 的深色要求。当前是 Win11 浅色主题、中性近黑强调色、商店式顶部搜索、左侧图标导航和左上圆角内容层。用户还提供了原程序“Relink枪皮助手”主界面截图，要求全部功能入口：已集中在“运行”页与“任务”页（含成色）。

- 更新后的程序仍在 `dist/RelinkStudio/RelinkStudio.exe`。
- 当前说明与原程序入口对照：`docs/STORE_UI.md`。
- UI 0.6 样式改版的历史验收与恢复记录：`artifacts/store_ui/VERIFICATION.txt`；0.5 备份在 `artifacts/store_ui/baseline/`。当前 PR12B 发布状态看本轮事务报告。
- 直接修改当前 `src/`，不重复执行一次性补丁脚本；不自动开启前台窗口，验收继续离屏。
- “优化”入口的具体功能待用户确认；运行参数只保存配置，不执行点击或购买。

这个目录是迁移后的工作根目录：

```text
C:\Users\Administrator\Desktop\price
```

## 先从这里继续

- 可运行前端：`dist\RelinkStudio\RelinkStudio.exe`
- 源码：`src\`
- Qt/CMake 工程：`CMakeLists.txt`
- 构建脚本：`build.ps1`
- 前端交付说明：`docs\FRONTEND_DELIVERY.md`
- 迁移校验：`artifacts\MIGRATION_VERIFICATION.txt`

## 构建和验证

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Package
```

迁移后的 `build.ps1` 会检测复制前留下的旧 CMake 缓存；如果 `build\CMakeCache.txt` 仍指向旧目录，会自动使用 `build_relocated\`，不会把新工程写回旧路径。

## 普通前端模式的功能边界

正常双击启动为前端和本地演示；显式 `--live-capture-check` 是独立只读诊断，不属于下列演示引擎：

- 不连接游戏进程。
- 不读取游戏内存。
- 不采集游戏画面。
- 不发送键盘或鼠标输入。
- 不执行收藏、购买或交易。
- 价格、皮肤、统计和运行结果都是演示数据。

后续接入市场页面时，继续从这个目录的 `src/` 和状态模型扩展即可。

## 迁移说明

原项目目录保留在：

```text
C:\Users\Administrator\Desktop\ida\relink_studio
```

原目录作为回滚副本保留；当前新会话使用 `C:\Users\Administrator\Desktop\price`。
