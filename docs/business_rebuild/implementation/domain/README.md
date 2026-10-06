# M1 领域契约：模型、定点值与配置

**状态：v0.3.0 开发规格；生产引擎尚未实现。** 本目录冻结 M1 所需输入、输出、错误语义与测试数据，不表示已有实时捕获、OCR 或业务动作。规则边界、数值预算、原因码是本项目设计，不是恢复出的 BBZPS 源码事实。

对应工作包 WI01/WI02，业务 B01/B04/B05/B11/B14–B16/B20/B39/B43；证据 E03（旧任务）、E04（历史观测）、E08（现有模拟）、E11/E12（用户约定）。既有 R12 支持配置提交方式，R08 支持单调计时；引用详见 [来源总表](../../06_evidence_references.md)。运行状态、事件账本和帧协议引用相邻 runtime 契约，本文件不复制它们的枚举。

## 1. 开发者从哪里开始

| 文件 | 用途 / 是否可直接被现有 0.6.0 读取 |
|---|---|
| [core.schema.json](core.schema.json) | Draft 2020-12 共享值对象与规则/观测/Decision 定义；现有程序不读取 |
| [config-v2.schema.json](config-v2.schema.json) | 新 v2 配置精确结构；现有 `schema_version=1` 解析器应继续拒绝它 |
| [import-preview.schema.json](import-preview.schema.json) | 只读导入预览，与可加载配置是不同类型 |
| [规则判定契约](RULE_ENGINE.md) | 判定优先级、原因码、null 语义、纯函数接口 |
| [导入事务契约](IMPORT.md) | v1、13 列、逐行诊断、确认与提交 |
| [fixtures/rule-golden.json](fixtures/rule-golden.json) | 28 组完整输入和预期结果；待实现的测试规格 |
| [fixtures/import-golden.json](fixtures/import-golden.json) | 14 组迁移/故障场景；v1 场景引用已随文附带的固定基线和顺序操作 |
| [fixtures/v1.synthetic.json](fixtures/v1.synthetic.json) | 逐字节复制项目旧演示配置，SHA-256与来源在相邻 `.origin.json`；不是市场实测 |
| [fixtures/config-v2.synthetic.json](fixtures/config-v2.synthetic.json) | 完整合成回放配置；单位明确标记为合成，不是现实币种 |
| [validation_manifest.json](validation_manifest.json) | 20 组实际可跑的结构接受/拒绝例；与黄金业务测试不同 |
| [build_contracts.py](build_contracts.py) | 仅生成文档契约/合成 JSON；不导入应用或样本模块 |

所有 schema 的 `$id` 位于 `https://schemas.relink.invalid/business/v1/domain/`，**只是离线标识**；校验器从本地注册，不访问该域名。`contract_version=0.3.0` 标识规格版；`schema_version=2` 标识配置格式，二者不能混用。

## 2. 六类对象各自负责什么

| 对象 | 身份 / 生命周期 | 明确不承担 |
|---|---|---|
| `SkinCatalogEntry` | `product_id`；稳定商品资料和别名 | 不保存“该商品唯一的当前价”；不等于某个卖家的卖单 |
| `ListingObservation` | `observation_id`；一次不可变观测；同帧多行分别建对象 | 位置不是长期 ID；同特征条目不强行并为一单；不是成交记录 |
| `TaskRule` | `task_id + revision`；可版本化匹配规则 | 不保存运行进度；quantity 候选不是执行配额 |
| `TaskRun` | `run_id`；持有已冻结 `RuleRef` 与规则内容摘要 | UI 编辑不修改正在运行的规则快照；状态机由 runtime 契约定义 |
| `AttemptRecord` | `attempt_id` 关联 `intent_id/run_id/association_ref` | 发起或超时不等于成功；结果回执归账本管理 |
| `Profile` | `profile_id + revision`；选定规则修订版、模式、UI 参数 | 切方案不隐式开始；未绑定 UI 参数不解释为真实动作策略 |

引用按 ID 关联，不在每个事件中复制整个商品/规则。对象被规则引用后不可直接从配置物理删除：先禁用并生成新配置修订；已有 run 所持有的旧规则快照继续可读。M1 配置可只保留当前规则 revision；run 的不可变快照由回放/账本仓库存储，不能仅靠以后可能变动的配置文件解析历史。

## 3. 通用序列化与边界

### 3.1 类型、缺失与扩展

- 核心对象 schema 中的 `required` 是完整清单；没有 JSON 默认值注入。必填且 nullable 的字段必须写 `null`，不能省略来猜“不限”。可用默认只出现在导入适配器中并记录来源。
- `Id`：1–128 个 ASCII 字符，首字符字母或数字，其余允许字母、数字、`.`、`_`、`:`、`-`。ID 区分大小写；不将商品显示名当 ID。新建使用 UUID 文本；fixtures 使用可读固定 ID。外部卖单 ID 是独立原文字符串，不强迫符合内部 ID 语法。
- 核心对象 `additionalProperties=false`；拼错字段即错误。可扩展数据仅放 `extensions`，键必须 `x-` 开头，最多 32 个键。扩展值不参与匹配或执行。已导入但未认识的旧字段进入 `legacy_raw.unknown_json`，不提升为活动字段。
- 文档 schema 的 `maxLength` 采用 JSON 字符串长度语义；现有 Qt UI 约束为 UTF-16 单元。语义校验额外限制任务名不超过 60 个 UTF-16 单元、方案名不超过 40；含 31 个双单元字符的任务名拒绝，30 个可接受。不要用 schema 通过替代 UI 兼容检查。[E08]
- 配置 UTF-8 解码后总输入上限 8 MiB、最大嵌套深度 32，catalog/rules/migration_decisions 各最多 10,000 条，profiles 最多 100 个，units 最多 32 项。这些是 M1 资源预算，不是样本业务上限。扩展与原值同样计入总大小/深度限制。
- `null` 表示“没有已确认的值”。例如观测 price=null 是未识别，不是零价；规则 max_wear=null 明确表示不限制磨损；Money.unit=null 是未知单位，绝不代表默认人民币。

### 3.2 精确十进制（M1 冻结决定）

`DecimalValue = {unscaled: string, scale: integer}`，数值为 `unscaled × 10^(-scale)`。

| 属性 | 类型 / 范围 | 例子 / 拒绝 |
|---|---|---|
| unscaled | 非负十进制数字串，1–24 位；除 `0` 外无前导零 | `1187788`；拒绝 `-1`、`+1`、数值型 `1`、`01`、指数文本 |
| scale | 整数 0–12 | `1.187788 → {"unscaled":"1187788","scale":6}` |
| zero | `unscaled="0"`，scale 可为 0–12 | 保留输入精度；比较时全等 |
| overflow | 超 24 位或 scale>12 | `DECIMAL_CAPACITY_EXCEEDED`，进入 review/诊断，不截断、不四舍五入 |

比较不转 `double`：将两数对齐到较大 scale，右补至多 12 个零；比较有效数字串长度，再做字典序比较。中间串最多 36 位；不要求 64 位整数容纳。`0.399` 与 `0.399000` 相等；`0.399000000001` 严格更大。原输入仍在 raw/evidence 中保留。零的规范显示由 UI 决定，不改变值。

`Money = {value: DecimalValue, unit: Id|null}`。仅相同且已确认单位可比较。单位表 `UnitDefinition` 明确 `unit_id/display_name/quantum/reviewed=true/synthetic`；quantum 必须严格大于 0，表示拟支持的最小显示步长。所有进入活动规则的金额必须是 quantum 的精确倍数；不是倍数则 `UNIT_QUANTUM_MISMATCH`，不能静默舍入。合成测试 quantum 为 `0.000000000001`，**并未断言现实市场有这种精度**。

TaskRule 价格为闭区间 `[min,max]`，磨损条件为 `wear <= max_wear`；这是 ADR07 的新设计。规则 min/max 必须同单位且 min<=max。价格/磨损不得负数。没有从样本推断出真实金额币种、最大值或合法磨损精度；超过项目表示预算的原值保留并报告，不据此声称业务非法。

### 3.3 时间与视口

`MonoMs` 是 JSON 整数 0–9,007,199,254,740,991，落地用 `qint64`。单位毫秒，基准是 `clock_domain_id` 指定的单调时钟；不是 UTC。runtime 在不同进程间传相对预算，不直接比较两个进程本地绝对计时值。观测必须同时匹配 session、clock_domain、viewport_generation；任何一个不符视为陈旧上下文，不能拿时间接近来补救。[R08；设计]

M1 `max_age_ms` 为调用者显式注入的 0–60000 ms，未设置就是配置错误，不存在隐式 1000 ms 默认；黄金用例采用 1000 ms 仅为了边界测试。真实产品的各步骤新鲜度在 M2 测量后配置，不把测试值宣传为性能目标。

runtime→domain 的时间映射固定为：先通过 ObservationDemand 的屏障与 FrameEnvelope 新鲜度证明，再将 `ListingObservation.observed_mono_ms` 设置为保守像素时刻下界（已校准 `source_mono_ms - source_uncertainty_ms`，或经后端证明的 `capture_start_mono_ms`）。不要使用 capture_end 或 OCR 完成时间来让旧图看起来更新。source下界为负、时钟域未知或 `freshness_basis=unproven` 时不构造可判定观测，留在视觉诊断层。

## 4. 字段字典（schema 为机械结构权威）

### 4.1 商品与观测

| 字段 | 必填 / 可空 | 业务语义 |
|---|---|---|
| catalog.product_id/name | 必填 / 否 | 稳定内部 ID；name 最多 256 字符，非空白 |
| catalog.series/season/grade/rarity | 必填 / 是 | 独立资料；grade 与 rarity 不互相补全 |
| catalog.aliases | 必填 / 否 | 0–32 个候选名；名称归一化不能覆盖原名 |
| catalog.source_kind | 必填 / 否 | native / legacy_v1 / legacy_13 / synthetic |
| observation.product_ref | 必填 / 是 | 未完成可靠目录映射用 null；不能仅凭模糊名称自动选商品 |
| observation.market_listing_id | 必填 / 是 | 真实界面提供的可见 ID 原文；现证据尚未确认存在 |
| observation.association | 必填 / 否 | association_ref 可空；status=confirmed/provisional/ambiguous/unknown；仅是本地关联判断 |
| observation.price/wear | 必填 / 是 | Money / DecimalValue；缺失不是零；成色不能反推 wear |
| observation.condition | 必填 / 是 | S/A/B/C；暂不支持的文字保留 raw 并进入 review，不新增隐藏默认 |
| observation.season/grade/rarity | 必填 / 是 | 经目录字典确认的字符串 token；长度 <=128；未识别为 null |
| observation.ownership | 必填 / 是 | owned/unowned；未知为 null；不从数量 0 推断 |
| observation.publicity | 必填 / 是 | active/ended/none；未知为 null；与截止时间估计分开 |
| observation.frame_ref | 必填 / 否 | 内存帧逻辑 ID，不是磁盘路径；帧释放后不保证还能获取图片 |
| observation.session_id/clock_domain_id | 必填 / 否 | 观测归属的会话与时钟域 |
| observation.observed_mono_ms/viewport_generation | 必填 / 否 | 观察时刻与窗口视口代次，不是 OCR 完成时刻 |
| observation.field_evidence | 必填 / 否 | product_ref/price/wear/condition/season/ownership/grade/rarity/publicity 均有 evidence 条目 |

`FieldEvidence.status=observed/missing/invalid/ambiguous`；raw 可空或 0–4096 字符；confidence 是 null 或 [0,1]；source_ref 指向帧/合成源 ID。M1 不擅设统一 OCR 分数阈值：合成/可信回放可用 confidence=null；M2 视觉适配器依据评测先把输入标为 observed 或拒识，再交领域层。证据与值冲突是 `EVIDENCE_VALUE_CONFLICT` 语义错误，不由 matcher 猜哪个正确。

confirmed/provisional 必须带 association_ref；unknown 必须为 null；ambiguous 可为 null 或一个歧义组 ID。market_listing_id 非 null 也不自动将关联升为 confirmed。绑定 ID 的生成/比较策略要经视口与条目观测验证，不能将屏幕坐标串当平台 ID。

### 4.2 TaskRule

| 字段 | 类型 / 约束 | 行为 |
|---|---|---|
| task_id/revision | Id；整数1–2147483647 | 修改任何活动规则条件生成新 revision；不改旧快照 |
| name | 非空文本，<=60 UTF-16单元 | 展示标题，不决定身份 |
| product_ref | Id/null | reviewed 规则必须存在且引用 catalog |
| enabled | bool | false 不参加匹配；review 规则只能 false |
| review_required/review_codes | bool / 最多64个Id | true 时至少一个原因；false 时空列表 |
| filters | 六个必填 Selector | season/ownership/grade/condition/publicity/rarity；any 与 eq 明确分开 |
| price_range | `{min:Money,max:Money}`/null | reviewed 规则必填非空，单位/边界通过语义校验 |
| max_wear | DecimalValue/null | null 明确为不限制；有值时含等于上界 |
| requested_sort | default/price_asc/price_desc/rarity_asc/rarity_desc/unknown | 请求排序，不是已验证排序；matcher不据此提前结束扫描 |
| quantity_candidate | null 或整数1–9999 | 只用于兼容 UI/导入展示；不是目标成交量或额度 |
| quantity_semantics | unreviewed/demo_count_only/none | 原v1 quantity只可确认演示计数；正式限购仍待 D05 |
| legacy_raw | LegacyRaw/null | 原文/行号/哈希/各列/未知非凭据字段 |
| extensions | object | 只存元数据，不参与执行 |

`Selector` 为 `{"op":"any"}` 或 `{"op":"eq","value":...}`，不使用 `"不限"` 兼作业务 token。condition eq 仅 S/A/B/C；ownership eq 仅 owned/unowned；publicity eq 仅 active/ended/none。season/grade/rarity eq 值必须能由配置字典审核过程解释；未审的导入 token 保留候选，不假装已知。

### 4.3 Profile 与界面参数保存

Profile 必须含 profile_id、revision、name、mode、rule_refs、ui_run_settings、execution_policy、schedule_timezone、extensions。mode=demo/replay/observe；**M1 只允许 demo/replay 启动，observe 显示尚未安装适配器；配置可保存未来选择但不得默默改成 demo。** execution_policy 固定 `not_bound`；UI 参数值与业务启用分离。

ui_run_settings 继续保存现有所有字段及现有范围：purchaseDelayMs 0–60000；queueFullTrigger/publicityTrigger 1–9999；步长 0–1000、最多 1 位小数；clickIntervalMs 1–10000；橙紫蓝数量 0–9999；快捷键 F1–F12；scheduleStart/Stop 为 HH:mm；其他开关为严格 bool。浮点配置噪声兼容由 v1 适配器处理，v2 新输入语义按十分之一毫秒精确检查。schema 对这两个保留的 UI 小数字段只校验数值类型和范围；实现按 JSON 数字 token 检查最多一位有效小数，不依赖使用浮点实现的 multipleOf 来作精度验收。

schedule_timezone 可空；启用真实定时前必须非空且对应支持时区。M1 仅保留 UI 的 scheduleEnabled 和时间，回放由注入时钟驱动，不用未定义市场开放规则启动目标窗口。

### 4.4 配置根与语义校验

根必填 schema_version=2、contract_version=0.3.0、config_id、revision、catalog、rules、profiles、active_profile_id、units、migration_decisions、extensions。JSON Schema 结构校验后，还必须执行以下跨字段校验：

1. catalog product_id、rule(task_id,revision)、profile(profile_id,revision)、unit_id、migration decision_id 唯一；M1 当前配置每个 task_id/profile_id 只保留一个当前 revision。
2. rule.product_ref、profile.rule_refs、active_profile_id 全部可解引用；不可悬空引用或默默丢弃。
3. Decimal 容量、Money 单位与 quantum、min<=max、字符串非空白、UTF-16 UI 长度、扩展预算、evidence 一致性符合上述规则。
4. reviewed 规则没有未决项；可配置的未知候选必须 review_required=true、enabled=false。
5. quantity_candidate=null 对应 quantity_semantics=none；非空时仅 unreviewed/demo_count_only；不解释为无限配额。
6. 同一配置不能用同一个 ID 指代两个不相同实体；输出顺序保持输入顺序，不靠 map 排序改变用户任务先后。

## 5. 领域接口签名与实现文件建议

以下是拟实现接口，不是当前 src 中已存在的代码；可由开发者等价实现，但输入/输出/错误语义必须保持。C++17 使用 `std::variant` 表示 Result，不依赖新标准的 expected。

```cpp
// Proposed: src/business/value_types.h, models.h, validation.h
namespace relink::business {
struct Diagnostic { QString code, field, message; int line = 0, column = 0; };
template<class T> using Result = std::variant<T, QVector<Diagnostic>>;
struct DecimalValue { QString unscaled; quint8 scale = 0; };
enum class Ordering { Less, Equal, Greater };
Result<DecimalValue> parseDecimal(QStringView raw, const DecimalParsePolicy& policy);
Ordering compareDecimal(const DecimalValue& a, const DecimalValue& b);
Result<ValidatedRule> validateRule(const TaskRule&, const Catalog&, const UnitCatalog&);
Result<ValidatedObservation> validateObservation(const ListingObservation&);
Decision evaluate(const ValidatedRule&, const ValidatedObservation&, const EvaluationContext&);

// Proposed: src/business/import_preview.h, config_repository.h
Result<LegacyImportPreview> previewV1(const QByteArray& source, const SourceDescriptor&);
Result<LegacyImportPreview> preview13Columns(const QByteArray& source, const ImportOptions&);
Result<ConfigV2> materializeCandidate(const LegacyImportPreview&, const ReviewDecisions&);
Result<ValidatedConfig> validateConfig(const ConfigV2&);
Result<CommitReceipt> commitNewConfig(const ValidatedConfig&, const CommitOptions&);
}
```

Domain 库不依赖 QWidget/QTimer/捕获器/数据库连接；调用者提供所有上下文。matcher 不读取全局时间、不写日志/文件、不更改数量、不发动作。导入预览只读输入；提交仓库独立承担磁盘副作用。所有函数遇到 schema 不支持/超预算给结构化诊断，不通过异常泄漏一半变更的配置。

建议测试落点（将来创建）：`tests/business/value_tests.cpp`、`rule_tests.cpp`、`import_tests.cpp`、`config_repository_tests.cpp`。黄金 fixture 测试只加载本目录固定输入，不启动样本或真实目标。相同 rule/observation/context 必须得到字节稳定的规范化 Decision。

## 6. M1 完成边界与尚未冻结的内容

已冻结：定点表示预算；闭区间；空值语义；规则优先级；预览/配置类型分离；v1 兼容映射与 13 列 raw 保留；整份原子导入；错误原因；字段单位；合成测试例。

仍待 D01/D02/D03/D04/D05/D07/D13：未知尾列、样本价格列语义确认、原时间单位/动态公式、正式限购周期/颜色映射、稳定卖单 ID、现实金额/精度/目录字典。这些不阻塞值对象/纯规则/只读预览/合成回放开发；只阻塞相应规则的正式激活。禁止以“先开发”为由填入猜测默认。

结构通过只代表 JSON 契约成立。业务黄金案例尚待 C++ 引擎实现后执行；OCR 准确率、真实视口行为、实际磁盘写入压力不由本目录的 schema 测试证明。
