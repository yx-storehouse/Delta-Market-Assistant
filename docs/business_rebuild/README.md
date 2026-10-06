# Relink Studio · 业务重构开发文档

> 文档版本：0.3 · 2026-10-06 · 状态：M1 已达到可开工规格，业务引擎尚未实现。  
> 依据：既有 BBZPS 静态证据、历史运行日志、当前 Relink Studio 0.6.0 源码及最新界面约定。此次只生成文档和文档验收工件，没有运行 BBZPS，也没有修改前端源码或发布程序。

## 先看结论

**v0.2 已按用户补充固定采集策略：到需要识别的业务步骤才取新画面，图像只走内存，默认不保存截图或临时图片。** 空闲/暂停/长等待不持续采集和OCR；成功、报错也不自动落图。图片只在用户主动导出或显式开启限量调试时保存。配置和必要业务记录仍正常持久化，不逐帧写日志。[E12；ADR10]

**采用“保留现有 Win11 白灰界面，独立重写业务层”的路线，不把原程序的 EXE、DLL 或授权组件搬过来。** 最值得采用的是它的两阶段业务组织：先按条件找卖单并收藏，再在关注列表处理公示倒计时、确认与结果核验。[E03、E04]

**它有 OpenCV，但不只是 OpenCV。** 目录有 `cv2`，OCR 库里有 OpenCV 字符串；这份名为 `gdi32.dll` 的文件实际导出 `TomatoOCR`、`ocrScreen`、`ocrWindow`、`ocrImageData` 等接口。更准确的判断是“图像处理 + OCR + 页面语义 + 状态机”。`ppocrv5` 和 MNN 工程路径只是内部组件线索，尚不足以证明运行时选用了哪一个模型及什么参数。[E01、E02、E10]

**我们需要重构的不是几个识别函数，而是完整的可核验流程。** 数据上要分开商品、卖单观测、任务规则、运行实例和操作回执；工程上要分开 UI、截图、视觉识别、纯规则、调度与账本。先让离线数据回放正确，再做只读观察和自有测试窗口闭环。[E04、E08；本项目设计]

**现在已经补到了可以真正开工的程度，但只对 M1 首条纵切片作出 Ready 判断。** 域模型、规则原因码、状态/事件、按需采集与租约、UI命令投影、账本事务、任务票、GWT测试、构建基线和回退要求均有独立契约；M2 的真实捕获/OCR、M3 的旧延迟/限购语义、M4 的外部动作仍不能靠文档“假装完成”。[08、implementation/readiness]

## 从这里阅读

| 文档 | 解决什么问题 |
|---|---|
| [01 · 业务清单与追踪矩阵](01_business.md) | 44 项可见业务/重构能力，逐项列来源、现状、模块和验收 |
| [02 · 图像识别与系统架构](02_vision_architecture.md) | OpenCV / OCR 分工、Qt 工具链、worker 协议、截图与 DPI |
| [03 · 数据结构与配置迁移](03_data_config.md) | 13 列任务、全部可见运行参数、schema v1→v2、未知字段保留 |
| [04 · 状态机与业务规则](04_state_machine.md) | 收藏/关注双阶段、倒计时、结果核验、数量预留、失败恢复 |
| [05 · 开发计划与验收](05_delivery_tests.md) | M0–M4 工作包、测试数据、验收门槛、发布和回退 |
| [06 · 证据与参考资料](06_evidence_references.md) | E→F→P 追踪链及 16 项官方资料、核查记录 |
| [07 · 决策与待确认项](07_decisions.md) | 不猜填未知字段；阻塞点、默认处理及解除条件 |
| [88 条验收规格](acceptance_cases.md) | 正常、边界、异常用例；目前均未执行后端验收 |
| [后续开发交接提示词](NEXT_IMPLEMENTATION.md) | 下一轮可直接复用的 M1 开发任务 |
| [08 · 开发开工标准与首条纵切片](08_implementation_ready.md) | 文档达到什么闭环才可写代码；M1/M2/M3/M4当前等级和PR01–PR05顺序 |
| [implementation/](implementation/README.md) | 领域Schema、运行时协议、UI/存储集成、12张PR任务票和30条GWT测试 |

便于直接阅读的单文件入口是同目录 `index.html`；不依赖在线脚本、字体或样本程序。Markdown 是可编辑源文件，JSON 是业务/用例/资料的机器可读索引。真正开工时先读 `08_implementation_ready.md`，再进入 `implementation/readiness/backlog.md`，不要直接从旧 `src/domain.h` 猜新业务。

## 文档中的事实等级

| 标记 | 含义 | 使用方式 |
|---|---|---|
| 证据确认 | 配置、导出接口或历史日志直接支持 | 说明看到的行为，不扩展为全部源代码行为 |
| 合理推断 | 多个证据支持，但没有完整函数或边界证据 | 进入导入预览/待决表，不作为默认执行规则 |
| 用户约定 | 原 Relink 截图入口与本项目 AGENTS.md | 保留入口；不能声称 BBZPS 有完全相同实现 |
| 重构设计 | 为可靠性、可测试性提出的新方案 | 接受后实施；不冒充原程序逻辑 |
| 待决 | 当前资料不足 | 保留原值、记录影响和解除条件 |

E 编号沿用上一轮分析并补充 E10、E11、E12；E12记录用户本次的按需采集要求，R 编号是官方技术资料。**技术资料说明新实现可以如何做，不是证明 BBZPS 具体用了某个 API。**

## 当前基线和第一步

- 当前程序仍是 C++17 / Qt 6 Widgets 的 0.6.0 演示前端；配置为 `schema_version=1`、`demo=true`，`simulateTick()` 只处理合成数据。[E08]
- 界面继续 Win11 / 微软商店浅色白灰：`#F3F3F3` 底、`#F9F9F9` 内容、白卡片、中性近黑强调，**不使用蓝色**；运行/任务页、关注列表、右侧编辑、底部价格图不重做。[E11]
- 第一开发里程碑 M1：独立领域模型、纯规则解释器、旧配置导入预览、离线回放、InMemory事件账本和UI解释投影。截图/OCR 在 M2 接入；当前界面本身继续不发出点击或购买。[设计]
- M1首条纵切片顺序固定为 PR01→PR05：领域库/值对象→纯匹配→可控回放→现有UI显示；QtSql、OpenCV、OCR、WGC/DXGI和真实输入不进入首个PR。[08、implementation/readiness]
- 完成代码修改后仍交付解压目录 `C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`，并保留 DLL 和 `platforms`。本轮没有改这份 EXE。[E11]

## 文档交付复验

在项目根目录运行：

```powershell
python -I -X utf8 docs\business_rebuild\scripts\validate_docs.py
python -I -X utf8 docs\business_rebuild\scripts\validate_docs.py --evidence E04
```

检查覆盖业务与用例互链、示例 JSON、证据哈希、官方资料核查记录、内部链接，以及源码/发布目录与本轮基线是否一致。它检查的是**文档与示例契约**，不是后端业务已经通过 88 条用例。实际输出见随包的 [VERIFICATION.txt](verification/VERIFICATION.txt)，项目原始记录在 `artifacts/business_rebuild_docs/`。
