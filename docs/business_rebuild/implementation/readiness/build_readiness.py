"""Generate documentation-only PR backlog and test specifications. No application execution."""
from pathlib import Path
import json

HERE = Path(__file__).resolve().parent

def save(name, data):
    (HERE/name).write_text(json.dumps(data, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')

tickets = []
def ticket(id, title, wi, b, t, deps, inputs, outputs, files, checks, rollback, decisions, excludes):
    tickets.append(dict(id=id, title=title, status='planned_not_implemented', work_items=wi.split(),
      business_ids=b.split(), acceptance_ids=t.split(), depends_on=deps.split(),
      inputs=inputs, outputs=outputs, suggested_files=files, acceptance=checks,
      failure_rollback=rollback, unresolved_decisions=decisions.split(), non_goals=excludes,
      evidence_ids=['E08','E11','E12'] + (['E03','E04'] if id not in ['PR01','PR12'] else []),
      reference_ids=['R08','R12'] if id not in ['PR10','PR12'] else ['R12','R13','R15'],
      result_required=['source_revision','build_dir','fixture_sha256','command','stdout','stderr','exit_code','assertion_count'],
      estimated_size='一个可独立审查和回退的PR；耗时按实现评估，不承诺工期'))

ticket('PR01','建立领域库和可追溯测试入口','WI01 WI05','B39 B42 B43','T39-A T42-A T43-A','',
 ['E08现有CMake/build.ps1/src/domain.*与domain_tests；engineering_baseline.md；domain/runtime公开契约'],
 ['relink_business静态目标只链接QtCore；旧demo目标行为不变','构建记录输出实际buildDir与toolchain；测试路径绑定同一产物','把批准的契约fixture注册为测试输入，不生成伪业务实现'],
 ['CMakeLists.txt','build.ps1','src/business/CMakeLists.txt（或根目标段）','tests/business/test_support.h'],
 ['现有domain_tests与ui_offscreen在本次构建目录通过','新的空壳本身不算功能完成；链接依赖审查无QtGui/捕获/输入组件','验证当前build_relocated而非旧build运行路径'],
 ['恢复CMake/build脚本变更即可；不改用户配置/发布目录','若旧基线失败，先单独修复或记录已有失败，不吞掉失败继续开发'],
 '', ['不迁移AppState','不引入OCR/SQLite','不覆盖发布EXE'])

ticket('PR02','实现精确数字、不可变观测和规则对象','WI01','B04 B14 B15 B20 B39','T04-B T14-A T14-B T15-A T15-B T20-B T39-B','PR01',
 ['domain/core.schema.json与字段字典','domain/fixtures/rule-golden.json中的合法/非法十进制和对象样例'],
 ['从字符串解析Decimal与Money，24位unscaled、scale0..12按域契约处理','值对象保留raw/source/revision；商品与卖单分离','结构/语义错误返回字段路径和稳定诊断；不先经过double'],
 ['src/business/decimal.h','src/business/decimal.cpp','src/business/models.h','src/business/model_codec.cpp','tests/business/value_tests.cpp'],
 ['同值异scale比较相等；最大值/超长/指数/负数/非法scale按golden结果拒绝或接受','缺失与0区分；未知单位不是默认货币','旧v1 double保留在旧demo适配层，没有改义；schema与C++接受集合一致'],
 ['新目标保持可移除；旧AppState继续工作','数字契约不匹配时停止合并并修改同版本golden评审，不临时加epsilon'],
 'D13', ['不定义市场实际币种','不决定真实数量周期','不实现视觉'])

ticket('PR03','实现带原因码的纯规则匹配器','WI01','B11 B14 B15 B16 B20 B28','T11-A T11-B T14-A T14-B T15-A T15-B T16-A T16-B T20-A T20-B T28-B','PR02',
 ['域契约Decision优先级、字段可靠性和新鲜度规则','rule-golden.json完整输入+expected；运行上下文显式注入'],
 ['match(rule, observation, context)返回Match/NoMatch/NeedsReview及逐字段原因','结构/语义非法输入与业务不匹配分开','Match仅说明规则符合，eligible_for_action保持false；执行额度由runtime显式快照判定'],
 ['src/business/rule_matcher.h','src/business/rule_matcher.cpp','tests/business/rule_tests.cpp'],
 ['全部golden逐字段相等，不只比较enum','未审配置、陈旧/歧义/缺字段/单位问题按优先级给Review；闭区间边界包含相等','未知限购不被转换成无限；测试反例证明Match不发意图','相同输入重复执行没有IO/系统时间/随机数依赖'],
 ['恢复纯函数实现不影响配置；继续显示旧demo','任何优先级变更先修改契约/反例再实现，禁止只修单例绕过'],
 'D02 D05 D07 D13', ['不调用截图或OCR','不操作外部程序','不把Match当发起许可'])

ticket('PR04','实现可控时钟与确定性离线回放','WI03','B02 B22 B35 B43','T02-A T22-B T35-A T35-B T43-A T43-B','PR03',
 ['runtime/state_machine.md、state.schema.json和fixtures/replay_cases.json','domain规则fixture和可控单调时钟输入','模式/epoch/step/事件顺序约束'],
 ['ReplayEngine只读结构化fixture，支持start/pause/step/stop','按输入seq/假时钟推进；取消后迟到结果不改变状态','观察需求与实际采集计数分离：M1假源，真实capture=0','复现确定的状态/事件/意图数量；输出来源replay'],
 ['src/replay/fake_clock.h','src/replay/replay_reader.cpp','src/replay/replay_engine.cpp','tests/replay/replay_tests.cpp'],
 ['相同输入两轮输出去除明确声明的运行标签后完全一致','pause/stop重复调用不多建run；旧epoch/重复/乱序事件按契约处理','Ready/Paused/Stopped/长等待无新增观察需求；不创建截图文件','IO失败/错误fixture不会推进半个状态；事件序列有逐项oracle'],
 ['停止新ReplayController并恢复旧demo入口；保留只读fixture','回放失败可重启新run，不能悄悄续用半污染状态'],
 'D03 D06 D10 D11', ['不实现实时捕获','不模拟未知动态公式','不更改系统时间'])

ticket('PR05','交付最小规则回放到原UI纵切片','WI05','B02 B04 B16 B20 B38 B40 B43','T02-A T04-A T16-A T20-A T38-A T40-A T40-B T43-A T43-B','PR04',
 ['集成ui_integration.md；PR03/04的只读状态投影','现有mainwindow/widgets和E11布局约定'],
 ['在原关注列表/条件面板/记录区显示回放模式、来源、观测时刻、决策原因','新增控制仅发run命令；旧demo继续可用且统计隔离','优化入口原位保留未定义说明；按需观察策略在状态栏可解释'],
 ['src/application/run_controller.h','src/application/ui_projection.cpp','src/mainwindow.cpp','src/mainwindow.h','src/main.cpp离屏自检扩展'],
 ['离屏展示一条Match、一条NoMatch、一条NeedsReview及相应原因','暂停后UI不继续变；切换demo/replay不会串日志/曲线/计数','E11颜色/顶部搜索/左导航/关注列表/右编辑/底部图和原入口全部保留','纯回放过程中真实capture/worker/input启动数均0'],
 ['功能开关撤回新回放入口，旧演示不受影响','保存的新数据独立路径；撤回UI不删除回放记录'],
 'D08', ['不重做主题/布局','不删除原入口','不因UI连通宣称M1账本已完成'])

ticket('PR06','桥接schema v1而不改变旧演示含义','WI02','B04 B39 B43','T04-B T39-A T39-B T43-B','PR02',
 ['当前AppState v1读写和tests/domain_tests.cpp','版本化v1黄金fixture；domain import-golden与迁移规则'],
 ['独立V1Adapter将旧值转为demo来源的新配置候选','缺condition仍不限；旧RunSettings默认与原解析器一致','迁移诊断与原始值保留；v1原路径只读、v2新路径'],
 ['src/config/v1_adapter.h','src/config/v1_adapter.cpp','tests/business/migration_tests.cpp','tests/fixtures/v1/'],
 ['旧合法文件加载后原有显示与演示行为不变；不自动变Observe','原单位未明确则新执行配置保持未审；double来源不伪装精确真实观测','坏类型/重复ID/失效引用拒绝并保持原内存、源文件hash和活动路径'],
 ['继续使用原v1与旧解析器；丢弃未提交候选','源文件未覆盖，因此无需逆向恢复猜测字段'],
 'D03 D05 D13', ['不批量自动激活','不改旧schema1格式','不接受样本代码或凭据'])

ticket('PR07','实现13列文本只读导入预览','WI02','B05 B14 B15 B39','T05-A T05-B T14-B T15-B T39-B','PR02',
 ['E03 15条13列文本与脱敏schema快照','domain/import-preview.schema.json、import-golden.json'],
 ['行号/编码/源hash/raw13列/候选映射/逐行诊断','第7/8列候选、12/13列原位保留；enabled=false/review_required=true','明确文件/行/字段长度上限、错误编码和重复键策略，限制按域契约执行'],
 ['src/config/legacy_importer.h','src/config/legacy_importer.cpp','tests/business/legacy_import_tests.cpp'],
 ['15条列数/原文往返；源hash不变，输出无spt或登录字段','12/14列、坏编码、超限、空字段、额外管道符均给确定诊断且不半提交','预览被传入runnable入口时明确拒绝；未知列从不变quantity'],
 ['丢弃preview即可；旧配置和当前run保持不变','编码不确定时展示诊断，不用有损解码掩盖'],
 'D01 D02 D03 D12 D13', ['不执行INI路径或命令','不恢复原授权','不猜字段语义'])

ticket('PR08','实现方案仓库与v2审定提交','WI02 WI05','B01 B04 B24 B39','T01-A T01-B T04-A T04-B T24-B T39-A T39-B','PR06 PR07',
 ['domain/config-v2.schema.json、import preview与migration decisions','集成storage_contract.md的配置提交约定','当前界面参数与profile名'],
 ['profile稳定ID/revision；独立新路径v2；QSaveFile全量校验后提交','候选确认记录带字段、决定、来源；有效规则快照不被编辑改变','动态默认关闭，未定限购语义保存但不进入执行许可'],
 ['src/config/profile_store.h','src/config/profile_store.cpp','src/application/profile_controller.cpp','tests/business/profile_store_tests.cpp'],
 ['两方案往返和脏编辑取消；活动run继续用旧revision','写失败/目录不可写/无效引用不覆盖旧文件或当前状态','取消审定、缺单位、未知qty仍不能激活；新原生规则闭区间明确','原RunSettings所有入口字段保留并有映射/原始容器'],
 ['恢复v1程序/配置独立副本；v2与迁移报告保留','事务失败不更改活动configPath'],
 'D02 D03 D04 D05 D11 D13', ['不启用真实动态策略','不写死市场时段','不将schema导出成功等同业务已启用'])

ticket('PR09','实现内存事件仓库、数量预留与回执幂等','WI04','B28 B30 B31 B37 B43','T28-A T28-B T30-A T30-B T31-A T31-B T37-A T43-B','PR04',
 ['integration/storage_contract.md的IEventStore/attempt事务与quota规则','runtime状态/Attempt契约与ledger fixture','显式synthetic quota snapshot'],
 ['IEventStore和InMemory实现；事件/attempt/receipt/投影职责分离','按intent/event/receipt去重；相同ID不同payload作为冲突','预留→发起→成功/失败/未知；Unknown继续占位','必要事件可追溯，逐帧OCR日志不开启'],
 ['src/ledger/event_store.h','src/ledger/in_memory_ledger.cpp','src/business/quota_policy.cpp','tests/business/ledger_tests.cpp'],
 ['重复成功回执只计一次；重复ID不同payload拒绝','明确失败释放、未知不释放；缺限购scope/单位时拒绝新的许可','停止prepared可取消；可能已发出保持Unknown待核验','demo/replay投影与其它模式分库或按契约键空间隔离'],
 ['内存实例可丢弃重放，但不能宣称保存真实外部历史','语义失败时不继续给UI成功计数，回到未决'],
 'D05 D07', ['不提供外部exactly-once保证','不执行外部输入','不声称内存仓库具有崩溃持久性'])

ticket('PR10','加入QtSql/SQLite持久化和崩溃核验','WI04','B28 B30 B31 B37','T28-B T30-B T31-A T31-B T37-A T37-B','PR09',
 ['integration/storage_contract.md与可执行DDL','已通过的内存仓库语义测试复用','Qt6Sql/QSQLITE来源与当前SDK/发布差距'],
 ['QtSql适配器与显式schema迁移；关键状态事务提交','恢复扫描未决attempt，prepared/sent处理遵循契约','DB忙/满/写失败/提交失败明确回报；必要账本与降采样诊断分开','一致性备份、schema版本拒降级；部署Sql DLL/driver清单'],
 ['CMakeLists.txt','src/ledger/sqlite_ledger.h','src/ledger/sqlite_ledger.cpp','tests/business/sqlite_ledger_tests.cpp','build.ps1依赖部署检查'],
 ['进程在事务前/提交前/提交后/回执前受控退出，重开结果等于oracle','数据库唯一约束与逻辑幂等一致；成功投影只在commit成功后更新','未知attempt不自动重发；一致性备份恢复含未决预留','实际发布目录可加载QSQLITE；不只证明SDK可用'],
 ['退回旧程序+独立v1配置但保留v2 DB；新版DB不交旧程序解释','损坏/新schema进入只读诊断，不删除重建伪装成功'],
 'D05 D07', ['不把SQLite WAL当永不丢失保证','不为减少IO跳过关键事务','不优化到逐帧存库'])

ticket('PR11','完善回放、账本、方案和记录的UI投影','WI05','B01 B02 B04 B20 B37 B38 B39 B40 B43','T01-A T02-A T04-A T20-A T37-A T38-A T38-B T39-A T40-A T43-A T43-B','PR05 PR08 PR10',
 ['integration/ui_integration.md','已提交event/attempt/profiles和回放状态；原UI入口清单'],
 ['商品与卖单列表可辨；字段缺失/时间/来源/陈旧可见','记录分匹配/收藏/发起/确认成功/失败/未知；图表区分观测价与成交价','CSV防公式转义；日志限速汇总且不嵌像素/OCR全文','停止/模式切换/脏配置交互和错误提示完整'],
 ['src/application/ui_projection.cpp','src/application/run_controller.cpp','src/mainwindow.cpp','src/main.cpp离屏验收扩展'],
 ['相同商品两卖单不覆盖彼此；成功/Unknown/NoMatch数字与账本相等','模式切换清理旧投影但不删历史；回放快速推进UI合并刷新','配置错误/磁盘失败显示原因，主线程不中断响应','默认图片写入策略UI不被成功或异常自动开启'],
 ['停用新增投影但保留数据库/配置；旧UI继续只演示','失败不降级成由按钮槽函数直接执行业务'],
 'D08', ['不重新设计配色布局','不添加未定义优化功能','不把观测价标实际成交价'])

ticket('PR12','全量回归、离屏打包和解压目录回退验证','WI05','B39 B41 B42 B43','T39-A T41-A T42-A T42-B T43-A T43-B','PR11',
 ['全部前票测试证据与源码revision','实际buildDir、旧发布目录及独立配置/账本备份','E11固定交付路径和依赖manifest'],
 ['参数化verify_delivery的构建目录/版本/断言数量','构建+全CTest+package-only PATH离屏+依赖manifest核对','发布前复制旧目录并在另一目录验证恢复；固定EXE路径更新','交付证据逐命令输出和未执行层说明'],
 ['tests/verify_delivery.py','build.ps1','tests/release/verify_package.py','docs/发布记录'],
 ['刚构建的EXE/hash与被测试和发布者相同','Qt DLL/platforms/Sql driver和新文件完整；不含样本EXE/DLL','仅发布目录+系统PATH可离屏启动；原入口、模式、配置兼容回归通过','在独立副本恢复旧程序/旧配置，新账本保留；用户前台无窗口'],
 ['固定路径切回经过验证的旧解压目录与旧配置副本','未决ledger保留并向用户展示，不通过回退清零'],
 '', ['不把压缩包当唯一交付','不自动启动可见窗口','不宣布M2实时OCR或M4已完成'])

decision_map = {
 'D01':{'tickets':['PR07'],'blocking':'旧未知列语义激活，不阻塞保真预览'},
 'D02':{'tickets':['PR03','PR07','PR08'],'blocking':'旧任务未审自动启用；原生闭区间可开发'},
 'D03':{'tickets':['PR04','PR06','PR07','PR08'],'blocking':'WI10/WI11真实时序/旧延迟迁移；M1仅显式合成时间'},
 'D04':{'tickets':['PR08'],'blocking':'WI10动态策略默认启用；M1保存且关闭'},
 'D05':{'tickets':['PR03','PR06','PR08','PR09','PR10'],'blocking':'真实限购激活与动作许可；M1测试显式合成scope'},
 'D06':{'tickets':['PR04'],'blocking':'WI09正式回补收藏条件；合成回放可做'},
 'D07':{'tickets':['PR03','PR09','PR10'],'blocking':'WI08关联和WI11结果自动归属；歧义保持Review/Unknown'},
 'D08':{'tickets':['PR05','PR11'],'blocking':'优化内容；不阻塞入口保留'},
 'D09':{'tickets':[],'blocking':'WI07 OCR生产选型；M1无视觉依赖'},
 'D10':{'tickets':['PR04'],'blocking':'WI06真实目标捕获兼容；M1假源可做'},
 'D11':{'tickets':['PR04','PR08'],'blocking':'WI10正式时段策略；显式合成计划可做'},
 'D12':{'tickets':['PR07'],'blocking':'旧枚举推演的新功能；不阻塞原文保存'},
 'D13':{'tickets':['PR02','PR03','PR06','PR07','PR08'],'blocking':'真实导入启用/匹配；合成单位值对象可做'}}
backlog = dict(document_version='0.3', status='specification_only', application_implemented=False,
 source_catalog='../../business_catalog.json', minimum_vertical_slice=['PR01','PR02','PR03','PR04','PR05'],
 all_work_items=['WI01','WI02','WI03','WI04','WI05'], tickets=tickets, decision_impact=decision_map)
save('backlog.json', backlog)

lines=['# WI01–WI05 的 12 张可合并任务票','','> 所有票均为 planned_not_implemented。本文件由 build_readiness.py 生成；本轮只检查文档，不执行下面的业务验收。','',
'**先交 PR01–PR05 最小纵切片，再接迁移和持久化。** 每票以一次可审查的变更为单位；验收失败不拖入下一票用新功能掩盖。票据的验收细节见 [测试矩阵](test_matrix.md)。','',
'| 票 | 工作包 | 依赖 | 标题 |','|---|---|---|---|']
for p in tickets:
    lines.append(f"| {p['id']} | {' '.join(p['work_items'])} | {', '.join(p['depends_on']) or '无'} | {p['title']} |")
for p in tickets:
    lines += ['',f"## {p['id']} · {p['title']}",'',f"**关联：** {' '.join(p['work_items'])}；{' '.join(p['business_ids'])}；{' '.join(p['acceptance_ids'])}。",
      f"**证据/资料：** {' '.join(p['evidence_ids'])}；{' '.join(p['reference_ids'])}。依赖：{', '.join(p['depends_on']) or '无'}。未决影响：{' '.join(p['unresolved_decisions']) or '无产品语义阻塞'}。"]
    for key,label in [('inputs','输入'),('outputs','输出'),('suggested_files','建议修改文件（下一轮）'),('acceptance','合并验收'),('failure_rollback','失败与回退'),('non_goals','本票不做')]:
        lines += ['',f'**{label}**']+[f'- {x}' for x in p[key]]
lines += ['','## 每票提交的证据','', '统一保存 source_revision、build_dir、fixture_sha256、command、stdout、stderr、exit_code、assertion_count。测试必须指向本票源码构建的二进制。不能将文档schema验证改名成应用验收，也不能把planned_not_run批量改成PASS。','']
(HERE/'backlog.md').write_text('\n'.join(lines), encoding='utf-8')

cases=[]
def case(id, layer, phase, bt, tickets, given, when, then, fixture, command, caveat):
    cases.append(dict(id=id,layer=layer,phase=phase,status='planned_not_run',business_acceptance_refs=bt.split(),tickets=tickets.split(),given=given,when=when,then=then,fixture=fixture,command_plan=command,not_proved=caveat))

doccmd='python -I -X utf8 docs\\business_rebuild\\implementation\\readiness\\validate_readiness.py'
ct='powershell -NoProfile -ExecutionPolicy Bypass -File .\\build.ps1 -Test'
case('X01','document_schema','M0','B39 T39-B','PR02 PR08','有效config、disabled preview和缺单位/坏类型反例均在schema manifest中','离线JSON Schema验证manifest并对照expected_valid','正例接受、反例在预期路径拒绝；preview不是可执行profile','domain/validation_manifest.json及schema fixture','由根implementation验证入口运行JSON Schema批次','只证明schema形状/明示约束，不证明C++解析或业务函数')
case('X02','document_traceability','M0','B01 B44 T01-A T44-B','PR01','12张票、44业务、88规格和D01-D13存在','运行readiness文档自检','票依赖无环、WI01-WI05全覆盖、B/T/E/R合法、测试状态非已执行','backlog.json + test_matrix.json',doccmd,'不证明业务实现')
case('X03','pure_value','M1','B14 B15 T14-A T15-B','PR02','23.0/23.00、24位上界、scale12及非法指数/负数等golden','执行值对象解析/比较','每个结果与expected一致；不经double、不溢出、不截断','domain/fixtures/rule-golden.json + value边界fixture',ct+'；注册business_value_tests','不证明OCR读数准确')
case('X04','pure_rule','M1','B14 B15 B16 T14-A T15-A T16-A','PR03','明确单位、有效观察，价格恰好上下界且wear等于阈值','调用match','Match和逐字段满足原因；eligible_for_action=false','domain/fixtures/rule-golden.json',ct+'；business_rule_tests','匹配不是动作许可')
case('X05','pure_rule','M1','B16 T16-B','PR03','价格可靠超界，同时必要磨损缺失','调用match','按canonical优先级NeedsReview，缺失原因可见；不让超价掩盖不可靠输入','domain/fixtures/rule-golden.json混合反例',ct+'；business_rule_tests','不是用OCR分数代表可靠性')
case('X06','pure_rule','M1','B20 B28 T20-B T28-B','PR03 PR09','相同特征的两个卖单、身份歧义；或真实quota scope未明确','匹配并申请执行资格','关联歧义Review；缺quota许可不发新意图，不静默当无限','domain关联fixture + runtime额度fixture',ct+'；business_rule_tests/ledger_tests','合成ID不证明真实市场有稳定ID')
case('X07','migration','M1','B39 T39-A T39-B','PR06','旧v1缺condition/run_settings，旧demo=true；另有坏引用输入','迁移预览再提交新路径','合法默认与旧解析一致且仍demo；非法不改当前状态、源hash、活动路径','版本化v1 golden与domain/import-golden.json',ct+'；migration_tests','不把旧double演示价当真实精确行情')
case('X08','migration','M1','B05 T05-A T05-B','PR07','E03 15条13列与12/14列、乱码、超长反例','导入preview','原值、行号、13列保真；候选禁用；错误逐行，凭据不输出','evidence/task_schema.json + domain/fixtures/import-golden.json',ct+'；migration_tests','不证明第12/13列业务含义')
case('X09','migration','M1','B01 B39 T01-B T39-B','PR08','可用旧配置、待写新配置、注入写入/提交失败','QSaveFile提交或取消切方案','旧文件字节与内存不变、configPath不变、原run快照不变','profile_store临时目录和可注入故障writer',ct+'；profile_store_tests','合法配置持久化不证明策略已被批准')
case('X10','replay','M1','B43 T43-A','PR04','同一事件fixture/相同初始状态和FakeClock','回放两次','state/event/decision序列一致；无真实capture、OCR或input启动','runtime/fixtures/replay_cases.json',ct+'；replay_tests','不证明从像素可提取这些事件')
case('X11','replay','M1','B02 B35 T02-A T35-B','PR04','观察请求在途、cancel_epoch=1，随后暂停到epoch2','送回epoch1的有效旧结果并重复pause','旧结果不推进业务；无新需求；重复pause幂等；资源可draining而run=Paused','runtime/fixtures/replay_cases.json取消用例',ct+'；replay_tests','资源draining不能冒充已经回收')
case('X12','replay','M1','B22 T22-B','PR04','长等待10000ms、无近期观察需求；FakeClock墙钟回拨','推进单调时钟并送旧timer唤醒','长等待中需求数0；过期代次timer无效；唤醒只在新step申请','runtime时钟/长等待fixture',ct+'；replay_tests','不证明真实定时器亚毫秒精度')
case('X13','replay','M1','B02 B43 T02-A T43-B','PR04 PR05','运行中编辑规则或切换mode','继续送旧run事件','旧run用旧revision；新mode不消费旧事件；日志/统计不串','runtime规则快照/mode fixture',ct+'；replay_tests/ui_offscreen','不证明后台全局快捷键已实现')
case('X14','ledger','M1','B28 B30 B31 T28-B T30-B T31-A','PR09','target=1，一条sent后无回执的attempt','超时、stop、重放Start','保持Unknown预留；新尝试不获额度；停止不删除未决','integration ledger oracle + runtime Attempt fixture',ct+'；ledger_tests','不宣称外部exactly-once')
case('X15','ledger','M1','B30 B31 T30-A T31-B','PR09 PR10','同receipt_id两次相同payload，再一次不同payload','提交事务并读取投影','前两次成功只计1；不同payload冲突明确，历史不被覆写','integration receipt/idempotency fixture',ct+'；ledger_tests','收到成功文字但归属不明仍不能套入此正例')
case('X16','ledger_storage','M1','B31 T31-A T31-B','PR10','DB事务在提交前/后、intent已durable而回执未到的状态','在精确注入点结束自家测试子进程并重新打开DB','提交前无半条记录；提交后可重读；未决保持待核验，不重发','integration/storage_contract.md故障注入点 +临时DB',ct+'；sqlite_ledger_tests（自家子进程hidden）','不运行BBZPS或外部动作；不以正常关闭替代崩溃测试')
case('X17','ledger_storage','M1','B37 T37-A T37-B','PR10','数据库锁忙/满/磁盘写失败；UI订阅成功计数','尝试提交结果','提交失败则UI不加成功数，暂停并保存可读诊断；恢复后不重计','IEventStore故障注入实现',ct+'；sqlite_ledger_tests','日志降采样不允许省略必要交易事务')
case('X18','ledger_storage','M1','B31 B39 T31-A T39-B','PR10 PR12','含WAL数据/未决attempt的账本与独立v1备份','按契约一致性备份并在副本恢复旧程序','新账本与未决数据保留；旧程序只读v1；不只复制运行中主DB','integration备份fixture +发布副本',ct+'；sqlite_ledger_tests/发布回退计划','不保证任意故障硬件上的绝对不丢数据')
case('X19','image_quality','M2','B07 B08 T07-A T08-B','','独立采集会话划分的标注集，含页面/框/文本/卖单关联','离线评测候选OCR与预处理','每字段精确率/拒识/覆盖/行关联/误匹配单独报告；同帧变体不跨集','待M2建立的有来源图像真值清单','拟新增vision_benchmark CLI；在WI07注册后固定命令与模型hash','结构化历史OCR输出不能证明OCR精度')
case('X20','image_quality','M2','B14 B15 B20 T14-B T15-B T20-B','','价格千分位断裂、O/0、邻行磨损、重复商品、高DPI负例','从原始像素到ListingObservation','错误/歧义保留Unknown/Review；不把相邻行字段拼成合格卖单','M2逐字段人工真值及负例集','拟新增vision_benchmark --suite fields-negative','零观测误报只能描述该测试集，不能外推真实零风险')
case('X21','capture_contract','M2','B06 B44 T06-A T44-B','','step_id/epoch/viewport明确的新帧需求与两个占用槽','resize/DPI变化并送旧frame','旧代次失效；ROI映射往返在定义容差内；目标关闭为错误非空页','runtime帧/ROI schema +合成DPI场景','拟新增capture_contract_tests','不证明具体目标的全屏/遮挡捕获兼容')
case('X22','capture_lifecycle','M2','B06 B44 T06-B T44-B','','worker正读取租约，coordinator已cancel且超时','观察释放/退出前后buffer复用','取消立即停新需求；release或确认退出前不复用；回收后计数归零','runtime租约状态fixture+可信mock worker','拟新增worker_contract_tests --suite lease-lifetime','逻辑取消不等于线程/进程已停止读内存')
case('X23','capture_io','M2','B06 B07 B37 B44 T06-B T07-B T37-B T44-B','','默认持久化false；覆盖正常/成功/失败/worker崩溃/共享内存失败','执行自有只读捕获测试并观测应用进程树文件操作','图像/临时帧文件创建次数0、图像写入bytes0；配置和必要账本另计','runtime默认policy；自有测试窗口；进程树/路径/写入观测报告','拟新增capture_io_tests +受控文件IO观察，WI06前冻结操作命令','不承诺全机零IO/禁用分页；不可只搜索.png扩展名')
case('X24','capture_io','M2','B06 B22 B35 T06-A T22-B T35-B','','Ready/Paused/Stopped/长等待/已超时step，worker模型可驻留','等待观测窗口超过两个正常取帧周期','新增capture请求=0、OCR请求=0；bounded_watch帧/时间预算不越界','runtime观察计数fixture+instrumented真实provider','拟新增capture_io_tests --suite demand-boundaries','M1假源计数不替代本层真实采集计数')
case('X25','capture_io','M2','B37 T37-A T37-B','','手动导出或显式有到期/容量/张数限制调试会话','达到预算/关闭/异常','仅显式会话允许保存；到期后零自动图像写；异常不偷偷续期开启','有界调试policy及文件IO报告','拟新增capture_io_tests --suite explicit-export','正常业务默认仍不落图，不以测试导出更改默认')
case('X26','ui_offscreen','M1','B16 B20 B38 B43 T16-A T20-A T38-B T43-B','PR05 PR11','replay包含Match/NoMatch/Review及同品两卖单','离屏加载、单步、暂停、切回demo','各行来源/观测时间/原因明确，曲线不混数据；暂停后不变','domain+runtime黄金fixture与UI视图断言',ct+'；ui_offscreen扩展','UI截图是测试产物，不是实时目标采集落盘')
case('X27','ui_offscreen','M1','B01 B04 B40 T01-B T04-A T40-A T40-B','PR05 PR11','原界面全部入口、100%/125%/150%缩放、长中文/错误信息','运行自家Qt离屏布局检查','Win11白灰/无蓝色；导航/关注/右编辑/底图保留；控件不遮挡；优化不臆造','E11入口清单、离屏snapshot与geometry assertions',ct+'；另以QT_SCALE_FACTOR矩阵跑ui_offscreen','截图肉眼好看不替代字段/可达性断言')
case('X28','release_package','M1','B41 B42 T41-A T42-A T42-B','PR12','当前revision新构建、QtSql新增依赖、旧目录备份','build -Test -Package，清空SDK路径后离屏启动发布EXE','二进制hash一致；Qt DLL/platforms/qsqlite齐；无样本组件；命令退出0','发布manifest与SDK隔离环境','powershell -NoProfile -ExecutionPolicy Bypass -File .\\build.ps1 -Test -Package；参数化verify_delivery','SDK环境下通过不证明发布依赖完整')
case('X29','release_package','M1','B39 B43 T39-A T43-B','PR12','备份旧解压目录/旧v1、新v2账本含未知attempt','在另一副本执行回退并打开旧配置（离屏）','旧行为恢复；新账本未删除；固定交付目录可切换；无可见窗口','独立rollback-test目录与hash清单','PR12回退脚本（仅验证副本）；按包manifest复核','单纯复制旧EXE不是完整回退')
case('X30','logging_budget','M1','B37 T37-A T37-B','PR09 PR10 PR11','大量重复诊断、少量关键状态/intent/receipt；默认无逐帧OCR全文','压测必要事件和日志汇总','关键账本不丢，重复诊断限速并有被抑制计数；容量有限、记录不含像素/凭据','integration日志预算与必要事件fixture',ct+'；ledger_tests与UI响应观察','降低日志量不能等同数据持久性已充分验证')

save('test_matrix.json',dict(document_version='0.3',status='specification_only',application_tests_run=0,cases=cases))
lines=['# 测试设计与88条规格的缺口补齐','','> 本表是执行设计，不是执行结果。全部 X 用例仍为 planned_not_run；M0文档检查在根验证报告登记，不改业务用例状态。源数据：[test_matrix.json](test_matrix.json)。','',
'## 分层与证据边界','',
'| 测试层 | 何时 | 输入/输出 | 不能替代 |','|---|---|---|---|',
'| 文档schema/追踪 | 本轮M0 | schema接受/拒绝、互链、oracle结构 | 业务实现单测 |',
'| 纯值/规则/迁移 | M1 | 精确数字、Decision、原子迁移 | OCR和采集 |',
'| 回放 | M1/M3 | 状态/事件/请求计数逐项oracle | 像素识别 |',
'| 账本故障 | M1 | 幂等/事务/崩溃/未知预留 | 外部操作exactly-once |',
'| 图像质量 | M2 | 独立图像真值、留出集字段指标 | 真实捕获兼容/动作结果 |',
'| 捕获+IO | M2 | 自有窗口、可信worker进程树、租约和文件操作 | 全机无IO或真实市场成交 |',
'| UI离屏 | M1/M4 | 几何/入口/模式/来源/状态及测试snapshot | 后端判断正确性 |',
'| 包装/回退 | 发布前 | 脱SDK启动、manifest、独立副本恢复 | OCR性能 |','',
'## 现有88条为何不能直接当回归测试','',
'现有T01–T44的A/B描述保留为产品验收上层；新增X是更细的工程试验，不替换或虚增“已通过”数量。具体缺口：','',
'| 上层T | v0.2不足 | 下钻用例 |','|---|---|---|',
'| T14/T15/T16 | 无混合缺失优先级、24位精度/scale/溢出oracle | X03–X05 |',
'| T20/T28 | Match与执行资格易混淆；未知限购是否默认无限未测 | X06/X14 |',
'| T05/T39/T01 | 文件字节/坏编码/事务失败/快照与路径不变未细化 | X07–X09 |',
'| T02/T22/T35/T43 | pause/stop竞态、旧epoch、长等待、模式隔离顺序未固定 | X10–X13 |',
'| T30/T31/T37 | 只提幂等/崩溃，缺精确注入点与冲突payload | X14–X18/X30 |',
'| T07/T08/T14/T15/T20 | 图像真值、跨集泄漏、行关联、覆盖和拒识口径不足 | X19/X20 |',
'| T06/T44 | 取消不等于释放、双槽内存复用竞态、OSD/DPI细节不足 | X21/X22 |',
'| T06/T07/T37/T44 | 成功/失败/崩溃、子进程IO、文件类型绕过、显式调试预算不足 | X23–X25 |',
'| T16/T20/T38/T43/T40 | 只有可达性，缺具体UI投影/多卖单/缩放 | X26/X27 |',
'| T39/T41/T42 | 构建缓存迁移、SDK依赖泄漏、Sql driver和副本回退未固定 | X28/X29 |','',
'## Given / When / Then 规格','']
for c in cases:
    lines += [f"### {c['id']} · {c['layer']} / {c['phase']}",f"关联：{' '.join(c['business_acceptance_refs'])}；{' '.join(c['tickets']) or 'WI06–WI08后续视觉工作项'}。状态：planned_not_run。",'',
      f"- **Given：** {c['given']}",f"- **When：** {c['when']}",f"- **Then：** {c['then']}",
      f"- **Fixture：** `{c['fixture']}`",f"- **执行计划：** `{c['command_plan']}`",f"- **不证明：** {c['not_proved']}",'']
lines += ['## 命令与门槛的执行纪律','','带“拟新增”“注册后”的命令是下阶段目标，不应在本轮伪执行或写成已通过。既有build.ps1负责PATH和实际构建目录；CTest筛选命令需同一环境。每个golden的expected由规格独立给出，不能调用被测实现生成expected再与自己比较。','',
'失败记录至少包含case_id、fixture hash、expected、actual、build hash、完整command/stdout/stderr/exit code、注入点和时间基准。业务数、回放事件数、实际截图数、OCR数、图像文件bytes和必要账本bytes分列；不把观察请求数等同已成功捕获帧数。','']
(HERE/'test_matrix.md').write_text('\n'.join(lines), encoding='utf-8')
print('READINESS_GENERATED tickets='+str(len(tickets))+' tests='+str(len(cases))+' application_execution=0')
