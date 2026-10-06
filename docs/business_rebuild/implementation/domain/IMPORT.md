# M1 只读导入与配置事务

对应 B05/B39、T05-A/B/T39-A/B、WI02；E03 证实 `[SkinTasks] tasks=` 及带缩进的 15 条 13 列文本，E08 证实现有 schema v1 行为。以下错误码、事务与审查机制是新实现设计，不是原样本的内部算法。[R12 支持 QSaveFile 提交机制]

## 1. 两条输入路径，不把预览当配置

1. `previewV1`：读取当前项目 `schema_version=1,demo=true` JSON；目标是保存兼容信息，并形成明确标为 demo 来源的候选。
2. `preview13Columns`：读取纯 13 列任务文本，或显式 `container=ini` 时只提取 `[SkinTasks]` 的 `tasks` 键及缩进续行。编码与容器由输入选项决定；不根据文件扩展名执行任何程序。

输出始终为 `LegacyImportPreview`，`committable=false`。即使解析全部成功也必须先完成 ReviewDecisions，再 materialize 为 ConfigV2，做整份结构/语义校验后提交。预览中的 candidate 只用于展示，不直接加入正在运行的任务。

## 2. 字节、编码和行解析

### 2.1 边界

- 文件原始字节上限 8 MiB；任务行上限 10,000；单物理行上限 65,536 个解码字符；字段值上限服从 schema；超过即给 FILE_TOO_LARGE / TOO_MANY_ROWS / LINE_TOO_LONG，不截断继续。
- 默认 UTF-8 严格解码；UTF-8 BOM、UTF-16LE BOM 可显式识别；无 BOM 的 GB18030 仅用户选择编码后重试，记录 `GB18030-explicit`。错误字节不替换成问号，报 BAD_ENCODING。
- 支持 CRLF/LF；源 SHA-256 对原始字节计算；行号为物理行 1 起。raw_line 保留解码后的物理文本但不含行终止符；续行缩进、空白、原始字段内容保留。用于计算的 token 可 trim 外围空格，不改变 raw。
- INI 只识别普通 `[section]`、`key = value`、tasks 的空格/TAB 缩进续行；不支持变量插值、include、命令、转义执行。只有在当前键为 `[SkinTasks].tasks` 时，缩进非空无 `=` 的行才作为任务续行；其他情况报 ORPHAN_CONTINUATION。
- section/key 用 ASCII 大小写不敏感匹配；同一 section 下重复 key 报 DUPLICATE_KEY，指出首行和重复行。续行中的 `|` 是字段分隔；没有引号转义规则，商品名若含 `|` 报 COLUMN_COUNT，不能猜测拼接。
- 空行和行首 `;` / `#` 注释不生成任务，但计入物理行号。任务字段中间的 `#` / `;` 不是注释；不截掉商品原名。

### 2.2 INI 中不导入的内容

`userinfo`、`GlobalSettings.spt`、token/password/secret 等凭据字段不进入候选或导出；诊断仅记“字段排除”和行号，不记值。其他非任务的历史运行参数保留为有界且脱敏的 metadata，不自动换算时间单位，不自动启用动态参数。任何路径/URL只是数据，不下载、不连接、不加载 DLL。

## 3. 13 列逐列映射

| 列（1起） | 候选位置 / 处理 | 人工确认 |
|---|---|---|
| 1 商品 | 目录名称候选；product_ref 初始 null | 映射到目录 ID；多个同名不得自动取第一项 |
| 2 赛季 | 全部赛季→any，其余 eq 候选 | 未知字典值需确认 |
| 3 拥有 | 未拥有→eq unowned，已拥有→eq owned，不限→any | 未识别文本保留并 review |
| 4 品阶 | 独立 grade 候选 | 不能用 Skin.rarity 或颜色反推 |
| 5 成色 | 成色S/A/B/C→eq S/A/B/C，不限→any | 其他文本 review；不从磨损补成色 |
| 6 公示 | 不限公示期→any；其他原文候选 | active/ended/none 的界面对应未明确前 review |
| 7/8 价格 | min/max 候选，显式 DecimalValue | 人工确认列语义、闭区间和单位 |
| 9 磨损 | max_wear 候选，DecimalValue | 该规则阈值不是全市场成色定义 |
| 10 稀有度 | 所有稀有度→any；其余独立 rarity 候选 | 不覆盖第4列 |
| 11 排序 | 默认排序→default；支持的精确字符串才映射其他枚举 | 未知排序保留，requested_sort=unknown |
| 12/13 未知 | 原位保留 columns[11]/columns[12] | 不映射数量；即使显示 `7|9` 也不猜 |

raw 13 列永久保留，不因为后来确认映射就丢弃。第 12/13 列只可确认“保留为未解释 metadata、不启用相应语义”，不是伪造解释。若用户要求忠实复现这两列含义，该功能仍待 D01；不妨碍其余明确新规则独立建立。

所有合法解析行初始 `enabled=false, review_required=true`。导入价格/单位/目录字典候选未确认前不能放入已审规则。已确认的原生任务采用闭区间不是默认复制旧比较方式（D02）。

## 4. 数字文本解析

M1 默认接受 ASCII 数字的两类语法：

```text
plain:   ^[0-9]+(?:\.[0-9]+)?$
grouped: ^[1-9][0-9]{0,2}(?:,[0-9]{3})+(?:\.[0-9]+)?$
```

外部空白先 trim；合法分组 `1,000` 去逗号；`1,00` 报 DECIMAL_GROUPING_INVALID；`2O0` 报 DECIMAL_INVALID，不猜字符。指数、小数逗号、货币符号、负数、`NaN/Infinity` 均不接受。前导零可在文本解析中规范化到 unscaled，但 raw 保留；小数尾零保留 scale。规范化后数字串超过 24 位或 scale>12 报容量错误，不四舍五入。

当第7/8列形式均合法但 min>max 时，行状态 invalid，PRICE_RANGE_REVERSED，column=7；不要用“旧语义未知”把明显倒置自动交换。零价可表示，unit 仍要确认；空串不是零。

## 5. schema v1 兼容规则（基于当前代码，不扩大旧接受范围）

| 旧字段 | 迁移原则 |
|---|---|
| schema_version/demo | 必须为1/true；其他值 VERSION_UNSUPPORTED / V1_NOT_DEMO，不猜成回放 |
| skins.id/name/series | 转目录候选并保留原 ID 映射；原 ID 不合新 Id 语法时生成新 ID，写 id_map，不改源 |
| Skin.price/wear/change/followed | 留作合成观察/展示来源，不宣称实时市场；变化率与收藏演示不进成交账本 |
| Skin.rarity | 原字符串保留；不猜它究竟是新 grade 还是 rarity，待字典确认 |
| Task.minPrice/maxPrice | 继承旧2位小数/UI范围校验；通过后转换精确十进制候选；合成单位单独标识 |
| Task.maxWear | 继承旧最多6位、0–100约束；不是现实磨损合法范围证明 |
| Task.condition 缺省 | 明确映射 any；当前旧解析器允许任意非空文本，因此非 S/A/B/C/不限 进入 review，而不是静默丢弃 |
| Task.quantity | 1–9999；quantity_candidate，quantity_semantics=demo_count_only；不映射正式额度 |
| Task.enabled | 原值进入 legacy metadata；新导入候选仍禁用待审，不因旧true立即开始 |
| Task.status | 不迁移为订单状态；当前序列化本来就不保存执行状态 |
| run_settings 缺省 | 使用当前 RunSettings 明确默认表，逐字段记录兼容来源 |
| run_settings 部分缺字段 | 与当前解析器相同逐字段补默认；已有字段必须类型/范围合法 |
| 未认识 JSON 字段 | 保留脱敏到 unknown_json；记录 JSON Pointer，不使其参与运行 |

v1 校验必须保留现有 transactional 行为、ID唯一性、skin引用、数量、UI范围和 UTF-16 长度要求（见现有 `tests/domain_tests.cpp`）。旧价不是粗暴强制两位四舍五入：先按当前 `hasDecimalPrecision` 容忍二进制表示噪声，只有通过旧验证才按目标 scale 规范化。例如 `0.30000000000000004 → 0.30`，写 V1_FLOAT_NOISE_NORMALIZED；`0.301` 拒绝。raw 数字的原始 JSON token 或来源字节范围必须记录，以便审计；不能在解析到 double 后把原始精度丢了再声称无损迁移。

超过12位精度的旧 Skin 观测值不能为了迁移塞进 v2 Decimal：保留演示原始数据并 review；v1 旧源仍可由旧版本读取。迁移目标不是强迫所有旧展示数字变成可执行观测。

## 6. ReviewDecisions 的完成条件

预览需展示每行原列、候选字段、行/列诊断、严重度（error/review/info）。所有结构 error 必须先解决；review 需由用户逐类确认：目录关联、价格列语义、单位/quantum、成色/品阶/稀有度字典、未解释尾列仅保留、真实数量暂不绑定。

每个确认写 migration_decisions：decision_id/source_sha256/source_line/field/choice/actor/note。用户确认 actor=user；v1 缺省/浮点噪声规范化由 compatibility_adapter 记录，不能代替用户确认旧市场语义。

生成完整 ConfigV2 后再次校验全部 ID 引用、单位、规则状态和配置预算。选择跳过坏行必须是显式操作并写排除清单；默认不“好行先导入、坏行静默跳过”。同内容重复行保留两条、标 POSSIBLE_DUPLICATE_ROW；不擅自去重丢任务。

## 7. 提交事务与并发

```text
READ_ONLY_PREVIEW
→ REVIEW_DECISIONS
→ MATERIALIZE_CANDIDATE
→ VALIDATE_WHOLE_CONFIG
→ CHECK_SOURCE_HASH_AND_CONFIG_REVISION
→ WRITE_NEW_DESTINATION
→ COMMIT_FILE
→ PUBLISH_IN_MEMORY_SNAPSHOT
```

冻结规则：

1. 导入目标必须是新路径，既不等于源文件也不覆盖已有输出；输出已存在报 OUTPUT_EXISTS。用户另选新路径后重试；不悄悄替换唯一备份。
2. 提交前复核源哈希与预览一致；不一致报 SOURCE_CHANGED。内存基线 revision 必须仍等于预览时 revision，否则 CONFIG_CONFLICT，重新合并预览。
3. 全部操作由单一配置提交器串行化；同一目标文件需进程级锁，锁失败 CONFIG_BUSY；运行已持有规则快照时，仅发布下一轮配置，不热改当前 run。
4. 采用 QSaveFile，关闭 direct-write fallback；临时配置内容写完且 commit 成功后，才切换内存快照/当前路径并发出 changed。配置文件临时写入与“图像不落盘”不是一回事。[R12，设计]
5. 写失败、commit失败、磁盘满、校验失败不修改当前内存配置或源文件。若在文件提交成功而内存发布前崩溃，新目标是完整文件；重启展示待恢复导入结果，不能自动开始业务。
6. 事务回退只恢复配置引用；不删除账本，不把未决 attempt 当没有发生。v1 文件与旧程序仍保留原行为，v2另路径保存。

## 8. 稳定导入诊断码与定位

| code | severity / 定位 |
|---|---|
| FILE_TOO_LARGE / TOO_MANY_ROWS / LINE_TOO_LONG / BAD_ENCODING | error；能定位时提供物理行，否则 line=null |
| DUPLICATE_KEY / ORPHAN_CONTINUATION / COLUMN_COUNT | error；实际行；column 为字段列号或文本字符位置，message 明示单位 |
| DECIMAL_INVALID / DECIMAL_GROUPING_INVALID / DECIMAL_CAPACITY_EXCEEDED | error；任务列7/8/9；不得附带凭据值 |
| PRICE_RANGE_REVERSED | error；列7，并说明第8列作为对照 |
| PRICE_MAPPING_UNREVIEWED / UNIT_UNREVIEWED / PRODUCT_UNRESOLVED / TAXONOMY_UNREVIEWED | review；对应字段 |
| UNKNOWN_TRAILING_COLUMNS | review；列12/13；raw保留且数量为空 |
| POSSIBLE_DUPLICATE_ROW | review；当前行与首个相同行号 |
| V1_FLOAT_NOISE_NORMALIZED | info；JSON Pointer 与原token来源 |
| V1_PRECISION_INVALID / VERSION_UNSUPPORTED / V1_NOT_DEMO | error；旧字段指针 |
| OUTPUT_EXISTS / SOURCE_CHANGED / CONFIG_CONFLICT / CONFIG_BUSY / COMMIT_FAILED | error；事务阶段；当前内存及源保持不变 |

13列中列号是分隔字段序号；JSON 使用 field=JSON Pointer，column=null，避免把 JSON 字符列误当任务列。每行最多128诊断、整份最多1000顶层诊断；达到上限追加 DIAGNOSTICS_TRUNCATED 汇总并阻止提交，不能让被截掉的错误变成通过。

## 9. 验收如何落地

[import-golden.json](fixtures/import-golden.json) 的14场景覆盖13列不丢尾列、错误行、数字分组、重复行、v1缺字段和浮点噪声、目标已存在、写失败和源文件变化；目前只是规格。v1固定基线已提供为 [v1.synthetic.json](fixtures/v1.synthetic.json)，来自项目旧演示 `artifacts/BASELINE.json` 的逐字节副本，来源/哈希见 [v1.synthetic.origin.json](fixtures/v1.synthetic.origin.json)。它含12个合成商品、5个演示任务，缺省字段保持原样以测试兼容，不表示市场观察。

I07–I09 装配顺序固定：以 import-golden.json 所在目录解析 base_ref → 校验 base_sha256 → 深复制 JSON → 按 operations 数组从前到后应用 → UTF-8序列化副本 → 调用 previewV1；从不修改基线文件。操作 path 是本规格 JSON Pointer：`replace` 要求目标已存在并仅改该值；`remove_if_present` 对缺字段无操作、存在则删除；不支持任意脚本/其他操作。各用例从全新基线重新装配，不能复用上一条已修改副本。数值token通过序列化保留 I08 的表示噪声；测试适配器必须验证字节包含 `0.30000000000000004` 后才开始该例。

未来测试需注入文件写失败，并同时比较源哈希、当前 configPath、配置 revision、日志数和运行快照，验证失败确实没有半提交。

成功测试不允许用“JSON Schema通过”代替导入语义正确。M1验收至少包括：原15条行列完整预览、数量不猜填、所有未知项可解释、源文件未变、v1仍由原读取器接受、新v2只有新读取器接受、取消预览不写配置。后续视觉图片采集始终走内存；此导入器没有任何捕获/图像保存职责。
