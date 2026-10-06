# PR12A · 页面职责与离屏验收入口分离

更新时间：2026-10-06（Asia/Shanghai）。

本票把部分页面、任务编辑器和离屏验收从主窗口/启动文件移出，先降低后续修改的耦合，再推进服务拆分和真实采集适配。它是原 PR12 交付验收前的第一步结构整理，不增加 OCR、市场连接或外部动作，也不改变 PR11 的方案、回放与账本语义。

**当前验收状态：PR12A 已完成源码冻结后的重新构建、14/14 CTest、发布目录离屏验收及独立副本回退复验。完整 UI 206 条、独立页面 90 条、工作区服务 400 条、工作区 UI 58 条和发布存储 22 条断言通过；对应命令退出 0。** 实际记录见 [实施进度](../09_m1_progress.md)、`artifacts/m1_pr12a_transaction/commands.json` 与最终 `VERIFICATION.txt`。本结论仅覆盖 PR12A，不表示 PR12B 或原 PR12 全项验收已完成。

继续使用 Qt Widgets 模块化单体；页面是编译期模块，不是独立进程、动态插件或额外服务。固定解压交付位置仍为：

~~~text
C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe
~~~

## 1. 按修改目标找入口

| 要修改什么 | 主要文件 | 责任边界 |
|---|---|---|
| 任务列表、选择、增删、启停演示任务 | [task_page.h](../../../src/ui/pages/task_page.h)、[task_page.cpp](../../../src/ui/pages/task_page.cpp) | 页面持有任务控件，通过借用的 `AppState*` 修改旧 Demo 任务 |
| 任务编辑表单、默认值、校验 | [task_editor_dialog.h](../../../src/ui/dialogs/task_editor_dialog.h)、[task_editor_dialog.cpp](../../../src/ui/dialogs/task_editor_dialog.cpp) | 对话框只读输入，维护本地 `Task` 草稿；接受后由页面提交 |
| 原运行参数与表单刷新 | [run_settings_page.h](../../../src/ui/pages/run_settings_page.h)、[run_settings_page.cpp](../../../src/ui/pages/run_settings_page.cpp) | 只绑定旧 `RunSettings`；保存、导入预览、导航、帮助发出信号 |
| 历史运行下拉与已提交记录表 | [workspace_records_panel.h](../../../src/ui/pages/workspace_records_panel.h)、[workspace_records_panel.cpp](../../../src/ui/pages/workspace_records_panel.cpp) | 只读投影渲染；选历史和导出发出信号 |
| 公共卡片、表格、按钮、展示文字 | [ui_helpers.h](../../../src/ui/presentation/ui_helpers.h)、[ui_helpers.cpp](../../../src/ui/presentation/ui_helpers.cpp) | 共享呈现函数，不持有应用服务或事务状态 |
| UI 使用的数据字段 | [workspace_projection.h](../../../src/application/workspace/workspace_projection.h) | 纯数据 `ProfileRow/ListingRow/RecordRow/RunRow/WorkspaceProjection`，不依赖 controller 或 SQL |
| 窗口导航与跨模块组合 | [mainwindow.h](../../../src/mainwindow.h)、[mainwindow.cpp](../../../src/mainwindow.cpp) | 创建页面、连接意图信号、执行跨页协调，不持有已提取页面内部控件的别名 |
| 命令行、主题、配置/工作区生命周期 | [main.cpp](../../../src/main.cpp) | 只做启动及验收入口组合，保留现有 CLI |
| 截图、文字、几何和交互自检 | [ui_self_test_runner.h](../../../src/diagnostics/ui_self_test_runner.h)、[ui_self_test_runner.cpp](../../../src/diagnostics/ui_self_test_runner.cpp) | 独立诊断入口，不解析命令行，不参与常规 UI 刷新 |
| 启动配置恢复路径 | [startup_config.h](../../../src/application/startup_config.h)、[startup_config.cpp](../../../src/application/startup_config.cpp) | 正常启动与损坏配置回归共用同一个函数，保留原配置并选择恢复文件名 |

`TaskPage` 和 `RunSettingsPage` 仍明确属于旧 Demo 配置系统，不是业务领域服务。记录面板则只接收工作区 DTO；将 DTO 从 controller 头文件移出并不表示 controller 的多责任问题已经解决。

### 编译依赖也是边界

| 目标 | 组成 | 直接依赖 |
|---|---|---|
| `relink_ui` 静态库 | 旧 `domain`、`widgets`、`fluenttheme`，公共 UI helpers，三种页面/面板和任务编辑器 | Qt Core/Gui/Widgets；Windows 下 `dwmapi`，不链接业务 controller、runtime 或 SQLite |
| `ui_module_tests` | 独立页面测试、共享启动配置函数、资源 | `relink_ui`；不编译 `MainWindow`，不连接工作区数据库 |
| `RelinkStudio` | 启动/主窗口、工作区和回放集成、包内自检 | `relink_ui` 以及现有业务/运行/工作区/存储库 |

依赖证据为 [CMakeLists.txt](../../../CMakeLists.txt)。`relink_ui` 仍包含旧 Demo 模型，因此不是与业务完全无关的通用组件库；它的价值在于页面可以离开 `MainWindow` 和 SQLite 单独构造、刷新和测试。

冻结源码实测 `main.cpp` 为 164 行，`mainwindow.cpp` 为 1912 行；对应提取前为 1037 / 3032 行。行数减少只反映职责移动，不作为运行内存、包体或性能改善的证据。

## 2. 对象归属与调用契约

### 页面拥有控件，主窗口拥有组合关系

`MainWindow` 创建页面，随后由布局、滚动容器和 `QStackedWidget` 的父子关系管理 QWidget 生命周期；主窗口保存页面指针用于组合，不手动删除其内部控件。`AppState` 和 `WorkspaceController` 仍在启动栈中先于主窗口构造，因此覆盖主窗口和页面的使用期；页面借用 `AppState*`，不负责销毁它。

| 模块 | 输入 / 输出 API | 刷新与副作用 |
|---|---|---|
| `TaskPage` | `TaskPage(AppState*, QWidget*)`；`refresh()`、`setSimulationAvailable(bool)`、`setCompact(bool)`、`editTask(id, skinId)` | 页面维护列表选中状态；任务编辑/删除/启停改变旧 `AppState` 并通知；`refresh()` 不充当业务执行入口 |
| `TaskEditorDialog` | `TaskEditorDialog(const AppState*, const Task*, skinId, QWidget*)`；`task()` 返回值 | 输入用于读取选项和初始任务；取消不写回；`QDialog::Accepted` 后页面取草稿并提交 |
| `RunSettingsPage` | `RunSettingsPage(AppState*, QWidget* workspacePanel, QWidget*)`；`refresh()`、`showSaveResult(message)` | 持有自身表单和 binder；用户编辑更新 `RunSettings`，保存等跨界操作转为信号 |
| `WorkspaceRecordsPanel` | `refresh(const WorkspaceProjection&, error)`；`showExportResult(path)` | 不保存投影引用，不访问 controller、数据库或导出文件；`showExportResult` 只显示已完成导出的路径 |

`RunSettingsPage` 的可选 `workspacePanel` 是主窗口构建并作为不透明子控件插入的工作区面板：控件物理上归页面父子树，方案选择、模式、回放按钮的业务绑定仍在主窗口。不要将这次拆分描述成整个运行控制页面已彻底独立。

信号的接收端明确如下：

| 发出者 | 信号 | 主窗口接收后的行为 |
|---|---|---|
| `RunSettingsPage` | `saveRequested()` | 走原配置保存路径，随后回传保存消息 |
| `RunSettingsPage` | `importPreviewRequested()` | 打开原只读导入预览 |
| `RunSettingsPage` | `navigateRequested(int)` | 使用原页序号导航，任务为 2、日志为 5 |
| `RunSettingsPage` | `helpRequested()` | 打开原帮助窗口 |
| `WorkspaceRecordsPanel` | `runSelected(runId)` | 调用工作区 `selectRun()`，再按已提交投影刷新 |
| `WorkspaceRecordsPanel` | `exportRequested()` | 主窗口取得文件名并调用 controller 导出；成功后显示结果 |

### 刷新不是用户命令

- 主窗口保留 `refreshAll()` / `refreshWorkspace()` 的协调点；页面不重复订阅同一状态而制造双重刷新。
- `TaskPage::refresh()` 使用自身刷新保护和表格信号阻断，按任务 ID 保留多选与当前行；新建行开关先赋值后连接用户信号。
- `RunSettingsPage` 的 binder 使用信号阻断，输入焦点保护仍按原行为保留；参数只进配置，不发送真实点击。
- `WorkspaceRecordsPanel::refresh()` 在重建下拉列表和设置选中项期间阻断信号，渲染不会反向触发 `runSelected()`。
- 中文展示文字和机器值分开：例如事件显示中文，`Qt::UserRole`、下拉 item data 和 tooltip 仍保留原身份、事件类型与来源。

### 自检生命周期与发布兼容

启动文件先解析参数并确定离屏平台、配置路径和输出目录，再调用：

~~~cpp
return relink::diagnostics::runUiSelfTest(app, window, state, workspace,
    {config, output, parser.isSet("self-test")});
~~~

这是 `main()` 已有组合位置的代码，不是额外后台线程。`UiSelfTestOptions` 只包含 `configPath`、`snapshotDirectory`、`runInteractionChecks`。Runner 调度同一 100 ms 定时验收并在自身调用期运行 `app.exec()`；回调对路径和布尔选项按值捕获，对应用、窗口、状态和工作区按引用捕获，其存活期覆盖调用。

几何/文字检查、截图与交互断言保持原顺序；`--snapshot-dir` 仍执行原几何/渲染流程，`--self-test` 额外执行交互，`--storage-self-test` 仍走原存储自检入口。损坏配置测试和正常启动继续调用同一个 `prepareStartupConfig()`。

**重型 UI 自检仍链接在发布 EXE 中。** 本票只分离代码责任，未实现独立诊断程序，也未据此宣称包体、内存或启动性能降低。后续拆出测试程序需要另行明确包内 smoke 验收与旧 CLI 兼容方案。

## 3. 修改与扩展时保持这些约定

| 后续工作 | 正确落点 | 应有回归 |
|---|---|---|
| 给任务表加一列或调整成色表单 | `TaskPage` / `TaskEditorDialog`，同时核对旧配置编解码 | 新建/编辑/取消、选择保留、保存/重开、紧凑尺寸 |
| 给现有运行参数增加展示或编辑 | `RunSettingsPage` 的控件与 binder；新增持久字段另改旧模型 | 默认值、修改/刷新、保存、焦点及启停依赖 |
| 给历史记录加展示字段 | 先明确 `WorkspaceProjection` 字段来源，再改记录面板 | 无数据/恢复状态、中文显示与机器值、只读、刷新不发命令 |
| 调整公共表格视觉 | `ui_helpers` / 既有 FluentTheme | 所有八页标准/紧凑尺寸、对比度、无蓝色、选中态和字体 |
| 修改方案保存、历史查询或 CSV 内容 | 工作区/存储服务，不向页面添加 SQL 或文件写入 | commit 失败、run 快照、revision、转义和受保护路径 |
| 引入后续采集/识别来源 | 完成 PR12B 和 PR12 后的观察适配器，不从 QWidget 事件直接执行采集 | 步骤触发、可取消、来源/时钟/新鲜度、内存图像路径 |

现有 objectName 是自动化兼容契约，保持 `taskTable`、`taskEnabled_<id>`、`runPurchaseDelay`、`workspaceRunCombo`、`workspaceRecordsTable` 等名字。页面不要拿 `MainWindow*` 再反查兄弟控件，也不要把共享全局状态藏进 `ui_helpers`。

## 4. 本票不改变的业务语义

1. 外观保持 Win11 / 微软应用商店浅色白灰、近黑强调、无蓝色；导航、顶部搜索、关注列表、条件编辑和价格图保留。
2. 原运行和任务参数仅随旧 Demo 配置保存；“优化”仍待定义，未引入真实输入、购买或交易。
3. 审定保存、选择、启用保持三种独立状态；审定后仍 `enabled=false`、`review_required=false`、`activation_required=true`。
4. 已保存方案仍只用首条禁用规则作解释性合成评估；内置八步 fixture 才产生模拟派发/回执，不是完整多规则执行。
5. 所有计数/记录来自已提交事实；commit 失败保留先前可见状态。历史 run 继续读取当时完整方案 JSON/revision，不读最新方案代替旧快照。
6. 恢复后可能已派发者继续 Unknown 并保留预留，不自动继续或重发；观察价与模拟确认价明确区分。
7. SQLite schema v2、ProfileStore/QSaveFile、只读工作区、dirty/active 保护和 CSV 转义/受保护路径保持原语义。
8. 业务图像路径仍未接入；后续采用步骤触发、内存像素、默认无截图/临时图像文件写入。显式离屏验收 PNG 属于测试产物。

完整业务依据见 [PR11 工作区](workspace_pr11.md)、[PR10 账本](sqlite_store_pr10.md) 与 [PR08 方案](profile_store_pr08.md)。

## 5. Evidence → Finding → Path

以下是源码边界证据，不代替二进制行为验收。命令均从项目根目录运行；`observed_at=2026-10-06`，`source_type=command`，`content_hash=n/a`，`artifact_path=n/a`，关联工作项为本票 PR12A。发布哈希与原始执行记录由本票独立事务工件保存。

| Evidence | 可复现命令 | 原始观察 / 摘要 |
|---|---|---|
| E-A01 | `Get-Content src/ui/pages/task_page.h -Encoding utf8; Get-Content src/ui/dialogs/task_editor_dialog.h -Encoding utf8` | 页面持有 `AppState*` 与任务表；编辑器构造输入为 `const AppState*`，`Task task() const` 返回草稿 |
| E-A02 | `Get-Content src/ui/pages/run_settings_page.h -Encoding utf8; Get-Content src/ui/pages/workspace_records_panel.h -Encoding utf8` | 参数页跨界操作发信号；记录面板仅接受 `const WorkspaceProjection&` 并发选择/导出意图 |
| E-A03 | `Get-Content src/diagnostics/ui_self_test_runner.h -Encoding utf8; Select-String -Path src/main.cpp -Pattern 'runUiSelfTest'` | `main` 委托独立 runner；选项值与调用期生命周期写在接口中 |
| E-A04 | `Get-Content src/application/workspace/workspace_projection.h -Encoding utf8; Select-String -Path src/ui/pages/workspace_records_panel.cpp -Pattern 'workspace_projection|QSignalBlocker'` | 投影为数据头；记录刷新阻断选择信号，不依赖 controller 头 |
| E-A05 | `Select-String -Path src/application/workspace/workspace_controller.cpp -Pattern 'committedRuns|eventsForRun|recoveryAudit|snapshot\('` | controller 刷新仍读取运行及其事件/账本/恢复审计；本票尚未优化历史读取 |

| Finding | Evidence | 结论与状态 | 后续 Path |
|---|---|---|---|
| F-A01 | E-A01、E-A02 | 已从源码确认控件归属和意图接口，不是将原大类分散到多个 cpp | P-A01：页面负责自身呈现，主窗口连接跨页意图，服务负责持久化 |
| F-A02 | E-A03 | 启动与自检源码已分离；包内重型诊断仍存在，不推出性能收益 | P-A02：CLI → runner options → 同线程定时验收 → 原 JSON/文本结果 → 退出码 |
| F-A03 | E-A04、E-A05 | 记录面板与 controller 依赖分离，但服务与历史全量读取问题仍存在 | P-A03：PR12B 提取窄查询/投影服务 → 实测多历史读取范围 → 原 PR12 完整验收 |

路径展开：

- **P-A01 / callflow**：用户编辑任务 → `TaskEditorDialog` 本地草稿 → Accepted 后 `TaskPage` 写回 `AppState` → 主窗口协调刷新；取消在草稿层结束。证据 E-A01，结论 F-A01。
- **P-A02 / callflow**：`main` 选择离屏启动 → 解析 `UiSelfTestOptions` → `runUiSelfTest` 调用同线程事件循环 → 原几何/文字/交互检查与 `ui_results.json` → 原 `UI_SELF_TEST` 和退出码。证据 E-A03，结论 F-A02。
- **P-A03 / callflow**：工作区已提交查询 → `WorkspaceProjection` → 主窗口传值视图 → `WorkspaceRecordsPanel::refresh` 只呈现；用户的 `runSelected` 才回到 controller。证据 E-A02/E-A04/E-A05，结论 F-A03。剩余事项是服务抽取与历史读取规模控制。

## 6. 验收、发布与回退

本票保留既有测试入口，新增 [ui_module_tests.cpp](../../../tests/ui/ui_module_tests.cpp) 并注册到 CTest。它覆盖表单默认值/修改、刷新不发命令、页面状态隔离、控件销毁、任务草稿接受/取消/校验、稳定 ID、记录只读显示、信号计数以及损坏配置保留。这个测试不构造 `MainWindow`、不链接 SQLite，与整窗口自检互补。

最终源码冻结后的第二次构建与发布复验已记录在 `artifacts/m1_pr12a_transaction/commands.json`：CTest 为 14/14；`UI_MODULE_TESTS=PASS; assertions=90; failures=0; mainwindow=false; sqlite=false; offscreen=true`；完整 UI 206 条、工作区服务 400 条、工作区 UI 58 条、发布存储 22 条断言通过。BASELINE / MODIFIED_BUILD / MODIFIED / ROLLBACK / RESTORED 均退出 0；回退后的原发布副本重新完成离屏验收，固定发布目录保留本票版本。这些数量来自本轮日志与 `snapshots/ui_results.json`，不从历史 PR11 成绩推定。

`visual_comparison.json` 记录 `tasks.png`、`task_editor.png`、`run.png`、`run_full.png`、`favorites.png`、`prices.png` 六张图与基线逐像素一致；`settings.png` 因隔离配置路径不同而不一致，未据此宣称整套 UI 逐像素一致。任务页、运行页与工作区记录另经图像目视检查。`architecture_checks.json` 同时记录 30 个业务/配置/账本/运行核心实现文件未改变、独立页面测试的直接 DLL 导入只有 QtCore/QtGui/QtWidgets 与 Windows `dwmapi`（不含 QtSql），以及冻结后的入口文件行数。

构建执行者统一使用同一个构建目录，避免并发写构建产物。

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test -Package
~~~

此命令供重现本票验收；实际输入/输出记录由事务日志承载。发布验收须使用 package-only PATH 与离屏平台实际调用固定目录程序，检查 UI、工作区、存储、兼容性及恢复。`build.ps1` 的包内存储 smoke 已改用 `Start-Process -Wait -PassThru -WindowStyle Hidden` 等待 GUI 子系统进程并检查其 `ExitCode`，不再用可能来自上一命令的 `$LASTEXITCODE` 判定；这不是新增可见窗口。

事务证据目录为 `artifacts/m1_pr12a_transaction`；验收记录需要包含实际命令、输入、stdout/stderr、退出状态、原始/发布哈希，以及四项工件 `MODIFIED_FILE`、`DIFF_FILE`、`VERIFICATION.txt`、`ROLLBACK.sh` 的真实路径。回退必须在另一解压副本运行，保留新方案和数据库；固定交付目录留在本票版本。源码结构检查、文档校验、应用 CTest、发布离屏验收和回退是不同层，不互相替代。

## 7. 下一阶段：PR12B，再完整 PR12

本票仍保留以下集中逻辑：主窗口的其它页面、导入/审定对话框、工作区方案/回放面板及旧兼容回放分支；`WorkspaceController` 仍承担方案扫描/保存、内置回放、历史查询、投影与 CSV 导出。共享 `ui_helpers` 也不等于领域层；`AppState` 与工作区 DTO 的双状态系统仍需要明确适配边界。

PR12B 优先细分服务责任和历史读路径：列表先查摘要，选中 run 才读取详情，并用实际临时数据库验证分页/增量边界。已有 `IEventStore` 写接口不包含所有历史查询，不能仅替换成员类型就宣称存储解耦。保持 SQLite 连接线程归属与提交后投影规则，不顺手接 OCR 或全规则执行。

可直接复用的下一阶段提示词和原 PR12 全量验收清单见 [NEXT_IMPLEMENTATION.md](../NEXT_IMPLEMENTATION.md)。完整 PR12 验收通过以后再进入 M2 的步骤触发、纯内存采集与识别适配器；本票的结构提取并不提前完成那些功能。
