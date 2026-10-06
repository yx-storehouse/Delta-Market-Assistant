# Relink Studio — new session entry

## 当前开发节点：PR11 持久化方案与回放界面已接入

- 当前仓库为 `yx-storehouse/Delta-Market-Assistant`，项目名为三角洲市场助手；构建目标/文件名保留 `RelinkStudio`。
- 先读 `docs/business_rebuild/09_m1_progress.md` 和 `NEXT_IMPLEMENTATION.md`；下一阶段为 PR12，不重复实施 PR07–PR11。
- `src/application/workspace/workspace_controller.*` 连接独立 ProfileStore/QSaveFile 和 SQLite。正常程序创建 `AppLocalDataLocation/business` 工作区；离屏自测使用独立临时工作区。
- 运行页支持方案选择/审定另存，工作台提供持久化回放单步与推进，日志和统计显示已提交事实；历史恢复只读、未知预留保留、不自动重发。内置八步样例才产生模拟账本，保存方案仍未启用。
- PR10 修正未知/歧义回执被计为成功的问题，并修复历史 UI 中文乱码；配色、布局和功能入口不变。
- 构建验收入口仍为 `build.ps1 -Test -Package`；发布程序增加 `--storage-self-test`，只操作临时数据库，不打开前台窗口。
- 本轮完整证据：`artifacts/m1_pr11_transaction/VERIFICATION.txt`。旧解压程序保留在该目录下 `baseline/release/`；回退脚本恢复程序文件并保留新数据库。

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
