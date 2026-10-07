# Delta Market Assistant · 三角洲市场助手

## 当前开发进度：PR12B 服务整理与 M2 非游戏基础层

项目保留 `RelinkStudio` 构建目标和可执行文件名。PR01–PR11 已接入领域规则、只读导入预览、审定方案保存、持久化合成回放、已提交账本与历史记录。运行页可选择审定方案或内置回放样例；工作台提供回放单步/快速推进，日志页查看并导出历史事实。已保存方案持续未启用，只有显式选择的内置合成样例产生模拟账本，不连接真实市场。

开发入口：`docs/business_rebuild/09_m1_progress.md`、`docs/business_rebuild/implementation/workspace_pr11.md`、`docs/business_rebuild/NEXT_IMPLEMENTATION.md`。正常业务数据位于应用本地数据目录的 `business` 子目录；`--workspace-dir` 可指定独立目录，`--workspace-read-only` 用于只读查看。界面显示中文状态，完整原始 ID 和原因码保留在提示中。

PR12A 的独立 UI 页面和验收模块保持；这轮完成 `ProfileCatalog`、`ReplayScenario`、`HistoryQueryService`、`RecordsExporter` 的服务职责分离。历史列表先读摘要、选中运行才读明细；UI、领域、运行时、SQLite 仍为轻量模块化单体，不引入后台服务或通用插件框架。

图像基础层已加入步骤触发的内存观察、worker 协议/双槽 lease、真实 Win32 共享内存及隐藏合成子进程传输。纯状态层 `relink_runtime` 与操作系统传输层 `relink_vision_transport` 分离；后者由独立测试目标验证，没有接入桌面程序。测试子程序不是 OCR worker，不随发布包交付。产品仍不读取游戏画面；`--snapshot-dir` 单独使用只作离屏呈现，交互回放仅由 `--self-test` 启动。

本轮完整 CTest 已通过 19/19：工作区 432、UI 模块 90、观察适配器 828、worker 协议 646、进程内图像链路 279、Win32 共享帧 65、隐藏合成 worker 传输 206 条断言通过；UI / 工作区 UI / 存储自检分别为 206 / 58 / 22 项，独立负向包验收为 41/41。最终发布、包内复验及回退状态以 `artifacts/m1_pr12b_transaction/VERIFICATION.txt` 和 `artifacts/m1_pr12b_delivery/VERIFICATION.txt` 中与本轮构建哈希对应的命令为准，不沿用旧包结果。接口见 [内存观察](docs/business_rebuild/implementation/runtime/observation_adapter_pr12b.md)、[协议与 lease](docs/business_rebuild/implementation/runtime/worker_protocol_pr12b.md) 和 [真实内存/进程传输](docs/business_rebuild/implementation/runtime/vision_transport_pr12b.md)。下一步是真实游戏窗口、ROI 和 OCR provider 校准；现有测试不代表真实识别已接通。

## 当前版本：0.6 Windows 11 浅色 · 微软商店布局

按用户要求改为 Win11 同款白灰配色（用户系统为浅色模式），不使用蓝色：Mica 浅灰底、略亮的内容层、白色卡片、细灰边框，强调色为中性近黑。窗口采用微软商店结构：顶部居中搜索、左侧图标导航栏、左上圆角内容层；控件、开关、下拉和弹窗按 WinUI 3 样式绘制，图标使用 Segoe Fluent Icons。

用户提供的原程序“Relink枪皮助手”主界面的全部功能入口已收进新的“运行”页和“任务”页：方案、运行快捷键、定时、购买延迟（含动态延迟、队列已满减延迟、公示期加延迟）、连点与间隔、品级限购、挂机三项、自动收藏与 OSD、收藏任务（新增成色）、优化、记录和帮助。参数随配置保存、导入、导出；前端不执行点击或购买。

直接打开 `C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`；不需要再次解压。详见 `docs/STORE_UI.md`。关注列表、右侧条件编辑和底部价格图保留。

这是 **C++17 + Qt 6.8.3 Widgets** 编写的独立 Windows 桌面前端。
当前版本验证界面与本地工作流，所有皮肤名称组合、价格、趋势和运行结果均为合成演示数据。

## 打开程序

打开交付目录中的 `RelinkStudio.exe`。请保留同目录的 DLL、`platforms`、`sqldrivers` 和 `qt.conf`，不要只移动 EXE。
本次开发和验证只使用 Qt `offscreen` 离屏模式，没有为测试启动可见窗口。

## 已完成的页面

| 页面 | 已实现的交互 |
|---|---|
| 工作台 | 指标、模拟开始/暂停、任务摘要、趋势与最近日志 |
| 我的关注 | 顶部搜索、类型/关注筛选、品级与任务筛选、星标关注、查看合成价格、按成色创建任务 |
| 自动任务 | 新增、编辑、成色、范围校验、确认删除、多选启用/暂停、数量上限 |
| 运行设置 | 原程序全部参数：方案、快捷键、定时、延迟、连点、品级限购、挂机、自动收藏、优化/记录/帮助入口 |
| 价格中心 | 皮肤切换、合成趋势、样本表、CSV 导出 |
| 运行统计 | 模拟扫描、命中、确认计数及结果分布；实际成交与支出留空 |
| 运行日志 | 级别与关键字筛选、最新事件定位、确认清空 |
| 设置 | 紧凑列表、日志自动定位、本地配置保存、运行模式与版本信息 |

演示引擎只推进本地内存状态，不连接游戏、不采集屏幕、不注入键鼠、不产生订单。
“我的关注”中的 12 条皮肤样本不是从当前游戏账户读取的关注记录。
价格单位、真实磨损语义和市场卖单字段尚待实际数据模块确认，演示数据不作为真实交易依据。

## 配置与导出

- 默认配置保存于应用本地数据目录，确切路径可在“设置”页查看。
- 界面关闭时保存当前任务和关注配置；模拟执行结果不恢复为真实运行结果。
- “导入配置”校验完整文件后才替换当前数据；导入源文件不被退出保存覆盖。
- JSON 配置包含 `schema_version: 1`、`demo: true`、`skins`、`tasks` 和 `run_settings`；任务可带 `condition`（缺省“不限”）。旧文件缺少新字段时按默认值载入。
- 数量上限从 1 开始；0 在原程序中的含义未确认，本版不自行解释为无限。
- CSV 明示 `DEMO_SYNTHETIC` 与 `exported_at`，不是实际行情采集时间。

## 工程结构

```text
src/main.cpp          启动参数、主题与配置/工作区生命周期
src/mainwindow.*      商店式外壳、页面组合、剩余页面与跨页协调
src/ui/pages/         任务页、运行参数页、工作区记录面板（各自拥有控件）
src/ui/dialogs/       本地任务编辑草稿与校验
src/ui/presentation/  共享表格、卡片、按钮与显示文字
src/diagnostics/      原有离屏几何/文字/交互验收 runner
src/application/startup_config.*  启动配置恢复的共享入口
src/application/workspace/       工作区协调、方案目录、回放、历史查询和导出服务
src/application/runtime/observation/ 步骤触发、有界合成内存帧与识别接口
src/application/vision/          协议/lease 状态；独立 Win32 共享帧/隐藏进程传输层
src/domain.*          演示数据、运行参数、事务配置校验、模拟状态与 CSV
src/widgets.*         导航项、筛选胶囊、开关、星标、下拉/数字框、指标、价格曲线
src/theme.qss         Win11 浅色公共样式
src/fluenttheme.*     WinUI 浅色色值、字体、Fluent 图标、应用图标、标题栏主题请求
src/app.rc            EXE 文件图标
tests/domain_tests.cpp 数据层测试
tests/ui/             独立页面模块测试，无 MainWindow/SQLite
build.ps1             构建、测试和打包（不启动可见窗口）
artifacts/            离屏页面图与校验记录
```

## 构建与离屏验收

当前工程使用隔离安装在 `.tools` 的 Qt 6.8.3、MinGW 13.1、CMake 和 Ninja。

```powershell
powershell -NoProfile -File .\build.ps1 -Test -Package
```

安装相同的独立工具链可使用 aqtinstall（项目内虚拟环境）：

```powershell
python -m venv .tools\aqt-env
.\.tools\aqt-env\Scripts\python.exe -m pip install aqtinstall==3.3.0 cmake ninja
.\.tools\aqt-env\Scripts\python.exe -m aqt install-qt windows desktop 6.8.3 win64_mingw --archives qtbase qttranslations -O .tools\Qt
.\.tools\aqt-env\Scripts\python.exe -m aqt install-tool windows desktop tools_mingw1310 qt.tools.win64_mingw1310 -O .tools\Qt
```

程序提供以下命令行入口。所有这些入口会强制使用离屏渲染，不占用游戏前台：

```powershell
.\RelinkStudio.exe --self-test --snapshot-dir .\screenshots --config .\test-config.json
.\RelinkStudio.exe --write-demo-config .\demo-config.json
.\RelinkStudio.exe --validate-config .\demo-config.json
```

GUI 程序从某些 PowerShell 会话启动时可能不等待退出；自动化验收使用 `subprocess.run` 或 CTest 等待真实退出码。

## 后续接入点

1. 页面观察适配：市场 / F4 → 典藏外观 → 我的关注。
2. 同帧识别皮肤、成色、磨损、卖单价格及状态。
3. 行情持久化与真实任务状态机。
4. 操作回执和结果确认，再接真实统计。

SQLite/QSQLITE 已接入工作区持久化、恢复、历史与账本，并通过发布包驱动验证。独立传输模块已验证 Win32 共享内存和隐藏合成子进程，但桌面产品仍未接入 WGC/DXGI 游戏窗口来源、OpenCV/ONNX OCR provider、真实 ROI 校准或交易。传输测试传递的是合成像素，不保存原始图片或临时截图。

## 依赖说明

发行目录包含动态链接的 Qt 与 MinGW 运行库，依赖许可文本随发行目录提供。
系统中文字体仅在本机离屏测试时从 Windows 字体目录读取，没有复制进发行包。
