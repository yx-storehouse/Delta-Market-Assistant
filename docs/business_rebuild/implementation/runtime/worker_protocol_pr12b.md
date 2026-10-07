# Worker 协议与双槽 lease 实现

续接日期：2026-10-07（Asia/Shanghai）。本模块实现的是**进程内协议校验和资源生命周期状态机**，不是 OCR 引擎或 Windows 捕获后端。`mapping_name` 在此只是经过校验的描述符；没有调用共享内存、进程启动、窗口捕获或图像文件 API。当前产品仍使用 synthetic/replay 数据。

## 在哪里维护

| 文件 | 责任 | 不承担的责任 |
|---|---|---|
| `src/application/vision/worker_protocol.h/.cpp` | 信封校验、NDJSON 分帧、双角色会话状态、请求/结果关联、消息幂等 | 管道读写、worker 进程、OCR 推理 |
| `src/application/vision/lease_pool.h/.cpp` | 双槽状态、generation、取消与显式释放、崩溃隔离 | 分配/映射真实共享内存或拷贝像素 |
| `tests/runtime/worker_protocol_tests.cpp` | 冻结正反 fixture、分片消息、状态迁移、异常/上限回归 | 真实 OCR 准确率或游戏画面验收 |
| `tests/release/verify_pr12b_package.py` | 独立测试目标、显式 fixture 路径、字面输出/退出码归档 | 以文档校验替代应用测试 |

该模块属于既有 `relink_runtime` 静态库，只依赖 Qt Core。页面不感知传输、帧 lease 或协议消息；未来连接层消费 [观察适配器](observation_adapter_pr12b.md) 的结构化请求和结果。

## 消息与状态

每条消息包含 `protocol_version=1`、`session_id`、`message_id`、`kind`、`body`。发送和接收都校验角色方向、字段集合、取值范围、会话以及消息字节上限。失败通过 `ProtocolError` 返回，候选状态未完整校验成功时不提交。

| 阶段 | 消息 | 关键检查 |
|---|---|---|
| 建立会话 | `hello` → `ready` | 协议版本、像素格式、模型/provider、单请求上限、两槽描述符 |
| 识别请求 | `recognize` | request/demand/context/frame 一致，ROI 有界，来源/时间元数据完整 |
| 正常结束 | `result` 或 request `error` | 结果必须关联原 request/frame/lease/context，provider/model 与握手一致 |
| 释放帧 | `release_frame` | request、lease、slot、generation 全匹配；终态或取消后才释放 |
| 中止请求 | `cancel` → `cancelled` | 按 request 或 demand 取消；确认取消不等于已释放像素 |
| 关闭会话 | `shutdown` → `bye` | 不接新识别请求；所有占用释放后才接受 bye |

同一 `message_id` 内容冲突返回错误。允许幂等的消息仅在内容完全一致时接受重试；结果不是可重复终态，释放重试也必须使用原消息 ID。指纹使用 SHA-256，不在重复消息表中保存整段大 payload。

上下文携带 run/session/clock/step、cancel epoch、viewport generation。取消后的迟到结果被拒绝；`cancelled` 回应不会提前使 lease 可复用。未知页或无文字是结构化 `unknown`，不是成功购买或成交事实。

## 内存与解析上限

| 项目 | 默认上限 |
|---|---|
| 单行 JSON payload | 1,048,576 bytes，结尾 LF 不计入 payload |
| 单帧 descriptor | 134,217,728 bytes |
| 输出 token / 单 token 文本 | 2,000 / 4,096 字符 |
| ROI | 64 |
| 同时识别请求 | 1 |
| 单次分帧返回 | 256 条消息 |
| 会话消息历史 | 4,096 条软上限，保留 4 个终态/释放/关闭名额用于排空 |
| 已释放 lease 历史 | 1,024 条，使用 lease ID + slot + generation 完整匹配 |

NDJSON 接收增量 `QByteArray`，支持单条消息跨多个块和一个块中多个消息；拒绝 BOM、无效 UTF-8、空行、CR、嵌入换行/NUL、超长行和 EOF 残片。分帧器发生错误后保持失败状态，显式 `reset()` 才重用。JSON 精确整数范围、BGRA8、stride×height、descriptor offset/length/capacity、ROI 边界都在进入请求状态前校验。

共享内存名称只接受本会话的 `Local\RelinkVision_<session>_<slot>` 形式，槽号只能是 0 或 1。这里的名称校验不是实际共享内存读写验收。消息不接受 `image_path` 等文件传输字段。

## 双槽回收规则

正常路径是 `Free → Writing → Pending → InUse → Free`。`Pending` 可以被更新的待处理帧替换，但更换 lease ID 并递增 generation；`InUse` 绝不覆盖。两个槽同时占用时拒绝第三帧，而不是无限排队。

取消 `Writing/Pending` 可以本地回收；取消 `InUse` 进入 `AwaitRelease`，必须等显式 release。worker 无响应时进入 `Quarantined`，阻止新写入和 pending 替换。`processExitConfirmed()` 与 `closeMapping()` 是两个独立确认，二者顺序完成后才能回收隔离槽。本实现只模拟这些确认，未真实终止进程或关闭映射句柄。

## 重跑本模块测试

完整构建通过项目入口执行；独立目标需使用包含 Qt DLL 的 PATH。发布验收脚本自动设置 package-only PATH，并将固定 fixture 目录作为参数传入：

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test
python -I -X utf8 tests\release\verify_pr12b_package.py modified --build-dir .\build_relocated
~~~

当前断言数量以 `WORKER_PROTOCOL_TESTS=PASS; assertions=...; failures=0` 的本轮实测记录为准；`external_processes=0; image_file_writes=0` 明确限定测试范围。冻结 schema/fixtures 是输入契约，不批量改写为生产验收结论。

## 接入真实图像前的交接

继续复用 [下一阶段提示词](../../NEXT_IMPLEMENTATION.md)，先保留进程内 fake/replay 集成回归，再为真实来源与识别 provider 建立独立实现。真正需要游戏画面时单独验证窗口/DPI/遮挡/最小化、ROI、页状态、价格/成色文字、低置信度和误识别拒绝；这部分当前没有测试结果。

运行参数仍只随配置保存。识别模块只输出结构化观察；观察价不冒充确认成交价，也不直接触发键鼠、购买或交易。
