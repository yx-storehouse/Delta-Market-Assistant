# Relink Studio — new session entry

## 当前开发节点：PR12A 页面模块化已完成

- PR01–PR11 的领域、方案、持久化回放与账本功能保持；PR12A 是原 PR12 总体验收前的第一轮维护性整理。
- `src/ui/pages/` 独立拥有任务页、运行参数页和记录面板；`src/ui/dialogs/` 独立任务编辑器；共享呈现工具在 `src/ui/presentation/`。
- `main.cpp` 为 164 行启动入口；完整离屏验收搬到 `src/diagnostics/ui_self_test_runner.*`，仍链接进 EXE 保持 CLI 兼容，不宣称包体缩小。
- `relink_ui` 只链接 Qt Core/Gui/Widgets 与 Windows DWM；独立 `ui_module_tests` 不依赖 MainWindow/WorkspaceController/SQLite。
- 阅读 `docs/business_rebuild/implementation/ui_modularization_pr12a.md`、`09_m1_progress.md` 和 `NEXT_IMPLEMENTATION.md`。下一步 PR12B 服务/历史查询整理，随后才是原 PR12 总体验收和 M2。
- 本轮 14/14 CTest、90 条独立模块断言、400 条 workspace 服务断言、58 条 workspace UI 断言、22 条存储自检通过。
- 构建发布：`build.ps1 -Test -Package`；发布验收：`python -I -X utf8 tests/release/verify_pr12a_package.py modified`。
- 证据 `artifacts/m1_pr12a_transaction/VERIFICATION.txt`；旧程序在其 `baseline/release/`，回退在独立副本验证并保留新账本/方案。
- 页面构造后由宿主显式调用 `refresh()`；旧 Demo 与新业务仍分开，新功能不堆入旧页面。

## 当前 UI 版本：0.6 Windows 11 浅色 · 微软商店布局

用户喜欢微软商店 / Win11 的界面，2026-10-06 明确要求“不要蓝色”“按 Win11 同款白灰配色”（系统为浅色模式），取代 10-05 的深色要求。当前是 Win11 浅色主题、中性近黑强调色、商店式顶部搜索、左侧图标导航和左上圆角内容层。用户还提供了原程序“Relink枪皮助手”主界面截图，要求全部功能入口：已集中在“运行”页与“任务”页（含成色）。

- 更新后的程序仍在 `dist/RelinkStudio/RelinkStudio.exe`。
- 当前说明与原程序入口对照：`docs/STORE_UI.md`。
- 本轮验收与恢复记录：`artifacts/store_ui/VERIFICATION.txt`；0.5 备份在 `artifacts/store_ui/baseline/`。
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

## 当前功能边界

当前版本是前端和本地演示数据：

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
