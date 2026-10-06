# 工程基线与第一阶段落点

> 2026-10-06 只读核对；本轮未构建/运行应用。文件存在、CMake缓存与版本资源不是“新功能已编译通过”。E08/R10/R12/R14/R15。

## 实际现状

| 对象 | 只读核对结果 | 开发影响 |
|---|---|---|
| `CMakeLists.txt` | CMake ≥3.22；项目0.6.0；C++17；Qt6 ≥6.8，Core/Gui/Widgets；WIN32链接dwmapi | 新领域库先只依赖QtCore；保留现有 `RelinkStudio` 和 `domain_tests` |
| `build.ps1` | `.tools/Qt/6.8.3/mingw_64`；`mingw1310_64`；Ninja；Release；构建并行6 | 实际选用本地SDK，不假设系统PATH或MSVC依赖可用 |
| Qt版本资源 | `Qt6Core.dll` ProductVersion = `6.8.3.0` | 文献R08–R15解释6.8接口，不代表本机已更新补丁 |
| 既有编译器缓存 | GNU `13.1.0`；C++17 | 第三方C++库的ABI需实核；M1不引入视觉依赖 |
| `build/CMakeCache.txt` | home/Qt/Ninja仍指向 `C:/Users/Administrator/Desktop/ida/relink_studio` | 不将旧build下二进制当本轮新构建 |
| `build_relocated/CMakeCache.txt` | home/Qt/Ninja指向当前 `C:/Users/Administrator/Desktop/price` | 当前 `build.ps1` 会使用此目录；后续证据记录实际buildDir |
| 现有CTest | `domain_tests`、`ui_offscreen`；后者设置 `QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1` | 扩展标签/目标并保留原回归；禁止仅运行新增测试 |
| `src/domain.h` | Skin/Task数字是double，AppState模拟和日志；RunSettings仅保存参数 | 新Decimal与真实观测分层，不静默重解释旧演示数据 |
| `tests/domain_tests.cpp` | 包含原子保存、无效导入不变、schema/demo、run参数、演示/成色/数量、日志容量等断言 | 只能证明演示v1能力；没有新Domain、OCR、SQLite和实时采集测试 |
| `tests/verify_delivery.py` | 固定执行 `build/domain_tests.exe`；内部说明仍有旧断言数量 | PR12将buildDir参数化/读取构建证据；不再把旧缓存测试当新产物；数量从结果实际统计 |
| QtSql SDK | `bin/Qt6Sql.dll` 与 `plugins/sqldrivers/qsqlite.dll`存在 | 采用QtSql时需要新增find/link/deploy和运行时driver检查；存在不等于已接入 |
| 当前发布目录 | 上述QtSql及qsqlite部署项未见；现有UI所需DLL/platforms保留 | PR10/12新增依赖必须入manifest；不要仅复制EXE |

## 建议新增目录与依赖方向

以下均是下一轮代码计划，不是本轮创建的实现：

```text
src/business/       decimal, models, decision, rule_matcher（无GUI/捕获/系统输入）
src/replay/         replay_reader, fake_clock, replay_engine（依赖business）
src/config/         v1_adapter, legacy_importer, profile_store（依赖business）
src/ledger/         in_memory_ledger, sqlite_ledger（依赖business；Sql只在后者）
src/application/    run_controller, ui_projection（连接域和原mainwindow）
tests/business/    数字、纯规则、迁移、账本测试
tests/replay/      状态/取消/重复事件测试
tests/fixtures/    已审定契约fixture的版本化副本
```

PR01建议新增静态库 `relink_business`，链接 `Qt6::Core`；保留旧 `src/domain.*` 给演示。AppState在PR05/11作为兼容展示适配，不成为新业务状态唯一真相。新测试目标使用 `business_value_tests`、`business_rule_tests`、`migration_tests`、`replay_tests`、`ledger_tests`。这几个名字是计划；创建目标后才可执行下列筛选命令。

PR10若采用本文优先方案 QtSql/SQLite，`find_package`新增Sql组件、只有仓库实现目标链接Qt6::Sql；测试验证QSQLITE driver真正可打开事务。账本逻辑与SQL存储分开，使内存仓库可以跑同一套语义测试。若改用独立sqlite3，则先记录ADR与编译器/来源/许可证/部署变化，不让两个实现同时进入首版。[R13–R15]

## 后续构建命令计划

项目根目录执行；**本轮没有执行这些应用命令**：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test
# 目前脚本会选择build_relocated；PR01让构建记录显式输出实际目录。
& .\.tools\aqt-env\Scripts\ctest.exe --test-dir .\build_relocated --output-on-failure
# 仅在PR01-PR04注册目标后用于分层诊断，不能替代上面全量回归。
& .\.tools\aqt-env\Scripts\ctest.exe --test-dir .\build_relocated -R 'business_|replay_' --output-on-failure
```

CTest直接启动前应沿用build.ps1设置的Qt/compiler PATH；CI建议统一入口脚本，不让手工shell偶然PATH掩盖缺依赖。PR12执行 `build.ps1 -Test -Package` 后，必须从固定发布目录、仅发布目录+系统PATH再做离屏验证；发布脚本不自动打开前台窗口。

## 每张代码票的提交记录

每次记录：源码revision/变更清单、SDK/编译器、实际buildDir、fixture哈希、完整command、stdout/stderr、exit code、断言数量、未执行层及原因。代码执行结果写入后续 `artifacts/implementation/<run-id>/`，不要改写本轮“planned_not_run”。

代码修改后才更新 `C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`。本轮文档更改不触发应用发布；旧程序及用户配置保持原状。[E11]
