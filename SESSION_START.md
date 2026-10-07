# Relink Studio — new session entry

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
