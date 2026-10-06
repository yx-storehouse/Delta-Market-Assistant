# Runtime 实施契约 v1 · 交接入口

状态：**开发规格冻结候选，产品后端尚未实现**。本目录将原第02/04章细化为可以分别编写协调器、回放源和视觉worker的接口；所有超时、预算、容量均为本项目新设计，不是BBZPS的实测最优值。

## 1. 开工顺序与唯一入口

| 顺序 | 工程人员先读 | 交付内容 | 阶段 |
|---|---|---|---|
| 1 | [状态机与账本协作](state_machine.md) | 纯事件reduce、假时钟、命令effects、Unknown/预留语义 | M1 / WI03–WI04 |
| 2 | [观察需求与帧](observation_contract.md) | FakeCaptureSource、Observe命令计数、代次屏障 | M1接口、M2真实实现 / WI06 |
| 3 | [worker协议](worker_protocol.md) | NDJSON解析、共享内存租约、worker故障 | M2 / WI07 |
| 4 | [离线场景](fixtures/replay_cases.json) | 用同一输入验证最终状态/事件/采集请求数 | M1；当前未运行业务引擎 |
| 5 | [机器契约](validation_manifest.json) | Draft 2020-12正反例验证，不自动下载引用 | 本轮文档检查 |

沿用核心值对象：[core.schema.json](../domain/core.schema.json)。技术资料索引：[references.runtime.json](references.runtime.json)。根需求以[E12](../../evidence/2026-10-06_capture_requirement.md)和[ADR10](../../07_decisions.md)为准。

## 2. 冻结的边界

- **M1** 只读结构化回放，运行FakeClock/FakeCapture/FakeAction/FakeLedger；不得启动原样本或连接真实输入。假采集计数是协议效果，不是实际采集性能。
- **M2** 加真实只读捕获、内存像素与OCR；选择WGC/DXGI、具体模型、硬件默认参数仍需实测评审。保持现有Qt/MinGW UI，不直接链接未知ABI的OCR C++库。
- **M3/M4** 先把收藏、时间、延迟、输入适配器接到自有测试窗口；真实目标输入是后续独立决策，未由本文档默认启用。
- 实时图像只进内存；**正常/成功/失败/崩溃都不自动写截图**；共享内存失败报错，不回退到临时图片文件。暂停/长等待/停止不新采集，模型可驻内存。
- 观察需求、业务运行、worker资源生命周期分别建模；“已暂停”不代表读者瞬间消失，“结果过期”不代表可复用读者仍持有的缓冲区。
- 所有绝对单调时间仅在相同 `clock_domain_id` 内比较；worker只接相对预算。重启后旧时钟域与旧帧全部失效。

## 3. 文件及适用精度

| 文件 | 约束级别 |
|---|---|
| observation.schema.json | ObservationDemand、FrameEnvelope及ROI字段/单位/上限 |
| worker.schema.json | 每个NDJSON消息的结构、禁止额外字段、像素不随JSON传递 |
| state.schema.json | TaskRun、Attempt、回放输入/期望结构；核心业务领域对象不复制 |
| semantic_cases.json | JSON Schema表达不了的大小关系、代次、租约、时间先后错误 |
| fixtures/replay_cases.json | 具名场景、事件输入、期望效果；不是本轮已通过后端测试 |
| fixtures/messages/*.valid.json / *.invalid.json | IPC结构正反例；错误码另有语义测试 |
| validation_manifest.json | 供主文档校验器离线注册schema并逐例核验 |
| references.runtime.json | 新查官方源的URL、访问状态、哈希和用途 |

**规范优先级：E12产品约定 > 本目录字段/生命周期规则 > 老版草案例子。** 本目录未修改现有应用；原 `examples/vision-result.sample.json` 属概念示例，实施时使用这里的协议，不混用字段名。

## 4. 代码放置建议（将来开发，不是现有文件）

```text
src/application/runtime/        RunCoordinator, RuntimeReducer, FakeClock
src/application/observation/    ObservationScheduler, LeaseRegistry
src/infrastructure/capture/    FakeCaptureSource; M2 WgcCapture/DxgiCapture
src/infrastructure/vision/     VisionProcessClient, NdjsonCodec
src/infrastructure/ledger/     ILedger adapter（由domain/存储合同约束）
worker/vision/                 protocol.py, shm_reader.py, provider.py
tests/runtime/                 replay_contract_tests（消费本目录fixtures）
```

UI线程只发布带command_id的命令并显示不可变状态；协调器单线程串行仲裁业务状态/代次；捕获线程只提交候选帧；IPC线程只解析和分发；OCR线程只读已租赁内存。任何线程不得直接改变其他线程的状态快照。R10/R11支持Qt线程/进程分工；具体目录和接口是本项目设计。

## 5. 追踪与未决

| 实施单元 | 原业务 | 原验收 | 证据/参考 |
|---|---|---|---|
| 按需采集/代次/暂停 | B06、B07、B22、B35、B43、B44 | T06-A/B、T07-A/B、T22-A/B、T35-A/B、T43-A/B、T44-A/B | E12；R03–R05、R08–R11 |
| 规则前后状态/未知结果 | B16、B17、B19、B20、B29、B30 | T16–T20、T29–T30各A/B | E04、E08；R08、R13 |
| 幂等预留/重启 | B28、B31、B37 | T28-A/B、T31-A/B、T37-A/B | E04、E12；R13 |
| 内存映射/验证协议 | B44 | T44-A/B | E12；RRT01–RRT03 |

依旧未决：D03延迟触发基准、D04动态公式、D05限购周期、D07稳定卖单ID、D09模型质量门槛、D10捕获适配性、D11市场时段。**这些不阻塞M1开发，阻塞相应真实后端/动作激活；不得以默认值消除未决。**

## 6. 可复用开发任务提示

```text
以 implementation/domain/core.schema.json 和 implementation/runtime/ 为唯一契约，先实现 M1 纯事件协调器。
只连接 FakeClock/FakeCaptureSource/FakeAction/账本测试适配器；不实现真实截图/OCR/输入。
运行 runtime/fixtures/replay_cases.json 的每条序列，逐项比较状态、去重事件、请求数、预留和图像文件写入计数。
新增源文件前先确认现有 CMake/test 约定；保持 Win11 白灰UI及现有配置可读。
将结构校验通过与后端行为测试通过分开记录；不要沿用文档的预期结果当测试结果。
```
