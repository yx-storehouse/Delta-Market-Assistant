# Delta Market Assistant · 业务重构开发文档

> 文档版本：0.4 · 2026-10-06 · 状态：PR01–PR10 已完成；本轮 12/12 CTest、316 条 SQLite 断言、22 条发布存储自检与真实发布回退复验通过，PR11 常规 UI 接入待开发。
>
> 项目名为 Delta Market Assistant（三角洲市场助手），现有构建目标及程序文件仍名为 RelinkStudio。原始静态分析与设计规格保留；当前实现、实测结果和新基线见 [09 · 实施进度](09_m1_progress.md)，不再将整个项目描述为“尚未实现”。

## 先看结论

**采集策略保持步骤触发、像素只走内存。** 到需要识别的业务步骤才取新画面；空闲、暂停、长等待不持续采集或 OCR，正常、成功与报错默认都不落截图。图片仅在用户主动导出或显式开启有界调试时保存。配置和必要账本正常持久化，不逐帧写日志。当前还未接入真实捕获/OCR。[E12；ADR10]

**保留 Win11 白灰界面，独立重写业务层。** 不搬运原程序的 EXE、DLL 或授权组件；采用两阶段业务组织：先按条件找卖单并收藏，再在关注列表处理公示倒计时、确认与结果核验。这是依据静态证据建立的重构路线，不是复制原程序运行环境。[E03、E04]

**原样本有 OpenCV 线索，但不是单靠 OpenCV。** 目录含 cv2；名为 gdi32.dll 的文件实际导出 TomatoOCR、ocrScreen、ocrWindow、ocrImageData。较准确的拆分是图像处理、OCR、页面语义与状态机。ppocrv5/MNN 字符串不足以证明实际选用模型或运行参数；完整分析见证据章节。本轮没有运行样本。[E01、E02、E10]

**开发已从规格进入 M1 实现。** PR01–PR09 已有领域模型、纯规则、回放、只读导入预览、ProfileStore 和内存账本；PR10 增加 SQLite 持久化、事务恢复与发布驱动验证。PR11 仍需把已保存方案和已提交账本接到现有 UI；M2 真实捕获/OCR、M3 旧延迟/限购语义与 M4 外部动作尚未完成。历史 schema、fixture、backlog 和 GWT 矩阵继续作为冻结设计输入，不因 CTest 通过而批量改写。[09、implementation/sqlite_store_pr10]

## 从这里阅读

| 文档 | 解决什么问题 |
|---|---|
| [09 · 实施进度与基线](09_m1_progress.md) | 当前已实现范围、本轮实际验证记录、基线与下一票 |
| [PR10 · SQLite 事件仓库](implementation/sqlite_store_pr10.md) | 接口、迁移、事务、恢复、备份、部署和冻结 DDL 差异 |
| [后续开发交接提示词](NEXT_IMPLEMENTATION.md) | 下一轮可直接复用的 PR11 开发任务 |
| [01 · 业务清单与追踪矩阵](01_business.md) | 44 项可见业务/重构能力的来源、模块与验收规格 |
| [02 · 图像识别与系统架构](02_vision_architecture.md) | OpenCV/OCR 分工、Qt、worker、截图和 DPI |
| [03 · 数据结构与配置迁移](03_data_config.md) | 13 列任务、运行参数、schema v1→v2 和未知字段保留 |
| [04 · 状态机与业务规则](04_state_machine.md) | 收藏/关注双阶段、倒计时、结果核验、预留与恢复 |
| [05 · 开发计划与验收](05_delivery_tests.md) | M0–M4 工作包、测试数据、门槛与回退 |
| [06 · 证据与参考资料](06_evidence_references.md) | E→F→P 追踪链与官方资料核查记录 |
| [07 · 决策与待确认项](07_decisions.md) | 未知字段、影响和解除条件 |
| [88 条验收规格](acceptance_cases.md) | 冻结的正常、边界、异常规格；逐条状态不等于当前 CTest |
| [08 · 开工标准](08_implementation_ready.md) | 历史 Definition of Ready 与首条纵切片 |
| [implementation/](implementation/README.md) | 当前实现入口、领域/运行时/集成契约、12 张票和 30 条 GWT 规格 |

同目录 index.html 保留原始设计的单文件阅读入口；当前开发状态以 09_m1_progress.md 和分票记录为准。Markdown 是可编辑源文件，JSON 是机器索引。继续开发先读当前进度，再核对 backlog 对应任务，不从旧 src/domain.h 猜新业务。

## 文档中的事实等级

| 标记 | 含义 | 使用方式 |
|---|---|---|
| 证据确认 | 配置、导出接口或历史日志直接支持 | 只说明已看到的行为 |
| 合理推断 | 多项证据支持，但缺完整函数或边界证据 | 进入预览/待决表，不自动成为执行规则 |
| 用户约定 | 用户截图、对话与 AGENTS.md | 保留入口，不冒充原样本的完整实现 |
| 重构设计 | 为可靠性和可测试性提出的新方案 | 在当前实现记录中说明落地与差异 |
| 已实现/已实测 | 源码与实际命令、日志、退出码支持 | 实现完成与验收完成分别记录 |
| 待决 | 当前资料不足 | 保留原值、影响与解除条件 |

E 编号沿用既有静态分析并包含 E10、E11、E12；E12 记录按需采集要求。R 编号是官方技术资料。**技术资料说明新实现可以如何做，不证明原程序具体调用过哪个 API。**

## 当前实现和下一步

- C++17 / Qt 6 Widgets 前端继续兼容 schema v1 演示配置；新业务层使用 Replay/Fake。PR08 审定方案为独立 ConfigV2 文件，不自动替换运行中的 AppState。E08 是历史基线，不是全部当前实现。[09、implementation/profile_store_pr08]
- 界面保持 Win11 / 微软商店浅色白灰：#F3F3F3 背景、#F9F9F9 内容、白卡片、细灰边框、中性近黑 #1F1F1F 强调，不使用蓝色。运行/任务页、关注列表、右侧编辑与底部价格图不重做。[E11；AGENTS.md]
- PR10 只增加后端结构化持久化；正常前台尚未接入 SQLite。PR11 再连接方案、账本、恢复结果与记录投影；QtSql 可用不等于 UI 工作流已经完成。[09]
- 当前没有真实捕获、OCR、鼠标键盘输入、购买或交易。步骤触发和图像不落盘要求，与配置/必要账本落盘分别执行。[E12；ADR10]
- 修改后构建、离屏验收并更新已解压程序 C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe；保留 DLL、platforms 和 sqldrivers。不自动打开可见窗口。[AGENTS.md]

## 文档与应用分别复验

在项目根目录运行文档校验：

~~~powershell
python -I -X utf8 docs\business_rebuild\scripts\validate_contracts.py
python -I -X utf8 docs\business_rebuild\implementation\readiness\validate_readiness.py
python -I -X utf8 docs\business_rebuild\scripts\validate_docs.py
~~~

这些命令检查文档、示例、证据、链接和基线，不运行后端业务测试，也不代表 88 条验收规格全部通过。应用构建测试独立执行：

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Package
~~~

当前实测结果见实施进度；历史文档校验记录保留在 [VERIFICATION.txt](verification/VERIFICATION.txt) 和 artifacts/business_rebuild_docs。旧 baseline 保持原样，每轮开发另建明确覆盖变更范围的新 baseline。
