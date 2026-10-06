# 视觉worker协议 v1 · NDJSON控制 / 命名共享内存像素

状态：拟实现接口；与BBZPS接口无二进制兼容关系。对应B44/WI07、E12、R10/R11/RRT01–RRT03。基于官方平台能力设计，不表示已经验证实际worker后端。

## 1. 会话、编码与方向

协调器以绝对路径启动项目自有worker，工作目录独立；不搜索样本目录或PATH中的同名程序。stdin/stdout只传UTF-8 NDJSON，每行恰好一个JSON对象并以LF结束，不输出BOM或调试print；stderr为有界诊断流，不转成逐帧OCR日志。

共同信封：`protocol_version=1, session_id, message_id, kind, body`；未知版本/未知kind/额外字段拒绝。一个会话里message_id唯一；重复控制消息幂等，recognize重复不得再推理。无HTTP、远程服务或样本授权依赖。

| 顺序/方向 | 消息 | 协商与错误 |
|---|---|---|
| C→W第一条 | hello | 支持版本[1]、新session_id、transport=win32_named_shared_memory、像素BGRA8、保护上限、两槽memory_pool清单 |
| W→C | ready | 相同session_id、选定版本1、worker_build、provider/model_id/model_sha256、能力列表 |
| C→W | recognize | 新request_id、Demand关联、lease描述、ROI、relative_budget_ms |
| W→C | result或error | 每request至多一个终结响应；关联字段必须与请求相同 |
| W→C | release_frame | 与终结响应独立；意味着不再读该lease并已关闭该读映射 |
| C→W | cancel | request或demand目标+reason；立即禁止结果驱动，不保证模型API可瞬时中断 |
| W→C | cancelled | 确认已登记取消；**不是release证明** |
| C→W | shutdown | 停止接收新请求并排空/取消读者 |
| W→C | bye | 已关闭全部映射、可正常退出；协调器仍确认进程退出 |

hello之前除hello以外均E_HANDSHAKE_REQUIRED；ready之前不发送recognize。worker输出模型加载进度只走限速stderr。ready迟到/版本不匹配不进入运行。握手超时**新设计默认10000ms**，不是模型启动性能承诺；超时后先结束该worker，模型初始化性能由M2测量再评审调整。

session_id同时绑定worker与当前协调器；worker重启必须新session_id，同时旧请求/lease全部无效，不在同一session里“假装没崩过”。run恢复时业务账本可保留run_id，但新step/观察会话不同。

## 2. recognize的检查顺序

`worker.schema.json` 负责结构；运行时执行下列语义约束：

1. 收到ready后，检查session/request未重复、当前处理槽空闲、版本/能力一致。v1 worker并发度固定1；pending由协调器持有，不发给worker排队。
2. 检查同需求的run_id/step_id/cancel_epoch/viewport_generation，预算>0；队列等待也计入协调器绝对deadline，relative_budget_ms不延长总截止。
3. descriptor.transport只接受win32_named_shared_memory；mapping_name必须匹配已协商会话派生前缀和协调器本地租约登记，不按客户端传来的路径打开任意对象。

   hello.memory_pool登记恰好slot_index 0与1各一次及mapping_name/capacity_bytes；worker拒绝两槽重复、name不等于`Local\\RelinkVision_<session_id>_<slot_index>`或容量不符。recognize必须与这份清单完全匹配，只允许lease_id/slot_generation随帧更新。视口扩大超过原池容量时先取消并排空旧会话，再重建池和hello；不能原地把worker正在读的映射替换成另一块内存。
4. 映射**只读**；检查真实section大小≥offset+byte_length且算术无溢出，stride×height=byte_length、ROI不越界；参数校验通过前不调用OCR。
5. 模型读取期间像素不可变；处理中不允许共享槽覆写。图像预处理可创建有上限的工作内存，但不生成图像文件，不把图片转Base64打印。
6. 推理完成先生成结构化result/error并终结request，再关闭读映射并发release_frame。网络流字节顺序不是业务代次授权；协调器仍逐条校验。
7. 取消可能先到/后到结果；协调器只认其单线程仲裁时刻：撤销已提交则所有后续结果无效，但release始终按lease表单独处理。

## 3. result及错误语义

result：status为ok或unknown；ok表示worker完成识别，不等于字段可信或业务Match，更不等于成交。tokens包含text、score（0..1引擎原始分数）、4点box_px、roi_id；source frame与request完全一致。耗时使用worker自己的elapsed duration，不提供跨进程绝对deadline。

unknown：reason_code指出空识别/不支持布局/低质量等，tokens可为空；不得把未知价格补0。error：稳定code、retryable、诊断message、request_id（握手错误允许null）。**retryable只允许协调器按剩余步骤预算重建观察，不允许worker自己发截图请求。**

| 稳定错误码 | 处理 | 可用同帧盲重试? |
|---|---|---|
| E_PROTOCOL_VERSION / E_HANDSHAKE_REQUIRED / E_MESSAGE_TOO_LARGE / E_MESSAGE_INVALID | 终止该协议会话，结构化记录 | 否 |
| E_SHM_OPEN / E_SHM_BOUNDS / E_LEASE_UNKNOWN | 停观察并显式失败；绝不改临时文件 | 否 |
| E_ROI_OUT_OF_BOUNDS / E_PIXEL_FORMAT | 校正布局/参数后新需求 | 否 |
| E_CANCELLED / E_DEADLINE | 丢业务结果，等待release或退出 | 否 |
| E_WORKER_CRASH / E_WORKER_UNRESPONSIVE | 隔离lease，终止并确认进程退出，代次更换 | 否 |
| E_CAPTURE_LOST / E_WINDOW_LOST / E_EMPTY_FRAME | 停当前需求，按恢复预算新观察 | 否 |
| E_STALE_OR_UNPROVEN_FRAME / E_CONTEXT_MISMATCH | 丢弃观测，新屏障后再观察 | 否 |
| E_OCR_PROVIDER / E_MODEL_MISMATCH | 暂停相应provider，保留诊断 | 仅人工选定策略后 |
| E_UNKNOWN_PAGE / E_NO_TEXT | 结果未知，不当空列表或到时 | 仅剩余有界观察预算 |
| E_DUPLICATE_MESSAGE | 只回原响应或去重计数，无重复推理/释放 | 否 |

code枚举冻结，message是脱敏诊断而非机器判断依据。模块可输出更细reason，但不得擅自添加未经协商的顶层code。

## 4. 超时/崩溃/回收与幂等

- 协调器达到绝对deadline，立即撤销该request并发cancel；业务无需等待模型结束才暂停。**lease仍隔离**，不能因计时器到了就重用。
- `cancel_grace_ms=1000` 是新设计默认。逾期无release时终止worker；`process_exit_grace_ms=3000`后仍未确认退出，保留Quarantined并Paused，不无限创建新worker或新缓冲池。
- 在Windows无法确认读者退出时，宁愿保留有上限的隔离内存和暂停状态，也不复用；不能仅把本端handle关闭当成对端已不读。
- 崩溃确认：QProcess finished/平台进程句柄已退出，再关闭本地映射并清理已登记lease。worker恢复最多由显式重试策略触发新会话；旧stdout/旧release即使晚到也不作用于新池。
- duplicate release对相同旧lease幂等返回；release包含错误lease/slot_generation时记录E_LEASE_UNKNOWN并拒绝释放当前槽。
- result→release是正常顺序；release→result视为协议异常，已归还的frame数据不再用于新结果。由于NDJSON单流保序，该情况通常代表worker实现不符合协议。
- 一个request出现两个不同终结响应时E_MESSAGE_INVALID，保持首次被接受的结果；不会第二次入账。
- 断管与半行JSON：stdout EOF时未完成末行视E_MESSAGE_INVALID，所有未终结request按worker崩溃流程处理。

## 5. 平台映射方案与I/O约束

Windows候选实现使用CreateFileMapping的paging-file-backed section（INVALID_HANDLE_VALUE）及MapViewOfFile；不使用任何真实文件句柄作为像素section backing。RRT01说明共享内存创建/生命周期能力，**不保证内存永不被OS换出到页面文件**。

命名模板为 `Local\\RelinkVision_<session_id>_<slot_index>`，生产session_id含随机性；实际句柄创建和访问权限由进程适配层完成，默认只允许当前用户/本进程树必要读取，不使用Global跨会话命名。name不是访问控制的替代。Python backend应采用兼容的Windows映射读取实现；不假设Python SharedMemory与任意Qt shared memory头格式天然相同。RRT02用于了解SharedMemory生命周期，最终跨语言互操作必须单独测试。

文件IO验收分别计量：
1. 图像/临时帧文件创建次数=0、图像写入字节=0，含正常/成功/失败/cancel/crash。
2. 配置、账本、限速结构化诊断按各自预算持久化。
3. 读取可信模型/手工回放素材不是图片写入。
4. debug/manual export是单独显式操作，有数量/字节/时限上限，默认关闭；不得由error自动开启。

## 6. 保护上限与线程责任

下表全部是**拟定保护值**，schema据此约束；M2可评审新版本，但不能静默扩展。

| 参数 | v1值 | 目的 |
|---|---:|---|
| NDJSON单行 | 1048576字节 | 解析前限流，防无限累积半行 |
| 单结果tokens | 2000 | 结果内存上限 |
| 单token text | 4096字符 | 防异常输出 |
| 单帧valid_bytes | 134217728字节 | 128MiB帧上限；两槽CPU像素≤256MiB |
| pending / in-flight | 1 / 1 | 不积压旧帧 |
| ROI数 | 64 | 明确批处理上限 |
| 控制队列 | 256条 | 过载暂停，不丢Stop/Cancel |
| worker stderr | 64KiB内存环，5秒一条汇总 | 不逐帧磁盘打印 |
| handshake / cancel / exit grace | 10000 / 1000 / 3000ms | 故障界限，非实测最优 |

协调器线程处理Cancel优先级、lease表、业务Reducer；采集线程不得直接调用账本；worker只读像素与产生OCR，不决定购买或追加观察。UI命令过载时合并重复显示更新，但Stop/Pause命令由高优先级控制通道处理，不能等256条普通观测全部消费后才停。
