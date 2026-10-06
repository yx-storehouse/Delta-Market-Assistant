# ObservationDemand、时钟与帧契约

本章为新设计，证据入口E12，追踪B06/B07/B22/B35/B43/B44、WI03/WI06–WI08/WI10。WGC、DXGI和DPI背景见R03–R05；它们不证明BBZPS采用相同实现。

## 1. 一次需求是什么

`ObservationDemand` 是协调器授予特定业务步骤的**有界观察权**，不是常驻采集开关。相同run在同一时刻最多一个活跃需求；多任务在协调器串行排队，不能各自创建无限采集循环。

机器对象将run/session/clock/step/epoch/viewport这六类上下文字段统一嵌在`context`对象中；正文表格为了可读性省略`context.`前缀，不代表存在两套字段。

| 字段 | 精确定义 |
|---|---|
| demand_id / run_id / step_id | 不可复用的本次需求/运行/步骤标识；step_id每次重入状态重新生成 |
| session_id | 运行/观察会话；协调器启动或worker被替换时更换，使旧观测/lease失效 |
| clock_domain_id | 协调器本次单调时钟域；重启更换，不与墙钟换算作授权 |
| cancel_epoch | 运行取消代次；暂停/停止/模式切换/窗口失效递增 |
| viewport_generation | 客户区尺寸、DPI、目标身份或坐标变换改变时递增 |
| mode | one_shot或bounded_watch |
| created_mono_ms / not_before_mono_ms / deadline_mono_ms | 本时钟域毫秒整数；开始屏障与绝对截止 |
| max_frame_age_ms | 业务消费时允许的最大保守帧龄；不是OCR模型推理时间 |
| frame_budget | **已发出采集请求次数**预算；失败/无新帧也消耗，取消前未发出的不消耗 |
| min_interval_ms | bounded_watch两次Acquire命令的最小间隔；不是保证帧率 |
| roi_specs | 本步骤需要的ROI及变换版本；不默认为整屏 |
| purpose | page_check、listing_fields、receipt、revalidate或countdown |
| persistence | 固定none；调试导出是另一显式服务，不在OCR链路暗加开关 |

仅worker重启但协调器存活时session_id更换、clock_domain_id保持；协调器重启二者都更换。该区别保证“新视觉会话”不伪造成新的单调时钟，也不允许旧观察混入新会话。

one_shot：frame_budget=1，min_interval_ms=0；只发一次Acquire，输出一个成功/未知/失败终结结果；不得在适配器内部隐式多次截图。要重试时由协调器新建需求并消耗该步骤重试预算。

bounded_watch：frame_budget≥1且有截止；进入时最多发一次Acquire，后续仅由WatchTick且满足间隔、未取消、未达到预算、未满足终结条件时发出。**请求仍在采集时不并发Acquire**；获取到一帧并填pending后才允许下一次Acquire。帧预算耗尽后不再采集，允许已获取帧在deadline之前完成；deadline一到无有效结果则超时。

## 2. 生命周期和停止

| 状态 | 可进入事件 | 动作 | 终结/后续 |
|---|---|---|---|
| Created | EnterStep | 检查配置、帧龄、ROI、窗口身份；创建屏障 | Valid→Active；非法→Rejected |
| Active | AcquireDue | 向CAP发一次Acquire，request_count+1 | 等待FrameCaptured或CaptureFailed |
| Active | FrameCaptured | 校验屏障和代次；按双槽策略放pending | 可调用VIS，不自动增预算 |
| Active | ObservationReady | 接受有效完整结果；观察状态判定停止谓词 | Satisfied或继续Active |
| Active | BudgetExhausted | 停止新增Acquire；等待已有有效任务 | Satisfied或Deadline到达→Expired |
| Active | Deadline/Cancel/ExitStep | 取消CAP等待、VIS请求、watch timer；禁止新增请求 | Expired/Cancelled |
| Satisfied/Expired/Cancelled | 任何迟到帧/结果 | 只做资源释放，聚合迟到计数 | 不再驱动业务 |
| 终结且无读者 | LeasesDrained | 释放采集会话/该需求资源 | Closed |

终结状态是业务语义；资源draining单独显示。暂停/停止必须立即拒绝新Acquire，并取消已有捕获等待；已在OS/GPU/模型调用中的操作可能稍后返回，返回值只做资源归还。模型可继续驻内存。**不得为了等到OCR回来而继续生产帧。**

暂停后Resume不恢复旧需求/旧定时器，建立新step_id、cancel_epoch屏障，先Observe；有未决attempt时进入无采集Reconcile待核验，不直接重发。

## 3. 时钟域与freshness barrier

R08描述单调经过时间，R09说明timer调度精度约束。以下比较规则由本项目规定：

1. 协调器内部用安全整数毫秒（0..2^53−1，C++ qint64）；所有跨度由显式clock_domain_id约束。墙钟仅展示/日历计划，不用于frame age或worker超时。
2. 进入步骤记录 `not_before_mono_ms`。普通检查取进入时刻；操作后反馈取“操作可能完成的最晚时刻”；viewport改变则同时换generation。
3. 捕获记 `capture_start_mono_ms` / `capture_end_mono_ms`，含义是本次API获取区间；不得把end冒充像素实际生成时刻。
4. `FrameEnvelope.freshness_basis` 为 `source_timestamp` 时，采集适配器必须给出已经校准到同一时钟域的 `source_mono_ms`、误差上界 `source_uncertainty_ms`；只有source−uncertainty≥not_before才越过屏障。
5. 若适配器能从API保证屏障后新的完整抓取，使用 `post_barrier_capture`，要求capture_start≥not_before且适配器兼容性报告明确保证；仅“调用了GetNextFrame”不足以证明后台缓冲不是旧帧。
6. 两种证据都未具备则 `freshness_basis=unproven`，只能诊断/显示，产生E_STALE_OR_UNPROVEN_FRAME，不进入关键字段判断。
7. 消费时保守年龄 `now - lower_bound` 必须≤max_frame_age_ms，且now≤deadline；lower_bound为source−uncertainty或capture_start。未来帧、不同时钟域、end<start全部拒绝。
8. 已取消/过期结果无论OCR置信度多高都拒绝；跨步骤不能复用旧帧。对于重复画面的合法新帧，像素digest相同不代表帧过期；同样digest不同也不证明足够新。

通过上述门槛后，构造domain ListingObservation时`observed_mono_ms`取同一保守像素时间下界lower_bound，不取capture_end或OCR完成时刻；session_id/clock_domain_id/viewport_generation原样传递。这样RULE的age≤max_age比较不会把OCR处理时间隐去。

窗口移动若只影响最终屏幕变换也递增viewport_generation，使旧屏幕坐标失效。遮挡/最小化/显示器变换是否可用属于D10实测，不在本章许诺后台仍可可靠读帧。

## 4. FrameEnvelope与ROI

`FrameEnvelope` 的schema见[observation.schema.json](observation.schema.json)。v1实时像素统一CPU内存BGRA8，左上原点、每像素4字节、非压缩、正stride；这只是首轮接口选择，不声称零复制/GPU最优。宽高1..8192、字节数≤134217728均为初始保护上限，不是目标分辨率或性能结论。

- `valid_bytes = stride_bytes × height`，stride_bytes≥width×4；descriptor.byte_length必须等于valid_bytes且≤lease容量。算术先检查溢出。
- ROI坐标是**客户区物理像素**，使用半开矩形[x,x+w)×[y,y+h)。x/y≥0，w/h>0，边界超帧报E_ROI_OUT_OF_BOUNDS；不静默裁切掩盖布局错误。
- ROI携带transform_version；裁剪/缩放/二值化的变换链留内存，OCR四边形回写为原帧物理像素。若视口变更，整条变换链失效。
- window_ref是本进程登记的不可伪造引用，不把裸HWND持久化当永久身份；窗口销毁后即使句柄复用仍登记新身份。
- `source_kind=replay` 时通过受控fixture源提供内存或结构化观测；现有图片读取可以在回放源做，worker协议仍不接受路径。
- 黑屏/空帧/目标消失/设备访问丢失/OSD遮挡有独立错误；黑屏不等于空列表。

## 5. 1处理+1待处理与内存所有权

两个槽为**两个像素存储容量**，不额外隐藏第三个待识别帧。CAP获取API/GPU表面的短暂生命周期需单独计量，转换目的缓冲区不得越过这两个槽的CPU上限。

| 槽状态 | 谁可写 | 谁可读 | 下一状态 |
|---|---|---|---|
| Free | CAP可原子占用 | 无 | Writing |
| Writing | 唯一CAP写者 | 无 | Pending或Failed→Free |
| Pending | 无；协调器可先撤销未发布候选后重新Writing | 协调器元数据 | Published或被新帧覆盖 |
| Published/InUse | **无人可写** | 已授权worker | AwaitRelease |
| AwaitRelease | 无 | worker可能仍读 | 收到匹配release→Free；崩溃→Quarantined |
| Quarantined | 无 | 未确认终止的旧worker可能仍读 | 确认进程退出并关闭本地映射→Free |

Pending替换顺序：先失效旧pending元数据/lease版本、确保从未给worker发送过该lease，再把同一槽置Writing；若已经Published就属于处理槽，不能覆盖。结果被消费与lease释放是两件事；只有release/进程退出证明没有读者才能复用。

每次发布生成不可复用lease_id与slot_generation；跨session重建池。duplicate release匹配已释放lease时幂等忽略并计数；未知lease、错误slot_generation、旧session release不释放现有槽。worker只映射本次hello session中由协调器登记的命名共享内存，拒绝任意文件路径和任意陌生映射名。

## 6. M2必须实测的门槛

实测报告必须记录系统版本、GPU、目标窗口模式、DPI、采集后端/模型哈希、cold/warm区分。对WGC/DXGI分别验证：首次取帧屏障、最小化/遮挡、尺寸变化、丢设备、取消后不新增帧、零应用图片写入。任一数据正确性/生命周期必测失败不得进入后续真实观察。

延迟P50/P95/P99、字段准确率/拒识率、内存峰值和复制成本先测量，再确定产品阈值；**本文不预填“实时xx FPS达标”**。关于数据集与D09评审沿用[第05章](../../05_delivery_tests.md)。OS页面换出不是本应用截图文件，不更改系统页面文件设置。
