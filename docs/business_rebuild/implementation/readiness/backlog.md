# WI01–WI05 的 12 张可合并任务票

> 所有票均为 planned_not_implemented。本文件由 build_readiness.py 生成；本轮只检查文档，不执行下面的业务验收。

**先交 PR01–PR05 最小纵切片，再接迁移和持久化。** 每票以一次可审查的变更为单位；验收失败不拖入下一票用新功能掩盖。票据的验收细节见 [测试矩阵](test_matrix.md)。

| 票 | 工作包 | 依赖 | 标题 |
|---|---|---|---|
| PR01 | WI01 WI05 | 无 | 建立领域库和可追溯测试入口 |
| PR02 | WI01 | PR01 | 实现精确数字、不可变观测和规则对象 |
| PR03 | WI01 | PR02 | 实现带原因码的纯规则匹配器 |
| PR04 | WI03 | PR03 | 实现可控时钟与确定性离线回放 |
| PR05 | WI05 | PR04 | 交付最小规则回放到原UI纵切片 |
| PR06 | WI02 | PR02 | 桥接schema v1而不改变旧演示含义 |
| PR07 | WI02 | PR02 | 实现13列文本只读导入预览 |
| PR08 | WI02 WI05 | PR06, PR07 | 实现方案仓库与v2审定提交 |
| PR09 | WI04 | PR04 | 实现内存事件仓库、数量预留与回执幂等 |
| PR10 | WI04 | PR09 | 加入QtSql/SQLite持久化和崩溃核验 |
| PR11 | WI05 | PR05, PR08, PR10 | 完善回放、账本、方案和记录的UI投影 |
| PR12 | WI05 | PR11 | 全量回归、离屏打包和解压目录回退验证 |

## PR01 · 建立领域库和可追溯测试入口

**关联：** WI01 WI05；B39 B42 B43；T39-A T42-A T43-A。
**证据/资料：** E08 E11 E12；R08 R12。依赖：无。未决影响：无产品语义阻塞。

**输入**
- E08现有CMake/build.ps1/src/domain.*与domain_tests；engineering_baseline.md；domain/runtime公开契约

**输出**
- relink_business静态目标只链接QtCore；旧demo目标行为不变
- 构建记录输出实际buildDir与toolchain；测试路径绑定同一产物
- 把批准的契约fixture注册为测试输入，不生成伪业务实现

**建议修改文件（下一轮）**
- CMakeLists.txt
- build.ps1
- src/business/CMakeLists.txt（或根目标段）
- tests/business/test_support.h

**合并验收**
- 现有domain_tests与ui_offscreen在本次构建目录通过
- 新的空壳本身不算功能完成；链接依赖审查无QtGui/捕获/输入组件
- 验证当前build_relocated而非旧build运行路径

**失败与回退**
- 恢复CMake/build脚本变更即可；不改用户配置/发布目录
- 若旧基线失败，先单独修复或记录已有失败，不吞掉失败继续开发

**本票不做**
- 不迁移AppState
- 不引入OCR/SQLite
- 不覆盖发布EXE

## PR02 · 实现精确数字、不可变观测和规则对象

**关联：** WI01；B04 B14 B15 B20 B39；T04-B T14-A T14-B T15-A T15-B T20-B T39-B。
**证据/资料：** E08 E11 E12 E03 E04；R08 R12。依赖：PR01。未决影响：D13。

**输入**
- domain/core.schema.json与字段字典
- domain/fixtures/rule-golden.json中的合法/非法十进制和对象样例

**输出**
- 从字符串解析Decimal与Money，24位unscaled、scale0..12按域契约处理
- 值对象保留raw/source/revision；商品与卖单分离
- 结构/语义错误返回字段路径和稳定诊断；不先经过double

**建议修改文件（下一轮）**
- src/business/decimal.h
- src/business/decimal.cpp
- src/business/models.h
- src/business/model_codec.cpp
- tests/business/value_tests.cpp

**合并验收**
- 同值异scale比较相等；最大值/超长/指数/负数/非法scale按golden结果拒绝或接受
- 缺失与0区分；未知单位不是默认货币
- 旧v1 double保留在旧demo适配层，没有改义；schema与C++接受集合一致

**失败与回退**
- 新目标保持可移除；旧AppState继续工作
- 数字契约不匹配时停止合并并修改同版本golden评审，不临时加epsilon

**本票不做**
- 不定义市场实际币种
- 不决定真实数量周期
- 不实现视觉

## PR03 · 实现带原因码的纯规则匹配器

**关联：** WI01；B11 B14 B15 B16 B20 B28；T11-A T11-B T14-A T14-B T15-A T15-B T16-A T16-B T20-A T20-B T28-B。
**证据/资料：** E08 E11 E12 E03 E04；R08 R12。依赖：PR02。未决影响：D02 D05 D07 D13。

**输入**
- 域契约Decision优先级、字段可靠性和新鲜度规则
- rule-golden.json完整输入+expected；运行上下文显式注入

**输出**
- match(rule, observation, context)返回Match/NoMatch/NeedsReview及逐字段原因
- 结构/语义非法输入与业务不匹配分开
- Match仅说明规则符合，eligible_for_action保持false；执行额度由runtime显式快照判定

**建议修改文件（下一轮）**
- src/business/rule_matcher.h
- src/business/rule_matcher.cpp
- tests/business/rule_tests.cpp

**合并验收**
- 全部golden逐字段相等，不只比较enum
- 未审配置、陈旧/歧义/缺字段/单位问题按优先级给Review；闭区间边界包含相等
- 未知限购不被转换成无限；测试反例证明Match不发意图
- 相同输入重复执行没有IO/系统时间/随机数依赖

**失败与回退**
- 恢复纯函数实现不影响配置；继续显示旧demo
- 任何优先级变更先修改契约/反例再实现，禁止只修单例绕过

**本票不做**
- 不调用截图或OCR
- 不操作外部程序
- 不把Match当发起许可

## PR04 · 实现可控时钟与确定性离线回放

**关联：** WI03；B02 B22 B35 B43；T02-A T22-B T35-A T35-B T43-A T43-B。
**证据/资料：** E08 E11 E12 E03 E04；R08 R12。依赖：PR03。未决影响：D03 D06 D10 D11。

**输入**
- runtime/state_machine.md、state.schema.json和fixtures/replay_cases.json
- domain规则fixture和可控单调时钟输入
- 模式/epoch/step/事件顺序约束

**输出**
- ReplayEngine只读结构化fixture，支持start/pause/step/stop
- 按输入seq/假时钟推进；取消后迟到结果不改变状态
- 观察需求与实际采集计数分离：M1假源，真实capture=0
- 复现确定的状态/事件/意图数量；输出来源replay

**建议修改文件（下一轮）**
- src/replay/fake_clock.h
- src/replay/replay_reader.cpp
- src/replay/replay_engine.cpp
- tests/replay/replay_tests.cpp

**合并验收**
- 相同输入两轮输出去除明确声明的运行标签后完全一致
- pause/stop重复调用不多建run；旧epoch/重复/乱序事件按契约处理
- Ready/Paused/Stopped/长等待无新增观察需求；不创建截图文件
- IO失败/错误fixture不会推进半个状态；事件序列有逐项oracle

**失败与回退**
- 停止新ReplayController并恢复旧demo入口；保留只读fixture
- 回放失败可重启新run，不能悄悄续用半污染状态

**本票不做**
- 不实现实时捕获
- 不模拟未知动态公式
- 不更改系统时间

## PR05 · 交付最小规则回放到原UI纵切片

**关联：** WI05；B02 B04 B16 B20 B38 B40 B43；T02-A T04-A T16-A T20-A T38-A T40-A T40-B T43-A T43-B。
**证据/资料：** E08 E11 E12 E03 E04；R08 R12。依赖：PR04。未决影响：D08。

**输入**
- 集成ui_integration.md；PR03/04的只读状态投影
- 现有mainwindow/widgets和E11布局约定

**输出**
- 在原关注列表/条件面板/记录区显示回放模式、来源、观测时刻、决策原因
- 新增控制仅发run命令；旧demo继续可用且统计隔离
- 优化入口原位保留未定义说明；按需观察策略在状态栏可解释

**建议修改文件（下一轮）**
- src/application/run_controller.h
- src/application/ui_projection.cpp
- src/mainwindow.cpp
- src/mainwindow.h
- src/main.cpp离屏自检扩展

**合并验收**
- 离屏展示一条Match、一条NoMatch、一条NeedsReview及相应原因
- 暂停后UI不继续变；切换demo/replay不会串日志/曲线/计数
- E11颜色/顶部搜索/左导航/关注列表/右编辑/底部图和原入口全部保留
- 纯回放过程中真实capture/worker/input启动数均0

**失败与回退**
- 功能开关撤回新回放入口，旧演示不受影响
- 保存的新数据独立路径；撤回UI不删除回放记录

**本票不做**
- 不重做主题/布局
- 不删除原入口
- 不因UI连通宣称M1账本已完成

## PR06 · 桥接schema v1而不改变旧演示含义

**关联：** WI02；B04 B39 B43；T04-B T39-A T39-B T43-B。
**证据/资料：** E08 E11 E12 E03 E04；R08 R12。依赖：PR02。未决影响：D03 D05 D13。

**输入**
- 当前AppState v1读写和tests/domain_tests.cpp
- 版本化v1黄金fixture；domain import-golden与迁移规则

**输出**
- 独立V1Adapter将旧值转为demo来源的新配置候选
- 缺condition仍不限；旧RunSettings默认与原解析器一致
- 迁移诊断与原始值保留；v1原路径只读、v2新路径

**建议修改文件（下一轮）**
- src/config/v1_adapter.h
- src/config/v1_adapter.cpp
- tests/business/migration_tests.cpp
- tests/fixtures/v1/

**合并验收**
- 旧合法文件加载后原有显示与演示行为不变；不自动变Observe
- 原单位未明确则新执行配置保持未审；double来源不伪装精确真实观测
- 坏类型/重复ID/失效引用拒绝并保持原内存、源文件hash和活动路径

**失败与回退**
- 继续使用原v1与旧解析器；丢弃未提交候选
- 源文件未覆盖，因此无需逆向恢复猜测字段

**本票不做**
- 不批量自动激活
- 不改旧schema1格式
- 不接受样本代码或凭据

## PR07 · 实现13列文本只读导入预览

**关联：** WI02；B05 B14 B15 B39；T05-A T05-B T14-B T15-B T39-B。
**证据/资料：** E08 E11 E12 E03 E04；R08 R12。依赖：PR02。未决影响：D01 D02 D03 D12 D13。

**输入**
- E03 15条13列文本与脱敏schema快照
- domain/import-preview.schema.json、import-golden.json

**输出**
- 行号/编码/源hash/raw13列/候选映射/逐行诊断
- 第7/8列候选、12/13列原位保留；enabled=false/review_required=true
- 明确文件/行/字段长度上限、错误编码和重复键策略，限制按域契约执行

**建议修改文件（下一轮）**
- src/config/legacy_importer.h
- src/config/legacy_importer.cpp
- tests/business/legacy_import_tests.cpp

**合并验收**
- 15条列数/原文往返；源hash不变，输出无spt或登录字段
- 12/14列、坏编码、超限、空字段、额外管道符均给确定诊断且不半提交
- 预览被传入runnable入口时明确拒绝；未知列从不变quantity

**失败与回退**
- 丢弃preview即可；旧配置和当前run保持不变
- 编码不确定时展示诊断，不用有损解码掩盖

**本票不做**
- 不执行INI路径或命令
- 不恢复原授权
- 不猜字段语义

## PR08 · 实现方案仓库与v2审定提交

**关联：** WI02 WI05；B01 B04 B24 B39；T01-A T01-B T04-A T04-B T24-B T39-A T39-B。
**证据/资料：** E08 E11 E12 E03 E04；R08 R12。依赖：PR06, PR07。未决影响：D02 D03 D04 D05 D11 D13。

**输入**
- domain/config-v2.schema.json、import preview与migration decisions
- 集成storage_contract.md的配置提交约定
- 当前界面参数与profile名

**输出**
- profile稳定ID/revision；独立新路径v2；QSaveFile全量校验后提交
- 候选确认记录带字段、决定、来源；有效规则快照不被编辑改变
- 动态默认关闭，未定限购语义保存但不进入执行许可

**建议修改文件（下一轮）**
- src/config/profile_store.h
- src/config/profile_store.cpp
- src/application/profile_controller.cpp
- tests/business/profile_store_tests.cpp

**合并验收**
- 两方案往返和脏编辑取消；活动run继续用旧revision
- 写失败/目录不可写/无效引用不覆盖旧文件或当前状态
- 取消审定、缺单位、未知qty仍不能激活；新原生规则闭区间明确
- 原RunSettings所有入口字段保留并有映射/原始容器

**失败与回退**
- 恢复v1程序/配置独立副本；v2与迁移报告保留
- 事务失败不更改活动configPath

**本票不做**
- 不启用真实动态策略
- 不写死市场时段
- 不将schema导出成功等同业务已启用

## PR09 · 实现内存事件仓库、数量预留与回执幂等

**关联：** WI04；B28 B30 B31 B37 B43；T28-A T28-B T30-A T30-B T31-A T31-B T37-A T43-B。
**证据/资料：** E08 E11 E12 E03 E04；R08 R12。依赖：PR04。未决影响：D05 D07。

**输入**
- integration/storage_contract.md的IEventStore/attempt事务与quota规则
- runtime状态/Attempt契约与ledger fixture
- 显式synthetic quota snapshot

**输出**
- IEventStore和InMemory实现；事件/attempt/receipt/投影职责分离
- 按intent/event/receipt去重；相同ID不同payload作为冲突
- 预留→发起→成功/失败/未知；Unknown继续占位
- 必要事件可追溯，逐帧OCR日志不开启

**建议修改文件（下一轮）**
- src/ledger/event_store.h
- src/ledger/in_memory_ledger.cpp
- src/business/quota_policy.cpp
- tests/business/ledger_tests.cpp

**合并验收**
- 重复成功回执只计一次；重复ID不同payload拒绝
- 明确失败释放、未知不释放；缺限购scope/单位时拒绝新的许可
- 停止prepared可取消；可能已发出保持Unknown待核验
- demo/replay投影与其它模式分库或按契约键空间隔离

**失败与回退**
- 内存实例可丢弃重放，但不能宣称保存真实外部历史
- 语义失败时不继续给UI成功计数，回到未决

**本票不做**
- 不提供外部exactly-once保证
- 不执行外部输入
- 不声称内存仓库具有崩溃持久性

## PR10 · 加入QtSql/SQLite持久化和崩溃核验

**关联：** WI04；B28 B30 B31 B37；T28-B T30-B T31-A T31-B T37-A T37-B。
**证据/资料：** E08 E11 E12 E03 E04；R12 R13 R15。依赖：PR09。未决影响：D05 D07。

**输入**
- integration/storage_contract.md与可执行DDL
- 已通过的内存仓库语义测试复用
- Qt6Sql/QSQLITE来源与当前SDK/发布差距

**输出**
- QtSql适配器与显式schema迁移；关键状态事务提交
- 恢复扫描未决attempt，prepared/sent处理遵循契约
- DB忙/满/写失败/提交失败明确回报；必要账本与降采样诊断分开
- 一致性备份、schema版本拒降级；部署Sql DLL/driver清单

**建议修改文件（下一轮）**
- CMakeLists.txt
- src/ledger/sqlite_ledger.h
- src/ledger/sqlite_ledger.cpp
- tests/business/sqlite_ledger_tests.cpp
- build.ps1依赖部署检查

**合并验收**
- 进程在事务前/提交前/提交后/回执前受控退出，重开结果等于oracle
- 数据库唯一约束与逻辑幂等一致；成功投影只在commit成功后更新
- 未知attempt不自动重发；一致性备份恢复含未决预留
- 实际发布目录可加载QSQLITE；不只证明SDK可用

**失败与回退**
- 退回旧程序+独立v1配置但保留v2 DB；新版DB不交旧程序解释
- 损坏/新schema进入只读诊断，不删除重建伪装成功

**本票不做**
- 不把SQLite WAL当永不丢失保证
- 不为减少IO跳过关键事务
- 不优化到逐帧存库

## PR11 · 完善回放、账本、方案和记录的UI投影

**关联：** WI05；B01 B02 B04 B20 B37 B38 B39 B40 B43；T01-A T02-A T04-A T20-A T37-A T38-A T38-B T39-A T40-A T43-A T43-B。
**证据/资料：** E08 E11 E12 E03 E04；R08 R12。依赖：PR05, PR08, PR10。未决影响：D08。

**输入**
- integration/ui_integration.md
- 已提交event/attempt/profiles和回放状态；原UI入口清单

**输出**
- 商品与卖单列表可辨；字段缺失/时间/来源/陈旧可见
- 记录分匹配/收藏/发起/确认成功/失败/未知；图表区分观测价与成交价
- CSV防公式转义；日志限速汇总且不嵌像素/OCR全文
- 停止/模式切换/脏配置交互和错误提示完整

**建议修改文件（下一轮）**
- src/application/ui_projection.cpp
- src/application/run_controller.cpp
- src/mainwindow.cpp
- src/main.cpp离屏验收扩展

**合并验收**
- 相同商品两卖单不覆盖彼此；成功/Unknown/NoMatch数字与账本相等
- 模式切换清理旧投影但不删历史；回放快速推进UI合并刷新
- 配置错误/磁盘失败显示原因，主线程不中断响应
- 默认图片写入策略UI不被成功或异常自动开启

**失败与回退**
- 停用新增投影但保留数据库/配置；旧UI继续只演示
- 失败不降级成由按钮槽函数直接执行业务

**本票不做**
- 不重新设计配色布局
- 不添加未定义优化功能
- 不把观测价标实际成交价

## PR12 · 全量回归、离屏打包和解压目录回退验证

**关联：** WI05；B39 B41 B42 B43；T39-A T41-A T42-A T42-B T43-A T43-B。
**证据/资料：** E08 E11 E12；R12 R13 R15。依赖：PR11。未决影响：无产品语义阻塞。

**输入**
- 全部前票测试证据与源码revision
- 实际buildDir、旧发布目录及独立配置/账本备份
- E11固定交付路径和依赖manifest

**输出**
- 参数化verify_delivery的构建目录/版本/断言数量
- 构建+全CTest+package-only PATH离屏+依赖manifest核对
- 发布前复制旧目录并在另一目录验证恢复；固定EXE路径更新
- 交付证据逐命令输出和未执行层说明

**建议修改文件（下一轮）**
- tests/verify_delivery.py
- build.ps1
- tests/release/verify_package.py
- docs/发布记录

**合并验收**
- 刚构建的EXE/hash与被测试和发布者相同
- Qt DLL/platforms/Sql driver和新文件完整；不含样本EXE/DLL
- 仅发布目录+系统PATH可离屏启动；原入口、模式、配置兼容回归通过
- 在独立副本恢复旧程序/旧配置，新账本保留；用户前台无窗口

**失败与回退**
- 固定路径切回经过验证的旧解压目录与旧配置副本
- 未决ledger保留并向用户展示，不通过回退清零

**本票不做**
- 不把压缩包当唯一交付
- 不自动启动可见窗口
- 不宣布M2实时OCR或M4已完成

## 每票提交的证据

统一保存 source_revision、build_dir、fixture_sha256、command、stdout、stderr、exit_code、assertion_count。测试必须指向本票源码构建的二进制。不能将文档schema验证改名成应用验收，也不能把planned_not_run批量改成PASS。
