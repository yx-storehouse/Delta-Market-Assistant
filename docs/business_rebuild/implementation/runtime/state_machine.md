# 可实现事件状态机、取消与未决预留

本章将[第04章](../../04_state_machine.md)落实为M1可回放的契约。所有状态/字段为重构设计，历史业务路径依据E04，按需采集依据E12。真实点击/购买仍未接入；下表“发起”在M1仅为FakeAction的显式事件。

## 1. 状态、事件、效果分离

`reduce(snapshot, event) -> {next_snapshot, effects[]}` 为纯函数。快照包含TaskRun、attempt摘要、当前step/demand和代次；不得读取系统时间/截图/文件。时间、领域Decision、账本提交/适配器回执均由event显式输入。effects是发给边界适配器的命令，不是已经发生的结果。

- run_state：运行当前位置；attempt_state：单次意图账本状态，二者分开。
- event_id全局去重；idempotency_key用于持久化事务去重，不以屏幕坐标构造。
- `seq`由协调器接收时单调分配；fixture数组顺序就是仲裁顺序。外部producer自己的序号不改变已经仲裁的先后。
- run/session/epoch/viewport/step过期事件仅增聚合计数；release_frame不走业务Reducer，独立进入lease表回收。
- Pause/Stop/ModeChange/WindowLost在一个排空批次中优先于普通观测；单线程Reducer已经提交的账本事务不“时间倒流”撤销。fixture给出的顺序是经过该优先级规则后的顺序。
- UI仍可接收统计/状态投影；停止不消除未决账本，不删除审计证据。

**阶段能力门先于状态guard**：M1只允许Replay+FakeCapture/FakeAction，Profile中Observe只保存不可启动；M2通过只读观察门槛后才启用Observe。Observe模式无论auto_collect是否为true都不生成收藏/输入命令、不申请购买数量。domain.execution_policy=not_bound不得被UI或runtime自动解释为真实授权；合成quota仅从回放fixture/自有测试适配器显式注入，不能从旧Profile猜出。

Start同时检查adapter_available（目标模式所需适配器已配置并通过版本/能力检查）；缺任何必需适配器留Ready并显示具体错误。Demo沿用现有前端模拟路径，不在本文RunCoordinator里冒充Replay/Observe。EvaluateRule完成事件必须携带domain完整Decision（rule_ref、observation_id、primary_reason、reasons、eligible_for_action=false）；正文及小型回放fixture中的decision字符串只是预先注入Decision的判定投影，不是生产接口只返回布尔/枚举。UI保存primary_reason与全部原因列表。

## 2. 完整转换表

“观察”列=进入后由协调器提出需求；每次重入观察状态创建新step_id和屏障。短时观察有显式frame_budget/deadline。表中Receipt必须通过关联、代次和语义校验。

| 当前状态 | 事件 / guard | effects（完成与发命令分开） | 下一状态 | timeout / cancel / error |
|---|---|---|---|---|
| Ready | Start；配置已审、模式通过阶段能力门 | 冻结规则快照；未到计划则设唤醒，否则Demand(page_check) | AwaitSchedule或Observe | 配置未审/Observe尚未启用留Ready；不采集 |
| AwaitSchedule | ScheduleDue；计划及市场时段有效 | 新step，Demand(page_check) | Observe | 未知市场时段→Paused；长等待0采集 |
| Observe | PageRecognized；页类可信 | 收藏路径→筛选命令；仅关注路径→观察关注页 | ConfigureFilter或Watchlist | 未知页预算耗尽→Recover |
| ConfigureFilter | FilterConfirmed；回读与当前规则匹配 | Demand(listing_fields) | ScanListings | 错误/超时→Recover；不假设点击成功 |
| ScanListings | ListingObserved；新鲜且关联可判 | EvaluateRule命令 | Evaluate | 缺字段→NeedsReview记录后下一项 |
| ScanListings | EndOfPage；非重复游标 | 在测试适配器提出翻页意图，页回读 | ScanListings | 重复页/预算耗尽→Watchlist或下一任务 |
| Evaluate | RuleDecision=NoMatch/NeedsReview | 记录原因，推进游标 | ScanListings | 不满足不生成动作/预留 |
| Evaluate | RuleDecision=Match；autoCollect且Replay/FakeAction或显式自有测试模式 | 提出收藏测试意图，Demand(receipt) | AwaitCollectReceipt | 收藏未知→Reconcile（collection种类、不占购买数量） |
| Evaluate | RuleDecision=Match；Observe或自动收藏关闭 | 仅发布匹配记录，推进游标 | ScanListings | 不接真实动作 |
| AwaitCollectReceipt | CollectReceipt=success且关联正确 | 记入关注关系；游标继续 | ScanListings | unknown→Reconcile，不重复收藏 |
| AwaitCollectReceipt | CollectReceipt=full | 记满额事件、保存收藏游标 | Watchlist | 满额值未知，不硬编码数目 |
| Watchlist | CandidateReady；无未决冲突 | 记录候选/时间计划 | AwaitDeadline或Revalidate | 目标消失/歧义→Recover或Reconcile |
| Watchlist | QueueEmpty且无未完成收藏 | 持久化结束原因 | Completed | 完成不批量改成功 |
| AwaitDeadline | DeadlineWake；计划代次有效 | 新屏障、Demand(revalidate) | Revalidate | 长等待0采集；旧唤醒丢弃 |
| Revalidate | Revalidated；领域Match、稳定关联、时间条件满足、显式合成执行策略且非Observe | RequestReservation原子申请 | Revalidate（reservation_pending） | 失效/NoMatch→Watchlist；未知→Recover |
| Revalidate | Revalidated；Observe模式 | 只发布匹配/时间状态，不申请额度或发动作 | Watchlist | 模式优先于autoCollect和旧Profile参数 |
| Revalidate | ReservationGranted；同intent且额度足 | 记Prepared；发FakeActionCommand | AwaitConfirm | 预留失败留Watchlist并说明 |
| Revalidate | ReservationDenied=unresolved_conflict | 说明现有未决占用额度，不再派发意图 | Reconcile | 不自动观察/重发；quota_full且全为成功则Completed |
| AwaitConfirm | ActionPossiblySent | 账本记Dispatching/Sent，Demand(receipt) | AwaitReceipt | 错页/失焦/未知→Reconcile |
| AwaitConfirm | DefinitelyNotSent且有强适配器证明 | 记Cancelled，释放对应预留 | Watchlist | “没收到ack”不算未发送证明 |
| AwaitReceipt | Receipt=success/failed；归属明确 | CommitReceipt事务请求 | PersistResult | 重复回执只去重，不重复计数 |
| AwaitReceipt | Deadline/Receipt=unknown/WindowLost | CommitUnknown，保持预留 | Reconcile | 不盲重发 |
| PersistResult | LedgerCommitted；事务ID匹配 | 以账本返回更新投影；可在测试模式退出结果页 | Watchlist或Completed | LedgerFailed→Paused，未提交前不显示成功 |
| Reconcile | RequestReconcile；显式用户/策略请求 | 新step，Demand(receipt)，不发原操作 | Reconcile | 无观察申请时0采集；有界超时仍Unknown |
| Reconcile | Receipt=success/failed；唯一关联 | CommitReceipt事务 | PersistResult | 歧义继续占预留 |
| Reconcile | Continue；全部未决已解释 | 新屏障，重新Observe | Observe | 尚有未决则留Reconcile |
| Recover | RetryObserve；还有恢复预算 | 新屏障，Demand(page_check) | Observe | 预算/截止耗尽→Paused |
| Paused | Resume；无未决 | cancel_epoch递增，新step，重新Demand | Observe | 不恢复旧定时器/旧帧 |
| Paused | Resume；有未决 | 展示未决，等待RequestReconcile | Reconcile | 不自动重新发起 |
| Stopped/Completed | StartNewRun | 新run_id、重读账本额度、重新验证配置 | Ready→Observe/AwaitSchedule | 不延用旧epoch或归零未决 |
| 任一非终态 | Pause | epoch+1；取消全部需求/定时器；处理未决 | Paused | 立即禁止新Acquire/意图 |
| 任一状态 | Stop/ModeChange | epoch+1；取消需求；未发准备可撤销、可能发出→Unknown | Stopped | 未决数可>0；资源draining独立 |
| 有活跃步骤 | ViewportChanged/WindowLost | viewport+1、epoch+1；丢帧/坐标/意图 | 无可能发出→Recover；否则Reconcile | 不自动沿用旧ROI |
| 崩溃后加载 | RestartLoaded | 新session/clock；旧dispatching/sent→Unknown；prepared检查发送日志 | 有未决→Reconcile，否则Ready | 不自动续播外部操作 |

每个Observe类状态都用[ObservationDemand](observation_contract.md)。配置后的输入行为仍只生成测试意图，不能绕过M1–M4阶段边界。

## 3. 取消与“是否发出”的线性化点

业务取消在协调器消费Pause/Stop的那一刻生效：epoch+1后，任何旧epoch事件不能新建动作或新预留。它**不倒推外部操作从未发生**。

| attempt状态 | 取消处理 | 额度 |
|---|---|---|
| Prepared且命令尚未交付动作适配器（明确本地outbox未派发） | Cancelled | 释放 |
| Dispatching（outbox已标记，外部是否收到未知） | Unknown | 保留 |
| Sent/AwaitingReceipt | Unknown | 保留 |
| Success/Failed/Cancelled终态 | 不变 | 幂等 |
| Unknown | 不变；停止自动动作，只允许后续核验 | 保留 |

发起顺序：事务持久化Prepared+reservation → 事务标记Dispatching → 交付适配器 → 收到明确ack可记Sent。崩溃可能发生在每两个动作之间，因此Dispatching即占未决额度。不能用“Prepared通常还没点”来批量释放恢复现场；必须有可审计的outbox状态证明尚未交付。

从Stopped/Paused收到旧视觉回执只丢弃；若后续人工/独立核验得到有效receipt，则以**新会话的ReconcileReceipt事件**进入账本，不让旧取消上下文直接驱动流程。

**账本提交响应不是旧视觉回执。** 已发CommitReceipt的transaction_id/idempotency_key被本地存储适配器确认提交后，即使Stop/Pause已增加epoch，仍须更新该attempt/额度投影；验证对象是未决事务登记与数据库提交事实，不用旧step/epoch拒绝真实持久化结果。此响应不得让Paused/Stopped回到Watchlist或开始采集。重启时通过读账本恢复已提交事实，不信任前一进程遗留的回调消息。RT-15覆盖“先请求成功入账，随后暂停，最后提交ACK”的竞态。

## 4. 数量授权与幂等事务

领域Decision=Match只表示字段符合。授权需要原子比较以下谓词，对task和已明确周期/品阶的各quota bucket同时检查：

```text
confirmed_success + unresolved_reservations + requested_quantity <= target_quantity
```

M1用显式合成quota bucket（例如task/run），不把D05未确认的真实限购周期/颜色映射偷设默认。requested_quantity正整数，第一版测试intent=1。

一笔事务：锁定/条件更新所有相关bucket → 同一intent_id幂等查重 → 写reservation与Prepared attempt → 提交；任一bucket失败全部回退。UI缓存不能参与额度判断。对两个并发申请只允许账本仲裁后的一个成功；纯Reducer也按回执顺序更新快照。

回执事务以 `attempt_id + receipt_identity` 去重，验证attempt关联：
- Success：预留转confirmed一次；同一成功重复不加数。
- Failed：释放预留一次，confirmed不变。
- Unknown：保留预留、保留关联；之后只在明确证据下转Success/Failed。
- Success与Failed相冲突：E_RECEIPT_CONFLICT，冻结该attempt人工核验；不采用“最后一条覆盖”。
- unrelated/ambiguous receipt：记未关联诊断，不变更额度。
- 数据库存储失败：业务进入Paused，成功UI投影未提交不更新；重试使用原事务/幂等键，不新建intent。

## 5. 无序、重复、超时优先级

事件接受检查按顺序：结构合法 → event_id去重 → run/session匹配 → cancel_epoch匹配 → viewport匹配 → step_id匹配 → 截止/帧龄 → 业务guard。停止事件从控制通道进入，可不携带旧step授权；但仍需当前run/session。

- deadline恰好等于now时允许已产生且本批仲裁在Deadline之前的有效结果（消费条件now≤deadline）；Deadline事件先仲裁则需求终结，后续同毫秒结果无效。fixture序列给出唯一顺序。
- 定时器晚到不补发一串积压tick，只对当前now评估一次；多次过期WatchTick合并。
- Receipt与Timeout同时间不靠墙钟排序；以协调器seq决定，Timeout后Receipt需通过新核验事件。
- worker duration只统计性能，不作为事件因果时刻。
- 未识别到图像变化不是点击失败证明；无明确回执一律Unknown而非无限重试。

## 6. 回放fixture执行合同

[fixtures/replay_cases.json](fixtures/replay_cases.json)含初始快照、按序事件、期望最终状态/事件/效果计数。每条都标 `planned_not_run`；本轮只做JSON/schema/内部一致性检查。

夹具规则：
1. initial是已到达检查点，不重跑entry effects；所有计数从0开始。第一条事件后才计数。
2. 输入event.context包含完整上下文；event mono_ms只在本fixture时钟域内前进，个别旧事件的event_time可旧但ingest顺序仍数组顺序。
3. `ObservationRequested`每创建需求计一次demand_count；`AcquireFrame`每向FakeCaptureSource申请计capture_request_count，one_shot第一次立即请求。
4. `ObservationReady`由假源输入，代表采集+识别已完成的合成业务观测，不用它证明OCR正确；真正worker协议另测消息/租约。
5. expected.required_events是必须按给定顺序出现的业务事件子序列；forbidden_events必须零次；控制/资源计数全部精确比较。
   expected.exact_events若非null则逐项比较**完整的对外业务事件流**，不允许额外业务事件；effects请求计数独立。RT-01给出完整首纵切片事件流，后续实现扩展场景时应逐步收紧为exact_events。
6. fixture内默认图像文件写入次数=0是预期，不是已测结果；M2必须在真实进程I/O追踪中重测。
7. quota数值从attempt集推导并验证一致，不允许只改expected“让测试通过”。

补充确定性规则：fixture初始step_id均为s0；每次进入新业务步骤（包括PersistResult）分配s1/s2顺序ID；Pause/Stop仅增epoch，不占新step序号；Resume增epoch并创建新step。AwaitDeadline夹具的schedule_generation初值固定0。事件at_mono_ms为输入被仲裁的本地时刻；相同event_id重送仍先去重。每个需要观察的entry先产生ObservationRequested再立即发一次AcquireFrame；Watchlist新进入也观察一次，只有Completed/Reconcile/Paused等非观察状态为0。Unknown在协调器内立即进入保守未决状态并发UnknownCommitRequested，成功/失败计数必须等LedgerCommitted。初始快照没有列出的活动timer/demand都视为不存在；AwaitReceipt/Observe的检查点用于测试业务事件，不倒算进入该状态前的采集。

## 7. M1完成的可检验定义

实现至少覆盖：所有fixture逐项比较；新的事件无法绕过停止/代次；Domain NeedsReview不变成授权；Unknown保留额度；重启不重发；结构化账本提交前UI不报成功；控制器运行不加载Windows捕获或模型；所有未实现适配器在模式标识中明示。M1通过后才开始M2只读像素链路。
