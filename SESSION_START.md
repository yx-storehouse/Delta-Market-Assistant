# Relink Studio — new session entry

## 最新功能：收藏任务接入程序（2026-10-07）

- 任务页新增导入真实 `.savedValue` 的入口，全局导入也识别该类型；严格按真实目录和原字典关联，逐行预览，确认后原子保存再发布到 AppState。数量 0 在编辑、列表、配置往返统一表示不限；稳定来源去重保留导入后的编辑，不覆盖原关注。
- 本机正式 config.json 已通过新入口实际导入，重开校验为149皮肤/50任务/21启用；重复导入零新增且配置哈希不变。导入前原配置已备份，未启动任何任务。
- 真实输入核实为 50 行 / 21 启用 / 13 全部商品 / 9 启用商品。总商品 13 包含停用10100、10702、10703、10704，之前猜测10仅为新增测试预期错误，已独立核对原始字节纠正。
- 源码入口 `application/collection_task_import.*`、`collection_task_commit.*`、`ui/dialogs/collection_import_dialog.*`。GUI保存失败保留预览；CLI `--import-collection-tasks <file> --config <explicit file>` 为同一数据提交，输入失败不覆盖配置。测试全部隔离，真实来源不改。
- 新 `--collection-import-self-test` 使用真实主窗导入入口及持久化链路；`--task-source` 可只读指定实际原文件。程序路径不变，详细说明 `docs/COLLECTION_TASK_IMPORT.md`；本轮事务 `artifacts/collection_workflow/VERIFICATION.txt`。
- **当前游戏现场与旧断点不同：** 本轮最初观察为大厅；新批次沿大厅→曼德尔砖→典藏→我的关注，现页显示空关注。原25次已确认收藏是历史事实，不能当作当前在售列表；未清空/取消/添加关注，未购买。当前页不能使用旧P90 B第六张坐标。批次结束已回IDE。
- 新 `tests/manual/collection_scroll.py` 只落实同帧布局/半行拒绝/滚动位移证明/对账契约和离线测试，C++动态边界采集与runner动作尚未接通。别把41项离线回归或原固定六卡几何当作已实测滚动支持。

## 最新开发节点：真实皮肤目录接入（2026-10-07）

- 本轮请求为替换全部模拟皮肤、暂留空缩略图、支持以后追加新赛季。用户已允许最大数量子代理，本轮 3 个子代理并行完成数据层、目录 UI 与真实数据默认行为。
- 内置用户核对资料 S1–S11 共 149 项；来源 `docs/business_rebuild/skin_reference/skin_reference.json`，运行数据 `src/assets/catalog/skins.json`。保留原商品 ID、武器、皮肤系列、极品/优品、菜单颜色；正式游戏品质未知，不按颜色猜测。没有扩充超出用户模板的后续游戏目录。
- 正常启动使用真实目录，清除严格识别的旧默认模拟商品/任务，旧配置迁移前做逐字节备份；保留真实用户业务条件及非模板条目。无报价/磨损/涨跌时显示未知，不生成曲线、成交或统计。诊断 fixture 仅由显式 `--self-test` 等测试入口加载。
- 关注页的“皮肤资料 / 追加赛季”进入查看/搜索、单条追加、JSON 批量追加与模板导出。用户扩展保存到配置同级 `catalog_extensions.json`；冲突/重复/损坏/提交验证失败均不部分写入。新商品可直接用于任务配置，扩展独立于程序升级目录。
- 使用说明 `docs/REAL_SKIN_CATALOG.md`；新增 `--catalog-self-test` 走同一正常启动路径，使用临时目录离屏验证完整追加与迁移链路。交付和回滚证据 `artifacts/real_skin_catalog/VERIFICATION.txt`，程序仍为固定解压路径 `dist/RelinkStudio/RelinkStudio.exe`。
- 本轮未运行原 BBZPS/Relink 程序、未启动可见窗口、未操作游戏。此前 25 条实际关注及 P90 天命 B 的滚动暂停点不变，继续收藏前仍读下方原 checkpoint；不要重放已完成星标。
- `.savedValue` 真实任务文件的执行参数没有被目录截图或本轮目录迁移覆盖。任务目录与实际游戏关注账本是独立数据；当前 UI 不冒充已同步那 25 条实际游戏关注。

## 最新问题：皮肤品质资料核对（2026-10-07）

用户给出 S1–S11 共 11 张原助手菜单截图，追问模板中的皮肤品质是否已记录。核查确认：任务成色/价格/磨损/限量与商品 ID 已按 savedValue 记录，但原提取器没有保留颜色/独立品质字段；`grade=any` 是筛选不限，不是皮肤本身的品质。现在已整理 `docs/business_rebuild/skin_reference/皮肤基础表.md` 及 `skin_reference.json`，149 项的菜单颜色、极品/优品后缀与来源图分开保存，原有商品 ID 不变。14 红、14 橙、33 紫、88 蓝；当前启用的 9 款均属截图紫色项。正式游戏品质名称映射仍待确认，参考资料尚未接入生产任务快照或收藏校验；不要声称已修好品质筛选，也不要把截图中的 1003 方案价格覆盖 0925 任务文件。此轮没有启动游戏操作，之前 25 条关注及滚动暂停点保持不变。

## 当前实机节点：任务文件已实际收藏 25 单，滚动关联待补（2026-10-07）

- 用户已提供 `C:\Users\Administrator\Desktop\S11新赛季0925.savedValue` 并要求先实际收藏。下文所有“尚无真实任务/等待指定商品/只读观察”的旧安排均已过期。收藏本身包含加关注；购买在后续我的关注业务阶段，本轮未进入。
- 先读 `docs/business_rebuild/implementation/runtime/savedvalue_collection_m2.md`、`artifacts/m2_savedvalue_collection/current_checkpoint.json` 和 `session_audit.json`。原数据 50 行、21 行启用、9 个商品；原始文件及解析快照在本地忽略的 `input/` 目录，不是演示数据。
- 已确认新增 25 单：AUG 天命 10 单（S 3/A 2/B 3/C 2），P90 天命 15 单（S 4/A 5/B 6）。所有 pending 已对账，原有关注保留；**不要重跑首条添加或任何已确认条目的星标点击**。
- 当前游戏在 P90 天命、成色 B、第 1 页；卡片索引 5（第三行右侧）已收藏。六张完全可见卡片都是 230，仍低于该规则 280；底部还有半行，现有几何尚未覆盖滚动后的字段关联。最后系列 `live_series_s6_v5` 停于 `COLLECTION_UNSCANNED_SCROLL_REGION`，不是整份任务完成。下一步先补滚动/半行的定位与同帧关联，再从源行 6 接续；不要直接切到成色 C 或下一个商品。
- 最后一个批次已恢复 Mirasim 前台。游戏图片未写磁盘；窗口身份每次重新解析；批次内连续操作，不逐动作切屏。严禁运行原 BBZPS/Relink 样本或历史探针。
- 发布仍在固定解压目录 `dist/RelinkStudio`，白灰 UI 不变。28 组 CTest 和收藏辅助测试以本轮 `VERIFICATION.txt` 精确记录为准。原生大标题 P90 的数字识别仍有缺口：本轮仅人工核对截图批准过一次导航，收藏前始终复核完整卖单标题，不能假称大标题已自动识别成功。
- `journal/` 先持久化再点击，记录白星到金星及提示回执。首条 AUG、AUG B 和 P90 A 各有一次旧自动回读失败，已另写 reconciliation；旧失败不覆盖、不改标。回滚只验证程序包副本，不撤销游戏收藏。

## 最新沟通要求（2026-10-07）

用户要求持续推进，不逐阶段暂停，不输出例行进度、改动、测试或思考总结。详细证据留在文件中。只在真实阻塞、业务逻辑冲突或需要用户决策时简短说明问题；不要用一个普通里程碑结束开发。

## 当前：总筛选状态已实测，真实任务目标待指定（2026-10-07）

- 入口 `docs/business_rebuild/implementation/runtime/catalog_filter_state_m2.md`。`CatalogFilterReader` 已读取实际赛季及六个三态框；选中样式为白色内嵌方块。普通品阶选中时需要独立文字区同帧3倍OCR，未替换错字。
- 实测已覆盖六个框的选中/未选以及品阶多选；最终发布包完成17步连续批次，前置/回IDE各一次。筛选未确认，赛季/六个框已全部恢复。失败、恢复过程均保留，无购买、库存移动或关注更改。当前游戏仍在筛选页。
- 独立 `CatalogFilterPlan` 处理 S10–S14，先比赛季，再拥有，再品阶残留清理/重选；只产出差异与回读要求，不执行点击。不要从示例任务臆造真实分组。
- 27组CTest、筛选153断言、84组实测数值信号回放；6张UI与基线逐像素一致，正式解压目录已更新并在隔离副本验证回退。事务 `artifacts/m2_catalog_filter/VERIFICATION.txt`。
- 本机 `LocalAppData/RelinkStudio/RelinkStudio/config.json` 仍是 `schema_version=1, demo=true`，5条任务全部禁用。按真实任务继续S15查找及S17标题复核之前，需要用户指定测试枪皮及其赛季/成色；不把历史/演示配置当成当前执行目标。获得目标后继续，不重复校准现有六个框。
- 原StartupObserver默认1秒时效未动；同帧多ROI诊断尚未接进真实异步运行时。当前状态识别不等于完整自动运行按钮已接通。
- 批次上限现在20步，总时限仍30秒。字段断言失败不重放点击；失败后重新观察再恢复临时状态，最终恢复IDE。用户切走时不得抢回继续操作。

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
