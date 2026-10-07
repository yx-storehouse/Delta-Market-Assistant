# Delta Market Assistant · 三角洲市场助手

## 当前交付：真实皮肤目录与赛季追加（2026-10-07）

正常启动内置用户提供并核对的 **S1–S11 共 149 项皮肤**，不再装载旧默认演示商品、任务、价格和运行结果。保留原商品 ID、赛季、武器、皮肤系列、极品 / 优品标记和菜单颜色。正式游戏品质尚未核实，不按颜色自动推断。缩略图暂时留空；无市场观测时，报价、磨损和历史曲线保持空白。

关注页点击 **“皮肤资料 / 追加赛季”** 可查看全部资料、添加同季皮肤、新建赛季、批量导入新增 JSON、导出或下载空白模板。新增数据独立保存在配置同级 `catalog_extensions.json`，升级程序不覆盖。重复 ID、格式错误或与现有配置冲突时整次拒绝，原数据保留。详见 [目录使用说明](docs/REAL_SKIN_CATALOG.md)。

旧配置在目录迁移前备份。已证实的默认模拟条目被移除；用户自定义任务条件与关注保留，不用截图中另一份方案的价格覆盖真实 `.savedValue` 参数。目录是固定商品资料，不是当前游戏账户的关注清单。

**实机业务仍以原流程为准：** 上轮已实际添加 25 条关注，暂停在 P90 天命 B 第 1 页的滚动/半行关联点；本轮没有操作游戏。接续开发读 `SESSION_START.md` 和 `artifacts/m2_savedvalue_collection/current_checkpoint.json`，不要重放已确认星标。正常 UI 的任务配置不代表完整游戏执行器已接通。

本轮 32 组 CTest、真实目录离屏追加/重启、实际旧配置副本迁移、发布包独立依赖和隔离回滚的最终证据在 `artifacts/real_skin_catalog/VERIFICATION.txt`。测试用合成数据仅保留在显式诊断入口；正常应用不显示模拟运行或合成账本入口。

## 当前版本：0.6 Windows 11 浅色 · 微软商店布局

按用户要求改为 Win11 同款白灰配色（用户系统为浅色模式），不使用蓝色：Mica 浅灰底、略亮的内容层、白色卡片、细灰边框，强调色为中性近黑。窗口采用微软商店结构：顶部居中搜索、左侧图标导航栏、左上圆角内容层；控件、开关、下拉和弹窗按 WinUI 3 样式绘制，图标使用 Segoe Fluent Icons。

用户提供的原程序“Relink枪皮助手”主界面的全部功能入口已收进新的“运行”页和“任务”页：方案、运行快捷键、定时、购买延迟（含动态延迟、队列已满减延迟、公示期加延迟）、连点与间隔、品级限购、挂机三项、自动收藏与 OSD、收藏任务（新增成色）、优化、记录和帮助。参数随配置保存、导入、导出；前端不执行点击或购买。

直接打开 `C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`；不需要再次解压。详见 `docs/STORE_UI.md`。关注列表、右侧条件编辑和底部价格图保留。

这是 **C++17 + Qt 6.8.3 Widgets** 编写的独立 Windows 桌面前端。
当前版本使用真实静态皮肤目录维护本地任务条件。未知市场数据留空，观察与执行模块独立于目录和界面。

## 打开程序

打开交付目录中的 `RelinkStudio.exe`。请保留同目录的 DLL、`platforms`、`sqldrivers` 和 `qt.conf`，不要只移动 EXE。
本次开发和验证只使用 Qt `offscreen` 离屏模式，没有为测试启动可见窗口。

## 已完成的页面

| 页面 | 已实现的交互 |
|---|---|
| 工作台 | 本地关注与任务摘要、价格空状态、最近日志 |
| 我的关注 | 149 项目录、搜索与类型/菜单颜色筛选、本地关注、条件任务创建、目录及赛季维护 |
| 自动任务 | 新增、编辑、成色、范围校验、确认删除、多选启用/暂停、数量上限 |
| 运行设置 | 原程序全部参数：方案、快捷键、定时、延迟、连点、品级限购、挂机、自动收藏、优化/记录/帮助入口 |
| 价格中心 | 皮肤切换、已知报价、无采样时空图、CSV 导出 |
| 运行统计 | 无实际记录时保持空白，不生成模拟成交或支出 |
| 运行日志 | 级别与关键字筛选、最新事件定位、确认清空 |
| 设置 | 紧凑列表、日志自动定位、本地配置保存、运行模式与版本信息 |

正常启动不会开启模拟引擎。目录皮肤、用户配置与实际游戏关注属于不同数据，不将本地星标伪称为游戏已关注回执。

## 配置与导出

- 默认配置保存于应用本地数据目录，确切路径可在“设置”页查看。
- 界面关闭时保存当前任务和本地关注配置。
- 右上角“导入”保留配置只读预览；追加目录使用目录弹窗中的独立导入入口，不覆盖现有配置。
- JSON 配置包含 `schema_version: 1`、`demo: false`、`skins`、`tasks` 和 `run_settings`；任务可带 `condition`（缺省“不限”）。旧文件缺少新字段时按默认值载入。
- 当前本地任务编辑的数量范围是 1–9999；原 `.savedValue` 的 0 不限值仍在其业务输入中保留，本轮不改该执行参数。
- CSV 保留数据来源与导出时间；未知数值为空，不编造价格或采集时间。

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
src/catalog/         真实目录、扩展合并、提交验证与原子保存
src/application/catalog_*  目录投影、旧配置迁移、启动备份
src/domain.*          配置模型、运行参数、可用性字段、配置校验与 CSV
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
