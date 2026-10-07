# 实施契约与当前实现入口

更新时间：2026-10-07（Asia/Shanghai）。服务、历史、观察、协议/lease 和通用视觉传输基础层已有 19/19 CTest 通过记录。独立 `relink_vision_transport` 已实际验证 Win32 共享帧与隐藏合成 fixture child；它没有链接桌面程序，child 不是 OCR worker、不随包发布。下一步是真实游戏窗口/frame source、ROI 与 OCR provider 校准。最终新版发布/回退以本轮构建后更新的 `artifacts/m1_pr12b_transaction/VERIFICATION.txt` 和 `artifacts/m1_pr12b_delivery_final/VERIFICATION.txt` 为准。

这里同时保留**历史开工规格**和**当前实现记录**。Schema、fixture、DDL、backlog 与测试矩阵是设计输入；它们通过文档校验，不代表相应生产功能或真实捕获已通过测试。当前行为以源码、CTest 与分票实现记录为准。

## 从当前开发位置继续

1. [09 · 实施进度](../09_m1_progress.md)：已实现范围、实测结果、当前基线与下一票。
2. [PR08 · ProfileStore](profile_store_pr08.md)：只读预览经过审定后保存 ConfigV2，不自动激活规则或替换运行配置。
3. [PR10 · SQLite 事件仓库](sqlite_store_pr10.md)：接口、版本迁移、提交快照、进程恢复、备份和验证记录。
4. [PR11 · 持久化工作区](workspace_pr11.md)：方案审定、运行/历史快照、controller/projection、错误处理和 CSV。
5. [PR12A · 页面模块化](ui_modularization_pr12a.md)：控件归属、接口、独立测试与剩余耦合。
6. [内存观察适配器](runtime/observation_adapter_pr12b.md)：步骤触发、内存帧、取消/超时/新鲜度和合成回归；不是实屏识别。
7. [Worker 协议与 lease](runtime/worker_protocol_pr12b.md)：NDJSON、关联、资源上限、双槽回收与合成回归。
8. [进程内图像链路](runtime/vision_pipeline_pr12b.md)：合成内存帧、协议/lease 串联与取消回收回归。
9. [真实共享帧/隐藏进程传输](runtime/vision_transport_pr12b.md)：Win32 mapping、QProcess、fixture 隔离和异常回收。
10. [下一轮开发提示词](../NEXT_IMPLEMENTATION.md)：M2 真实游戏窗口、ROI 与 OCR provider 校准；保留 PR12 历史验收清单。

## 查阅冻结规格

| 入口 | 内容 | 使用方式 |
|---|---|---|
| [开工标准](../08_implementation_ready.md) | 初始 Definition of Ready 与首条纵切片 | 历史开工依据，不覆盖当前进度 |
| [readiness/README.md](readiness/README.md) | 12 张 PR 任务票与 30 条 GWT 规格 | 保留原始任务、依赖与验收定义 |
| [domain/README.md](domain/README.md) | 值对象、商品/卖单/规则、Decision、导入与配置 schema | 用作输入契约；实现差异在分票记录说明 |
| [runtime/README.md](runtime/README.md) | 状态机、ObservationDemand、freshness barrier 与 worker 协议 | Replay/Fake 已有实现；真实捕获/OCR 仍属于后续阶段 |
| [integration/README.md](integration/README.md) | 领域、运行时、账本与 UI 的集成边界 | 同时阅读冻结目标与实际接口映射 |

## 机器契约与实测的区别

| 位置 | 内容 | 当前解释 |
|---|---|---|
| domain/*.schema.json | Draft 2020-12 领域与配置 schema | 结构验证不等于全部业务已实现 |
| domain/fixtures/ | 规则、导入与 schema 正反例 | 冻结规格输入；不是生产验收报告 |
| runtime/*.schema.json、runtime/fixtures/ | 观察、worker、状态与回放输入 | 不能由此声称真实捕获已测试 |
| integration/storage_schema.sql | 初始完整目标 DDL | PR10 未直接执行此 DDL；实际关系表与差异见分票记录 |
| readiness/backlog.json | PR01–PR12、依赖与回退 | 冻结任务计划；完成状态看实施进度 |
| readiness/test_matrix.json | 30 条 GWT 测试规格 | 保留 planned_not_run 的历史记录，不批量改写为 PASS |
| src/、tests/ | 当前 C++ 实现与自动化测试 | 用实际构建日志、退出码和断言证明行为 |

## 验证入口

在项目根目录执行文档校验：

~~~powershell
python -I -X utf8 docs\business_rebuild\scripts\validate_contracts.py
python -I -X utf8 docs\business_rebuild\implementation\readiness\validate_readiness.py
python -I -X utf8 docs\business_rebuild\scripts\validate_docs.py
~~~

这些工具只检查文档、schema、fixture、链接和基线。它们的 BUSINESS_ENGINE_TESTS_EXECUTED=0、LIVE_CAPTURE_TESTS_EXECUTED=0、APPLICATION_TESTS_RUN=0 表示**这些校验脚本本身没有运行应用测试**，不否认另行执行的 CTest；也不能把这些字段改成业务验收结果。

应用测试与打包另行执行：

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Package
~~~

固定交付为 C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe；保留 DLL、platforms 与新增的 sqldrivers。发布验收仅离屏运行，不自动打开用户前台窗口。

## 代码边界

~~~text
src/business/              精确值对象、模型、校验与纯规则
src/application/           Replay/Fake 运行时与 UI projection
src/application/workspace/ WorkspaceController、持久化 UI 投影与离屏自测
src/application/runtime/observation/ 步骤触发式合成内存观察
src/application/vision/    NDJSON/双槽 lease 状态；独立 Win32 共享帧与隐藏进程传输实现
src/config/                LegacyImportPreview 与 ConfigV2 ProfileStore
src/ledger/                InMemoryEventStore 与 SqliteEventStore
tests/business/            领域、迁移、profile、内存与 SQLite 仓库测试
tests/runtime/             回放与运行时测试
tests/application/         工作区、方案审定、提交失败、历史快照与 CSV 测试
~~~

PR11 在 PR10 结构化持久化之上显式接入 UI，不接入真实捕获/OCR、鼠标键盘动作或交易。配置仍走 PR08 的 QSaveFile 文件；审定保存不会自动选择方案或启用规则。界面显示的是已提交合成回放事实，历史运行使用冻结方案快照；Win11 浅色白灰布局保持不变。

## PR12B 服务边界与发布验收

PR12B 已完成四个服务边界的提取，控制器保留生命周期和命令协调；UI 页面继续消费只读投影，不操作 SQL：

| 服务 | 已提取的职责 | 保留的边界 |
|---|---|---|
| `ProfileCatalog` | 方案目录扫描、校验与审定物化 | QSaveFile 配置持久化独立于账本 |
| `ReplayScenario` | 内建八步合成场景和观察序列 | 不启动真实游戏，不是 OCR |
| `HistoryQueryService` | 运行摘要与选中 run 明细查询 | SQLite 连接保持在所属线程内 |
| `RecordsExporter` | CSV 转义、路径保护和只读投影导出 | 不修改账本或配置 |

当前通过 19/19 CTest：工作区 432、UI 模块 90、观察 828、协议 646、进程内链路 279、Win32 共享帧 65、隐藏合成 worker 进程 206 条断言；UI / 工作区 UI / 存储自检为 206 / 58 / 22 项，独立负向包验收为 41/41。发布脚本强制执行这些目标、检查 fixture child 不入包，并记录命令/输出/退出状态；新增 transport 后最终打包和独立副本回退以与本轮构建哈希对应的报告为准。41 条负向测试验证的是交付工具拒绝路径，不代替真实发布目录验收。

`tests/verify_delivery.py --build-dir ... --release-dir ... --output-dir ...` 只写入新建验收目录，核对清单全部文件/哈希/字节数、拒绝发布链接/重解析点、检查包内依赖、测试快照模式配置和工作区逐字节保持，并实际执行配置回退脚本。验收不会直接修改正常用户配置或账本。

## M2 接入边界

ObservationAdapter 通过合成内存源验证，SharedFrameMemory / ReadOnlyFrameMemory 与 WorkerProcess 通过真实 Win32 transport 测试。IObservationSource 与 IObservationRecognizer 仍未连接真实游戏画面和 OCR provider。观察/协议属于 relink_runtime，OS 传输属于独立 relink_vision_transport；内存字节和隐藏合成进程通过不代表真实 ROI 或 OCR 准确率已验收。

下一轮按 [M2 提示词](../NEXT_IMPLEMENTATION.md) 复用这些接口，不把捕获或识别代码塞进窗口，不写运行时截图或临时图片。需要真实游戏画面时进入单独联调；当前仍为 `game_connected=false`、`system_input_sent=false`、`image_file_write_count=0`，没有执行输入注入、购买或交易。
