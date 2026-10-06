# 实施契约与当前实现入口

更新时间：2026-10-06（Asia/Shanghai）。当前 PR01–PR10 已完成；PR10 的 QtSql/SQLite 持久化已通过 12/12 CTest、316 条 SQLite 断言、发布存储自检与回退复验，详见 [实施进度](../09_m1_progress.md)。PR11 的方案/账本 UI 接入与 PR12 的完整里程碑验收尚未完成。

这里同时保留**历史开工规格**和**当前实现记录**。Schema、fixture、DDL、backlog 与测试矩阵是设计输入；它们通过文档校验，不代表相应生产功能或真实捕获已通过测试。当前行为以源码、CTest 与分票实现记录为准。

## 从当前开发位置继续

1. [09 · 实施进度](../09_m1_progress.md)：已实现范围、实测结果、当前基线与下一票。
2. [PR08 · ProfileStore](profile_store_pr08.md)：只读预览经过审定后保存 ConfigV2，不自动激活规则或替换运行配置。
3. [PR10 · SQLite 事件仓库](sqlite_store_pr10.md)：接口、版本迁移、提交快照、进程恢复、备份和验证记录。
4. [下一轮开发提示词](../NEXT_IMPLEMENTATION.md)：继续 PR11 的可复用任务说明。

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
src/config/                LegacyImportPreview 与 ConfigV2 ProfileStore
src/ledger/                InMemoryEventStore 与 SqliteEventStore
tests/business/            领域、迁移、profile、内存与 SQLite 仓库测试
tests/runtime/             回放与运行时测试
~~~

PR10 只增加结构化账本持久化，不接入真实捕获/OCR、鼠标键盘动作或交易。配置仍走 PR08 的 QSaveFile 文件；账本落库不会自动把方案或记录接到前台。Win11 浅色白灰布局保持不变。
