# 03 · 数据结构与配置迁移

## 1. 六类业务对象与一个事件流

以下为拟定 v2 模型，**当前 0.6.0 的 schema v1 不接受这些新增字段**。金额和磨损的定点设计属于我们选择，不代表样本内原本这样存储。[E08；设计]

| 对象 | 核心字段 | 约束/用途 |
|---|---|---|
| `SkinCatalogEntry` | product_id、name、series、season、grade、rarity、aliases、source | 稳定商品资料；grade 与 rarity 分离；别名不能覆盖原文 |
| `ListingObservation` | observation_id、product_ref、market_listing_id可空、association_ref、price、wear、condition、publicity、frame_ref、timestamps、field_evidence、confidence | 一次观测不可变；同款可有多条卖单；无稳定ID时关联只是推断 |
| `TaskRule` | task_id、revision、filters、price_range、max_wear、quantity_target、enabled、review_required、legacy_raw | 业务匹配规则；未知字段和单位先保留、不激活 |
| `TaskRun` | run_id、rule_revision、mode、state、cursor、pending_ids、started_at、cancel_epoch | 运行使用快照；编辑不会悄改已启动规则 |
| `AttemptRecord` | attempt_id、intent_id、run_id、association_ref、intent_kind、prepared/sent/receipt times、status、reason、evidence_refs | 区分准备/发起/成功/失败/未知；支持崩溃核验 |
| `RuntimeProfile` | profile_id、revision、mode、schedule、delay_policy、limits、capture/vision settings、schema_version | 运行参数与任务分离，单位显式 |
| `BusinessEvent` | event_id、run_id、seq、monotonic_time、wall_time、type、payload、causation_id、evidence_refs | 可重放、可定位；seq决定顺序而非可回拨时间戳 |

### 数字、时间与身份

- `DecimalValue = {unscaled: 十进制整数字符串, scale: 非负整数}`。例 `1.187788 → {"unscaled":"1187788","scale":6}`；用十进制比较而非浮点近似。拟定 scale 上限12，超精度进入审查而不是截断。观察文本仍保留 raw。
- `Money = DecimalValue + unit`。当前样本不足以确定所有场景的实际币种和最小面额；配置中的 340 按“市场显示单位”记录，不能标成人民币。单位未配置时不激活新执行规则。
- `DeadlineEstimate = {earliest_ms, latest_ms, clock_domain, observed_frame}`。显示整数秒倒计时意味着区间不确定度；不能当成毫秒级截止真值。
- `association_ref` 不是平台订单ID。无稳定ID时按商品、磨损、价格、公示状态、空间连续性关联，记录歧义。相同特征的两条仍可能是不同卖单。
- 数量预留关联 `attempt_id`。回执丢失时保留未决预留，等待核验，不直接释放额度重复发起。

## 2. BBZPS 任务的 13 列

首行原样例来自 E03：

```text
AUG突击步枪-天命|全部赛季|未拥有|史诗品阶|成色S|不限公示期|230|600|0.399|所有稀有度|默认排序|不限|不限
```

| 位置（1起） | 可见语义 | v2候选字段 | 状态及处理 |
|---|---|---|---|
| 1 | 商品名 | product.name/raw_name | 确认；目录ID关联另做 |
| 2 | 赛季 | filters.season | 确认；不限独立枚举 |
| 3 | 拥有状态 | filters.ownership | 确认；未拥有不是数量0 |
| 4 | 品阶 | filters.grade | 确认；不与稀有度混用 |
| 5 | 成色 | filters.condition | 确认；S/A/B/C与不限 |
| 6 | 公示期 | filters.publicity | 确认；与倒计时估计分开 |
| 7 | 价格下限候选230 | candidates.price_min | 推断；需导入预览确认 |
| 8 | 价格上限候选600 | candidates.price_max | 推断；包含边界不是静态确证 |
| 9 | 最大磨损0.399 | limits.max_wear | 配置与日志支持；保留原文 |
| 10 | 稀有度 | filters.rarity | 确认；不可用第4列覆盖 |
| 11 | 排序 | filters.requested_sort | 确认；运行中另有observed_sort |
| 12 | 不限 | legacy_raw.columns[11] | 未知；不映射为quantity |
| 13 | 不限 | legacy_raw.columns[12] | 未知；不映射为quantity |

现有配置共15条，每条13列。S/A/B/C阈值出现 `0.399/1.249/2.499/5.0`，这是该份配置的用户规则，不是通用成色定义。当前 INI 与2026-09-22至10-01的历史日志不是同一时刻快照，不能用最终配置解释所有旧执行。[E03、E04]

## 3. 全部可见运行参数字典

“可见”限于上一轮已脱敏配置；被保护业务可能存在其他内部默认。时间参数原始值不自动换算。[E03]

| 段/键 | 原值 | 证据可支持的用途 | 迁移决定 |
|---|---|---|---|
| MainWindow.mode | BBZPS_134250 | UI/运行模式标识 | legacy metadata，不决定新模式 |
| GlobalSettings.addtime | 0.1 | 时间增量样式参数，公式未定 | raw + review |
| GlobalSettings.clicktime | 0.0005 | 与点击时间有关的配置线索 | raw；与SkinTasks同名键不合并 |
| GlobalSettings.spt | 已脱敏 | 含义和敏感性未定 | 不导入原值 |
| GlobalSettings.watermark | BBZ | 截图水印线索 | 新程序使用自身配置，不搬旧品牌默认 |
| SkinTasks.click_mode | 轨迹运行 | 操作模式 | 保留原文，不自动选择输入实现 |
| SkinTasks.rool_mode | 老测试模式 | 运行模式标识 | 保留拼写和原值，语义待定 |
| SkinTasks.srot | 按稀有度升序 | 排序配置 | 保留旧拼写，映射候选sort；不等于价格升序 |
| SkinTasks.autocountdown | 2 | 自动倒计时相关线索 | 单位/触发意义待定 |
| SkinTasks.clickinterval | 0.005 | 点击间隔候选 | raw，不从名称推断秒/毫秒 |
| SkinTasks.clickinterval2 | 0.05 | 第二间隔候选 | 独立raw，不能与前项合并 |
| SkinTasks.autotime | True | 时间自动处理开关，有日志支持 | 新方案只估计时钟偏移 |
| SkinTasks.buynum | 99999 | 购买数量样式字段 | 含义/周期待确认，不默认无限 |
| SkinTasks.timesync | 三个NTP主机 | 时间源配置 | 可读为历史配置；新程序使用独立可配置源 |
| SkinTasks.clicknum | 3 | 点击次数候选 | 与运行日志逐次确认相符，作用阶段仍需确认 |
| SkinTasks.counting | 炼金计数 | 计数模式标识 | 保留原文，不臆造炼金业务 |
| SkinTasks.autoparameter | True | 自动参数开关 | 动态策略线索，未恢复公式 |
| SkinTasks.parameter | 0.005 | 调参步长/量级候选 | raw + review，不直接映射queueFullStepMs |
| SkinTasks.statistics | True | 统计开关 | 新统计独立按回执账本实现 |
| SkinTasks.clicktime | 0.847 | 延迟区间参数候选 | 单位待定 |
| SkinTasks.clicktime2 | 0.857 | 第二延迟/区间端点候选 | 与clicktime关系待定，不宣称已还原上下界 |

两个 `*.exe.ini` 中的登录/凭据字段不进入业务配置迁移。原授权库的登录、心跳、到期等导出只说明旧发行包还有商业授权层，不是枪皮任务模型必要组成。[E02、E03]

## 4. 当前 RunSettings 的新旧关系

| 当前字段 | 当前意义 | 重构处理 |
|---|---|---|
| profile / hotkey | 默认方案名 / F2，配置层 | 新增方案ID、快捷键注册反馈，保留名称 |
| purchaseDelayMs | 默认830，范围由当前解析器验证 | 新规格单位毫秒；作用基准待D03决策 |
| dynamicDelay | 默认false | 未确认策略前维持关闭 |
| queueFullTrigger / queueFullStepMs | 次数/每次减量 | 独立事件计数、步长毫秒；触发语义待D04 |
| publicityTrigger / publicityStepMs | 次数/每次加量 | 不拿普通公示倒计时作为调参错误回执 |
| burstClick / clickIntervalMs | 默认true / 10ms，只保存 | 进入测试动作策略，不默认连接目标程序 |
| limitOrange / limitPurple / limitBlue | 各默认10 | 颜色↔品阶映射及计数周期待D05 |
| refreshPage / skipLotteryPage / skipSuccessPage | 挂机三项 | 分别绑定页面状态，不混成通用Esc |
| autoCollect / collectOsd | 自动收藏/OSD | 识别数据流中防止OSD反向污染 |
| scheduleEnabled / scheduleStart / scheduleStop | 默认关闭，09:00/23:00 | 明示时区、跨日区间和停止时未决动作处理 |

这组字段来自原 Relink 入口要求，不证明其与 BBZPS 同名/异名参数一一等价。[E08、E11]

## 5. 导入事务和兼容策略

1. 只读输入文件，记录哈希/编码/行号；不执行任何 INI 附带路径或命令。重复键、坏编码、超大文件、13列外的行给出逐行诊断。
2. 解析成 `LegacyImportPreview`，原值与候选字段并列；令 `enabled=false`、`review_required=true`。未知语义不自动填入数量/单位/延迟。
3. 用户确认候选价格上下限、币值单位、成色/品阶映射；每个决定写入 `migration_decisions`。
4. 校验整份候选配置、规则引用和ID唯一性；成功后以QSaveFile提交新路径。失败不改变当前内存状态/源文件。[R12]
5. v1的缺失condition按“不限”；保留 `demo=true` 的来源，迁为mode=demo而非observe。v2初期只写新文件，不覆盖旧v1。
6. 回退时使用原v1备份和原程序；v2账本独立保留。不能把未知结果通过回退当成没有发生。

示例：[schema-v2.sample.json](examples/schema-v2.sample.json) 是**待审导入结果草案**，不是当前前端可直接载入的配置。它保留13列，价格字段仍是候选，任务禁用；没有从末两列猜出数量。

## 6. 持久化与隐私边界

配置使用可读JSON；事件/attempt/receipt可采用SQLite，表主键和唯一约束围绕 `event_id/intent_id/receipt_id`。WAL可作为候选模式，但不是“永不丢数据”的保证；事务持久性选项、磁盘失败、检查点、备份时的WAL一致性均需验收。备份使用数据库提供的一致性机制或关闭连接后完整备份，不运行中只复制主文件。[R13；设计]

图像默认完全不落盘，包括成功、失败以及普通ROI截图；仅用户主动导出或明确启用有界调试时保存。实时图像不通过临时文件传给worker。账本保留字段值、观测时间、规则/模型版本等结构化证据，`image_ref`可空并标记“未保存图像”，不声称能重新打开已释放的内存帧。[E12；设计]

配置与必要业务账本按事务保存；日志不含凭据，不逐帧写OCR全文或像素数据，普通诊断采用限速汇总和容量轮转。账本一致性不因节省IO而省掉必要事务。旧程序近1.77GB日志不作为新默认产出规模。[E04、E12；设计]
