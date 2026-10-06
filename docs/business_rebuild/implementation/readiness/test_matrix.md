# 测试设计与88条规格的缺口补齐

> 本表是执行设计，不是执行结果。全部 X 用例仍为 planned_not_run；M0文档检查在根验证报告登记，不改业务用例状态。源数据：[test_matrix.json](test_matrix.json)。

## 分层与证据边界

| 测试层 | 何时 | 输入/输出 | 不能替代 |
|---|---|---|---|
| 文档schema/追踪 | 本轮M0 | schema接受/拒绝、互链、oracle结构 | 业务实现单测 |
| 纯值/规则/迁移 | M1 | 精确数字、Decision、原子迁移 | OCR和采集 |
| 回放 | M1/M3 | 状态/事件/请求计数逐项oracle | 像素识别 |
| 账本故障 | M1 | 幂等/事务/崩溃/未知预留 | 外部操作exactly-once |
| 图像质量 | M2 | 独立图像真值、留出集字段指标 | 真实捕获兼容/动作结果 |
| 捕获+IO | M2 | 自有窗口、可信worker进程树、租约和文件操作 | 全机无IO或真实市场成交 |
| UI离屏 | M1/M4 | 几何/入口/模式/来源/状态及测试snapshot | 后端判断正确性 |
| 包装/回退 | 发布前 | 脱SDK启动、manifest、独立副本恢复 | OCR性能 |

## 现有88条为何不能直接当回归测试

现有T01–T44的A/B描述保留为产品验收上层；新增X是更细的工程试验，不替换或虚增“已通过”数量。具体缺口：

| 上层T | v0.2不足 | 下钻用例 |
|---|---|---|
| T14/T15/T16 | 无混合缺失优先级、24位精度/scale/溢出oracle | X03–X05 |
| T20/T28 | Match与执行资格易混淆；未知限购是否默认无限未测 | X06/X14 |
| T05/T39/T01 | 文件字节/坏编码/事务失败/快照与路径不变未细化 | X07–X09 |
| T02/T22/T35/T43 | pause/stop竞态、旧epoch、长等待、模式隔离顺序未固定 | X10–X13 |
| T30/T31/T37 | 只提幂等/崩溃，缺精确注入点与冲突payload | X14–X18/X30 |
| T07/T08/T14/T15/T20 | 图像真值、跨集泄漏、行关联、覆盖和拒识口径不足 | X19/X20 |
| T06/T44 | 取消不等于释放、双槽内存复用竞态、OSD/DPI细节不足 | X21/X22 |
| T06/T07/T37/T44 | 成功/失败/崩溃、子进程IO、文件类型绕过、显式调试预算不足 | X23–X25 |
| T16/T20/T38/T43/T40 | 只有可达性，缺具体UI投影/多卖单/缩放 | X26/X27 |
| T39/T41/T42 | 构建缓存迁移、SDK依赖泄漏、Sql driver和副本回退未固定 | X28/X29 |

## Given / When / Then 规格

### X01 · document_schema / M0
关联：B39 T39-B；PR02 PR08。状态：planned_not_run。

- **Given：** 有效config、disabled preview和缺单位/坏类型反例均在schema manifest中
- **When：** 离线JSON Schema验证manifest并对照expected_valid
- **Then：** 正例接受、反例在预期路径拒绝；preview不是可执行profile
- **Fixture：** `domain/validation_manifest.json及schema fixture`
- **执行计划：** `由根implementation验证入口运行JSON Schema批次`
- **不证明：** 只证明schema形状/明示约束，不证明C++解析或业务函数

### X02 · document_traceability / M0
关联：B01 B44 T01-A T44-B；PR01。状态：planned_not_run。

- **Given：** 12张票、44业务、88规格和D01-D13存在
- **When：** 运行readiness文档自检
- **Then：** 票依赖无环、WI01-WI05全覆盖、B/T/E/R合法、测试状态非已执行
- **Fixture：** `backlog.json + test_matrix.json`
- **执行计划：** `python -I -X utf8 docs\business_rebuild\implementation\readiness\validate_readiness.py`
- **不证明：** 不证明业务实现

### X03 · pure_value / M1
关联：B14 B15 T14-A T15-B；PR02。状态：planned_not_run。

- **Given：** 23.0/23.00、24位上界、scale12及非法指数/负数等golden
- **When：** 执行值对象解析/比较
- **Then：** 每个结果与expected一致；不经double、不溢出、不截断
- **Fixture：** `domain/fixtures/rule-golden.json + value边界fixture`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；注册business_value_tests`
- **不证明：** 不证明OCR读数准确

### X04 · pure_rule / M1
关联：B14 B15 B16 T14-A T15-A T16-A；PR03。状态：planned_not_run。

- **Given：** 明确单位、有效观察，价格恰好上下界且wear等于阈值
- **When：** 调用match
- **Then：** Match和逐字段满足原因；eligible_for_action=false
- **Fixture：** `domain/fixtures/rule-golden.json`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；business_rule_tests`
- **不证明：** 匹配不是动作许可

### X05 · pure_rule / M1
关联：B16 T16-B；PR03。状态：planned_not_run。

- **Given：** 价格可靠超界，同时必要磨损缺失
- **When：** 调用match
- **Then：** 按canonical优先级NeedsReview，缺失原因可见；不让超价掩盖不可靠输入
- **Fixture：** `domain/fixtures/rule-golden.json混合反例`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；business_rule_tests`
- **不证明：** 不是用OCR分数代表可靠性

### X06 · pure_rule / M1
关联：B20 B28 T20-B T28-B；PR03 PR09。状态：planned_not_run。

- **Given：** 相同特征的两个卖单、身份歧义；或真实quota scope未明确
- **When：** 匹配并申请执行资格
- **Then：** 关联歧义Review；缺quota许可不发新意图，不静默当无限
- **Fixture：** `domain关联fixture + runtime额度fixture`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；business_rule_tests/ledger_tests`
- **不证明：** 合成ID不证明真实市场有稳定ID

### X07 · migration / M1
关联：B39 T39-A T39-B；PR06。状态：planned_not_run。

- **Given：** 旧v1缺condition/run_settings，旧demo=true；另有坏引用输入
- **When：** 迁移预览再提交新路径
- **Then：** 合法默认与旧解析一致且仍demo；非法不改当前状态、源hash、活动路径
- **Fixture：** `版本化v1 golden与domain/import-golden.json`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；migration_tests`
- **不证明：** 不把旧double演示价当真实精确行情

### X08 · migration / M1
关联：B05 T05-A T05-B；PR07。状态：planned_not_run。

- **Given：** E03 15条13列与12/14列、乱码、超长反例
- **When：** 导入preview
- **Then：** 原值、行号、13列保真；候选禁用；错误逐行，凭据不输出
- **Fixture：** `evidence/task_schema.json + domain/fixtures/import-golden.json`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；migration_tests`
- **不证明：** 不证明第12/13列业务含义

### X09 · migration / M1
关联：B01 B39 T01-B T39-B；PR08。状态：planned_not_run。

- **Given：** 可用旧配置、待写新配置、注入写入/提交失败
- **When：** QSaveFile提交或取消切方案
- **Then：** 旧文件字节与内存不变、configPath不变、原run快照不变
- **Fixture：** `profile_store临时目录和可注入故障writer`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；profile_store_tests`
- **不证明：** 合法配置持久化不证明策略已被批准

### X10 · replay / M1
关联：B43 T43-A；PR04。状态：planned_not_run。

- **Given：** 同一事件fixture/相同初始状态和FakeClock
- **When：** 回放两次
- **Then：** state/event/decision序列一致；无真实capture、OCR或input启动
- **Fixture：** `runtime/fixtures/replay_cases.json`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；replay_tests`
- **不证明：** 不证明从像素可提取这些事件

### X11 · replay / M1
关联：B02 B35 T02-A T35-B；PR04。状态：planned_not_run。

- **Given：** 观察请求在途、cancel_epoch=1，随后暂停到epoch2
- **When：** 送回epoch1的有效旧结果并重复pause
- **Then：** 旧结果不推进业务；无新需求；重复pause幂等；资源可draining而run=Paused
- **Fixture：** `runtime/fixtures/replay_cases.json取消用例`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；replay_tests`
- **不证明：** 资源draining不能冒充已经回收

### X12 · replay / M1
关联：B22 T22-B；PR04。状态：planned_not_run。

- **Given：** 长等待10000ms、无近期观察需求；FakeClock墙钟回拨
- **When：** 推进单调时钟并送旧timer唤醒
- **Then：** 长等待中需求数0；过期代次timer无效；唤醒只在新step申请
- **Fixture：** `runtime时钟/长等待fixture`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；replay_tests`
- **不证明：** 不证明真实定时器亚毫秒精度

### X13 · replay / M1
关联：B02 B43 T02-A T43-B；PR04 PR05。状态：planned_not_run。

- **Given：** 运行中编辑规则或切换mode
- **When：** 继续送旧run事件
- **Then：** 旧run用旧revision；新mode不消费旧事件；日志/统计不串
- **Fixture：** `runtime规则快照/mode fixture`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；replay_tests/ui_offscreen`
- **不证明：** 不证明后台全局快捷键已实现

### X14 · ledger / M1
关联：B28 B30 B31 T28-B T30-B T31-A；PR09。状态：planned_not_run。

- **Given：** target=1，一条sent后无回执的attempt
- **When：** 超时、stop、重放Start
- **Then：** 保持Unknown预留；新尝试不获额度；停止不删除未决
- **Fixture：** `integration ledger oracle + runtime Attempt fixture`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；ledger_tests`
- **不证明：** 不宣称外部exactly-once

### X15 · ledger / M1
关联：B30 B31 T30-A T31-B；PR09 PR10。状态：planned_not_run。

- **Given：** 同receipt_id两次相同payload，再一次不同payload
- **When：** 提交事务并读取投影
- **Then：** 前两次成功只计1；不同payload冲突明确，历史不被覆写
- **Fixture：** `integration receipt/idempotency fixture`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；ledger_tests`
- **不证明：** 收到成功文字但归属不明仍不能套入此正例

### X16 · ledger_storage / M1
关联：B31 T31-A T31-B；PR10。状态：planned_not_run。

- **Given：** DB事务在提交前/后、intent已durable而回执未到的状态
- **When：** 在精确注入点结束自家测试子进程并重新打开DB
- **Then：** 提交前无半条记录；提交后可重读；未决保持待核验，不重发
- **Fixture：** `integration/storage_contract.md故障注入点 +临时DB`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；sqlite_ledger_tests（自家子进程hidden）`
- **不证明：** 不运行BBZPS或外部动作；不以正常关闭替代崩溃测试

### X17 · ledger_storage / M1
关联：B37 T37-A T37-B；PR10。状态：planned_not_run。

- **Given：** 数据库锁忙/满/磁盘写失败；UI订阅成功计数
- **When：** 尝试提交结果
- **Then：** 提交失败则UI不加成功数，暂停并保存可读诊断；恢复后不重计
- **Fixture：** `IEventStore故障注入实现`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；sqlite_ledger_tests`
- **不证明：** 日志降采样不允许省略必要交易事务

### X18 · ledger_storage / M1
关联：B31 B39 T31-A T39-B；PR10 PR12。状态：planned_not_run。

- **Given：** 含WAL数据/未决attempt的账本与独立v1备份
- **When：** 按契约一致性备份并在副本恢复旧程序
- **Then：** 新账本与未决数据保留；旧程序只读v1；不只复制运行中主DB
- **Fixture：** `integration备份fixture +发布副本`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；sqlite_ledger_tests/发布回退计划`
- **不证明：** 不保证任意故障硬件上的绝对不丢数据

### X19 · image_quality / M2
关联：B07 B08 T07-A T08-B；WI06–WI08后续视觉工作项。状态：planned_not_run。

- **Given：** 独立采集会话划分的标注集，含页面/框/文本/卖单关联
- **When：** 离线评测候选OCR与预处理
- **Then：** 每字段精确率/拒识/覆盖/行关联/误匹配单独报告；同帧变体不跨集
- **Fixture：** `待M2建立的有来源图像真值清单`
- **执行计划：** `拟新增vision_benchmark CLI；在WI07注册后固定命令与模型hash`
- **不证明：** 结构化历史OCR输出不能证明OCR精度

### X20 · image_quality / M2
关联：B14 B15 B20 T14-B T15-B T20-B；WI06–WI08后续视觉工作项。状态：planned_not_run。

- **Given：** 价格千分位断裂、O/0、邻行磨损、重复商品、高DPI负例
- **When：** 从原始像素到ListingObservation
- **Then：** 错误/歧义保留Unknown/Review；不把相邻行字段拼成合格卖单
- **Fixture：** `M2逐字段人工真值及负例集`
- **执行计划：** `拟新增vision_benchmark --suite fields-negative`
- **不证明：** 零观测误报只能描述该测试集，不能外推真实零风险

### X21 · capture_contract / M2
关联：B06 B44 T06-A T44-B；WI06–WI08后续视觉工作项。状态：planned_not_run。

- **Given：** step_id/epoch/viewport明确的新帧需求与两个占用槽
- **When：** resize/DPI变化并送旧frame
- **Then：** 旧代次失效；ROI映射往返在定义容差内；目标关闭为错误非空页
- **Fixture：** `runtime帧/ROI schema +合成DPI场景`
- **执行计划：** `拟新增capture_contract_tests`
- **不证明：** 不证明具体目标的全屏/遮挡捕获兼容

### X22 · capture_lifecycle / M2
关联：B06 B44 T06-B T44-B；WI06–WI08后续视觉工作项。状态：planned_not_run。

- **Given：** worker正读取租约，coordinator已cancel且超时
- **When：** 观察释放/退出前后buffer复用
- **Then：** 取消立即停新需求；release或确认退出前不复用；回收后计数归零
- **Fixture：** `runtime租约状态fixture+可信mock worker`
- **执行计划：** `拟新增worker_contract_tests --suite lease-lifetime`
- **不证明：** 逻辑取消不等于线程/进程已停止读内存

### X23 · capture_io / M2
关联：B06 B07 B37 B44 T06-B T07-B T37-B T44-B；WI06–WI08后续视觉工作项。状态：planned_not_run。

- **Given：** 默认持久化false；覆盖正常/成功/失败/worker崩溃/共享内存失败
- **When：** 执行自有只读捕获测试并观测应用进程树文件操作
- **Then：** 图像/临时帧文件创建次数0、图像写入bytes0；配置和必要账本另计
- **Fixture：** `runtime默认policy；自有测试窗口；进程树/路径/写入观测报告`
- **执行计划：** `拟新增capture_io_tests +受控文件IO观察，WI06前冻结操作命令`
- **不证明：** 不承诺全机零IO/禁用分页；不可只搜索.png扩展名

### X24 · capture_io / M2
关联：B06 B22 B35 T06-A T22-B T35-B；WI06–WI08后续视觉工作项。状态：planned_not_run。

- **Given：** Ready/Paused/Stopped/长等待/已超时step，worker模型可驻留
- **When：** 等待观测窗口超过两个正常取帧周期
- **Then：** 新增capture请求=0、OCR请求=0；bounded_watch帧/时间预算不越界
- **Fixture：** `runtime观察计数fixture+instrumented真实provider`
- **执行计划：** `拟新增capture_io_tests --suite demand-boundaries`
- **不证明：** M1假源计数不替代本层真实采集计数

### X25 · capture_io / M2
关联：B37 T37-A T37-B；WI06–WI08后续视觉工作项。状态：planned_not_run。

- **Given：** 手动导出或显式有到期/容量/张数限制调试会话
- **When：** 达到预算/关闭/异常
- **Then：** 仅显式会话允许保存；到期后零自动图像写；异常不偷偷续期开启
- **Fixture：** `有界调试policy及文件IO报告`
- **执行计划：** `拟新增capture_io_tests --suite explicit-export`
- **不证明：** 正常业务默认仍不落图，不以测试导出更改默认

### X26 · ui_offscreen / M1
关联：B16 B20 B38 B43 T16-A T20-A T38-B T43-B；PR05 PR11。状态：planned_not_run。

- **Given：** replay包含Match/NoMatch/Review及同品两卖单
- **When：** 离屏加载、单步、暂停、切回demo
- **Then：** 各行来源/观测时间/原因明确，曲线不混数据；暂停后不变
- **Fixture：** `domain+runtime黄金fixture与UI视图断言`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；ui_offscreen扩展`
- **不证明：** UI截图是测试产物，不是实时目标采集落盘

### X27 · ui_offscreen / M1
关联：B01 B04 B40 T01-B T04-A T40-A T40-B；PR05 PR11。状态：planned_not_run。

- **Given：** 原界面全部入口、100%/125%/150%缩放、长中文/错误信息
- **When：** 运行自家Qt离屏布局检查
- **Then：** Win11白灰/无蓝色；导航/关注/右编辑/底图保留；控件不遮挡；优化不臆造
- **Fixture：** `E11入口清单、离屏snapshot与geometry assertions`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；另以QT_SCALE_FACTOR矩阵跑ui_offscreen`
- **不证明：** 截图肉眼好看不替代字段/可达性断言

### X28 · release_package / M1
关联：B41 B42 T41-A T42-A T42-B；PR12。状态：planned_not_run。

- **Given：** 当前revision新构建、QtSql新增依赖、旧目录备份
- **When：** build -Test -Package，清空SDK路径后离屏启动发布EXE
- **Then：** 二进制hash一致；Qt DLL/platforms/qsqlite齐；无样本组件；命令退出0
- **Fixture：** `发布manifest与SDK隔离环境`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test -Package；参数化verify_delivery`
- **不证明：** SDK环境下通过不证明发布依赖完整

### X29 · release_package / M1
关联：B39 B43 T39-A T43-B；PR12。状态：planned_not_run。

- **Given：** 备份旧解压目录/旧v1、新v2账本含未知attempt
- **When：** 在另一副本执行回退并打开旧配置（离屏）
- **Then：** 旧行为恢复；新账本未删除；固定交付目录可切换；无可见窗口
- **Fixture：** `独立rollback-test目录与hash清单`
- **执行计划：** `PR12回退脚本（仅验证副本）；按包manifest复核`
- **不证明：** 单纯复制旧EXE不是完整回退

### X30 · logging_budget / M1
关联：B37 T37-A T37-B；PR09 PR10 PR11。状态：planned_not_run。

- **Given：** 大量重复诊断、少量关键状态/intent/receipt；默认无逐帧OCR全文
- **When：** 压测必要事件和日志汇总
- **Then：** 关键账本不丢，重复诊断限速并有被抑制计数；容量有限、记录不含像素/凭据
- **Fixture：** `integration日志预算与必要事件fixture`
- **执行计划：** `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test；ledger_tests与UI响应观察`
- **不证明：** 降低日志量不能等同数据持久性已充分验证

## 命令与门槛的执行纪律

带“拟新增”“注册后”的命令是下阶段目标，不应在本轮伪执行或写成已通过。既有build.ps1负责PATH和实际构建目录；CTest筛选命令需同一环境。每个golden的expected由规格独立给出，不能调用被测实现生成expected再与自己比较。

失败记录至少包含case_id、fixture hash、expected、actual、build hash、完整command/stdout/stderr/exit code、注入点和时间基准。业务数、回放事件数、实际截图数、OCR数、图像文件bytes和必要账本bytes分列；不把观察请求数等同已成功捕获帧数。
