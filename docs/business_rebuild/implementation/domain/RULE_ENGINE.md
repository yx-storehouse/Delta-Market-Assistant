# 纯规则 Decision 契约

对应 B11/B14/B15/B16/B20，T11-A/B、T14-A/B、T15-A/B、T16-A/B、T20-A/B，WI01；事实来源 E03/E04/E08。以下优先级、原因码与接口是新设计，价格闭区间来自 ADR07，不声明原二进制比较方式已恢复。

## 1. 输入与输出

输入为已结构/语义校验的 TaskRule、ListingObservation 和显式 EvaluationContext。结构坏值在函数边界返回校验错误，不伪造 Match；C++ 建议只让 `ValidatedRule/ValidatedObservation` 进入 matcher。

输出 `Decision` 必填：status、primary_reason、reasons、rule_ref、observation_id、eligible_for_action。status 为 Match / NoMatch / NeedsReview；eligible_for_action 固定 false，表示该输出**仅做筛选说明**，不是动作授权。状态机仍需复核意图时效、已确认关联、数量预留、当前窗口、取消代次等。调用方不能只判断 Match 就发输入。

Decision 不创建新的随机 ID，不读取时间，不改变输入。reasons 使用固定顺序；主原因是有序列表第一项。Match 只返回一条 `{code:"MATCH",field:"",evidence_ref:null}`；非 Match 返回所有同一决定层级的适用原因，不混进更低层级已知不匹配。这样 UI 不会把缺单位错误显示成“价格不满足”。

## 2. 判定顺序（必须一致）

| 层级 | 条件及顺序 | 输出 |
|---|---|---|
| V0 | 结构/schema错误；跨字段语义错误 | 校验诊断；不进入 matcher，不创建 Decision |
| P1 | review_required=true | NeedsReview / RULE_REVIEW_REQUIRED；此时 enabled 必为 false |
| P2 | enabled=false 且没有待审 | NoMatch / RULE_DISABLED |
| P3 | session、clock_domain、viewport_generation 不等；observed>as_of；age>max_age | NeedsReview，按本行顺序给原因 |
| P4 | product_ref=null；association unknown/ambiguous；purpose=intent_candidate 且 provisional | NeedsReview；未知商品优先，随后关联原因 |
| P5 | 必需字段 evidence 非 observed / 值缺失；单位未知或不相同 | NeedsReview，字段次序见下方 |
| P6 | 已知字段逐个不满足规则 | NoMatch，返回所有已知失败项 |
| P7 | 没有上述失败 | Match / MATCH |

高层一旦命中不执行低层。例如价格高于上限且磨损缺失时是 NeedsReview/FIELD_MISSING，不是 NoMatch；这是为了解释“当前观测未足够可靠”，不把不完整识别充作确定规则结果。P1 先于 P2，使导入后禁用的任务仍显示待审原因而不是普通停用。

P3 同层按 session_id、clock_domain_id、viewport_generation、observed_mono_ms 顺序。上下文不一致时不跨 clock domain 算 age；先只报 CONTEXT_MISMATCH。年龄等于 max_age 允许；未来帧拒绝，不能把负数年龄截为零。max_age=0 仅接受 observed=as_of 的同时间快照。

P4 中，filter_only 允许 provisional，但不允许 ambiguous/unknown。intent_candidate 要求 confirmed，仍不自动授权操作。没有市场卖单 ID 的 confirmed 仅表示本地当前关联已明确，**不代表跨重启 exactly-once 身份保证**（D07）。

P5 检查字段顺序为 product_ref、price、wear、season、ownership、grade、condition、publicity、rarity：

- product_ref、price 永远必需；wear 仅 max_wear 非 null 时必需。
- 每个 filter 只有 op=eq 时相应字段必需；any 下的未知值不阻塞，不自动填一个值。
- 对每个必需字段，evidence status 按 missing/invalid/ambiguous 映射 FIELD_MISSING/FIELD_INVALID/FIELD_AMBIGUOUS；status=observed 但值 null 是 V0 的 EVIDENCE_VALUE_CONFLICT。
- 价格通过存在性/证据检查后，unit=null 或未注册报 UNIT_UNKNOWN；两个已知不同单位报 UNIT_MISMATCH，不尝试汇率或比例换算。
- 价格 raw 本身不在 matcher 中二次 OCR/修字；OCR 把 `O` 读成 `0` 等纠错属于上游带审计的解析，不允许 matcher 默默放宽。

P6 次序为 product_ref、season、ownership、grade、condition、publicity、rarity、price.min、price.max、wear。请求排序不参与单条 Match；“超价结束翻页”属于 B18，必须另有已证实的排序前提。

## 3. 原因码（线协议稳定英文，UI 独立翻译）

| code | status / 触发 |
|---|---|
| MATCH | Match；所有有效条件成立 |
| RULE_INVALID | 预留给上层展示校验失败；纯 matcher 不接受非法类型，不由本轮 fixtures 生成 |
| RULE_REVIEW_REQUIRED | NeedsReview；规则迁移/单位/映射待审 |
| RULE_DISABLED | NoMatch；普通停用 |
| CONTEXT_MISMATCH | NeedsReview；会话/时钟域/视口代次不同 |
| OBSERVATION_FUTURE / OBSERVATION_STALE | NeedsReview；未来观测 / 超新鲜度预算 |
| PRODUCT_UNRESOLVED | NeedsReview；商品未可靠关联目录 |
| ASSOCIATION_UNKNOWN / ASSOCIATION_AMBIGUOUS | NeedsReview；无关联 / 多候选 |
| ASSOCIATION_PROVISIONAL | NeedsReview；候选意图阶段仍只有临时关联 |
| FIELD_MISSING / FIELD_INVALID / FIELD_AMBIGUOUS | NeedsReview；必需字段缺失 / 无效 / 歧义 |
| UNIT_UNKNOWN / UNIT_MISMATCH | NeedsReview；币值单位未确认 / 不可比 |
| PRODUCT_MISMATCH | NoMatch；商品 ID 不同 |
| SEASON_MISMATCH / OWNERSHIP_MISMATCH | NoMatch；赛季 / 拥有状态不满足 |
| GRADE_MISMATCH / CONDITION_MISMATCH | NoMatch；品阶 / 成色不满足 |
| PUBLICITY_MISMATCH / RARITY_MISMATCH | NoMatch；公示期 / 稀有度不满足 |
| PRICE_BELOW_MIN / PRICE_ABOVE_MAX | NoMatch；严格低于下界 / 高于上界 |
| WEAR_ABOVE_MAX | NoMatch；严格高于磨损上界 |

reason.field 使用固定字段名；价格失败均为 `price`，观察时间为 `observed_mono_ms`。原因涉及观测字段时 evidence_ref=frame_ref；规则级原因、MATCH 时 null。message 不进入稳定判定结果，以免翻译修改改变 golden 输出。

## 4. 黄金输入的解释

[rule-golden.json](fixtures/rule-golden.json) 每组包含完整 rule/observation/context，不依赖随机源。28 组覆盖价格上下界、跨 scale 等值、最小差异、缺单位、不同单位、缺磨损、不限磨损、成色冲突、导入待审、停用、关联歧义、恰到新鲜度边界、旧视口、未来帧、品阶/稀有度独立、字段歧义、临时关联和精度错误。

G25/G26 的 expected.stage=structural_validation；G27=semantic_validation，分别期待诊断而非 Decision。其余 stage=decision。fixture 中 field_evidence.raw 是合成序列化说明，不宣称真实 OCR 模型产生了这样的文本；真实 OCR 适配器必须另有像素→字段测试。

业务测试实现时须增加组合场景（不是只跑 happy path）：同层多个失败的原因顺序；不同 clock domain 不算 age；一条规则在两次观测间更改时旧 run 仍引用原 revision；调用两次 evaluate 不改变输入/计数/文件；精度边界向两侧移动单个最小单位；null 与零互不混淆。

## 5. 与运行/数量的明确分工

- `Task.quantity` 原来只在模拟中控制演示完成次数；保留为 quantity_candidate，不直接生成正式 quota。
- matcher 不消费配额、不统计成功；已确认成功和未决预留由 runtime/账本维护。Unknown 不释放预留，重复回执按事件/回执 ID 去重。
- 不把 `limitOrange=0` 猜为“无限”或“禁止”；颜色→品阶和周期未定，执行策略未绑定。
- Match 可以展示到 UI 或提交状态机候选队列；Stop、ModeChanged、过期帧仍可使其失效。固定 Match 结果不能越过这些后续状态。
