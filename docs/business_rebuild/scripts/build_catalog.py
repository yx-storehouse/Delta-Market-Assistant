"""Build traceable business inventory and planned acceptance cases, without sample execution."""
from pathlib import Path
import hashlib
import json
import re
import shutil

ROOT = Path(__file__).resolve().parents[3]
DOC = ROOT/'docs/business_rebuild'
EVD = ROOT/'artifacts/bbzps_static'

# name, basis, evidence, references, module, phase, current, requirement, positive case, negative case
ROWS = [
('方案管理','用户约定','E08 E11','R12','CFG','M1','已有 profile 字段；独立多方案仓库待建','方案有稳定 ID、版本、独立任务与运行参数；切换不隐式启动。','切换两份不同方案后，各自任务/参数完整往返。','切换时存在未保存编辑，明确保存或放弃，不污染另一方案。'),
('运行、暂停、停止与快捷键','用户约定','E08 E11','R09 R10','RUN','M1','现有 start/pause 是模拟','按 run_id 管理运行；暂停冻结新意图，停止取消待办；快捷键具有防抖且可显示冲突。','连续两次开始只有一个活动 run；停止后不再产生新意图。','快捷键冲突或重复按键不启动第二个运行实例。'),
('定时与市场开放检查','证据确认+用户约定','E03 E04 E11','R08 R16','SCHED','M3','已有起止时间配置','用户定时与市场开放是独立条件；支持跨午夜及显式时区；未开放进入暂停而非失败。','09:00—23:00 与 23:00—次日01:00 两类区间边界按规格判定。','市场状态未知或本机时钟回拨时不误判为可执行。'),
('收藏任务编辑与成色规则','证据确认+用户约定','E03 E08 E11','R12','CFG','M1','已有商品/价格/磨损/数量/成色','区分成色枚举、磨损数值、品阶、稀有度；支持新增编辑删除、启停、复制；运行使用规则快照。','同一枪皮的 S/A/B/C 四条规则独立保存并解释匹配。','无效价格区间、空名称、重复 ID、运行中改规则均不悄悄改变旧 run。'),
('BBZPS 13 列任务导入','证据确认+推断','E03','R12','CFG','M1','尚未实现','只解析文本；未知末两列和有歧义的价格/时间先保留 raw 与 review 状态；不导入凭据。','15 条现有样例均解析为 13 列、保留行号及原值；导入预览不启动。','12/14 列、坏编码、非法数字的行进入错误列表且不覆盖原方案。'),
('步骤按需采集、DPI 与坐标变换','证据确认+用户补充','E04 E12','R03 R04 R05','CAP','M2','尚无截图模块','仅业务步骤提出需求时获取新帧；空闲/暂停/长等待停采集；图像走内存。Frame带DPI、尺寸、代次与单调时间。','缩放与移动保持坐标可映射；步骤开始取新帧，结束即停止；空闲/暂停新增采集请求为0。','黑帧、窗口变化令旧ROI失效；实时路径在成功/失败时均不生成图片或临时帧文件。'),
('全屏/ROI 图像处理与 OCR','证据确认+用户补充','E02 E04 E10 E12','R01 R02 R06 R07','VIS','M2','尚无视觉依赖','OpenCV与独立OCR按步骤处理内存ROI；原图暂存有界缓冲区，用完释放；不编码或落盘中间图片。','同一标注集得到整页/ROI结果；实时识别接收内存缓冲区，用完归还。','空结果/低可信/推理失败不当成功；无观察需求时OCR请求为0，失败不自动保存图像。'),
('页面分类与导航状态','证据确认','E04','R01 R06','NAV','M2','尚未实现','识别仓库、市场入口、筛选、商品列表、详情、关注列表、确认/结果弹窗；多锚点判定，Unknown 为正常状态。','固定截图序列生成预期页面状态及证据锚点。','单个同名文字出现在错页时不据此推进行为状态。'),
('赛季、拥有状态与品阶筛选','证据确认','E03 E04','R06','NAV','M3','Task 未包含这些字段','设置前后读取实际筛选值；品阶与稀有度独立；不支持的筛选停在可解释的状态。','全部赛季/未拥有/史诗品阶按顺序形成筛选意图，回读后确认。','筛选点击无回执或菜单被遮挡时不假设已选中。'),
('目标枪皮查找与翻页','证据确认','E04','R01 R06','NAV','M3','尚未实现','名称归一化但保留原文；分页进度/重复页检测；找不到时记录原因并转下一任务。','目标分布在多页时各规则均被访问且同页不反复扫描。','连续重复页或翻页无变化触发上限而不无限循环。'),
('成色筛选','证据确认','E03 E04 E08','R06','RULE','M1','现有 condition 匹配','不限与 S/A/B/C 分离；成色标签和磨损阈值共同约束，不把样例阈值当通用成色定义。','成色A且磨损1.187788可由独立 A 规则判断。','只满足磨损但成色不符，返回 condition_mismatch。'),
('公示期筛选','证据确认','E03 E04','R06 R08','RULE','M3','尚未实现','规则筛选公示状态与运行倒计时分开；缺失数据为 Unknown，原文可追溯。','不限公示期与明确状态规则生成不同匹配结果。','公示期 OCR 缺失不自动解释为公示已结束。'),
('稀有度与排序','证据确认','E03 E04','R06','NAV','M3','当前 Skin.rarity 含义需迁移','稀有度独立字段；区分请求排序和观测排序；默认排序不等于价格升序。','请求按稀有度升序后记录回读值，且不授权价格提前终止。','排序未知、刚刷新或顺序反转时清除有序假设。'),
('价格提取与区间判定','证据确认+推断','E03 E04','R06','RULE','M1','模拟含闭区间过滤','金额按明确单位的定点整数处理；千分位经格式校验；上下限边界是新规格而不是样本确证。','字符串1,300解析1300单位；候选340命中含340的闭区间。','+3、34O、NaN、负数及跨行错配不进入有效金额。'),
('磨损提取与阈值','证据确认','E03 E04','R06','RULE','M1','模拟含 maxWear 过滤','磨损独立解析，保留原文和精度；新模型采用十进制定点；禁止凭成色反推实际磨损。','1.187788 ≤ 1.249通过；边界精确相等通过。','空值、溢出、错行数字或阈值外的结果返回原因，不截断凑阈值。'),
('联合匹配与可解释原因','证据确认+重构设计','E04 E08','R06','RULE','M1','当前模拟布尔过滤','纯函数输入规则和观测，返回 Match/NoMatch/NeedsReview 及逐字段原因；缺字段不放宽筛选。','名称/价格/磨损/成色同时满足时才输出匹配，并携带 rule_revision。','价格符合而磨损不符返回 wear_exceeded，不进入收藏意图。'),
('自动收藏与成功回执','证据确认+用户约定','E04 E11','R08','COLLECT','M3','autoCollect 仅保存','匹配→收藏意图→观测成功提示/关注状态；autoCollect 关闭仅展示匹配；回执与原条目关联。','回放匹配后出现成功添加至我的关注，写一次收藏完成事件。','意图已发出但无回执归为未知/待核验，不计收藏成功。'),
('超价提前结束扫描','证据确认+重构设计','E04','R06','COLLECT','M3','尚未实现','只有证实全局单调价格升序且分页排序稳定，才用超上限终止余项；否则逐条跳过。','已验证全局价格升序下首个超上限条目终止扫描。','默认/稀有度排序或仅局部升序时，高价之后的低价仍被扫描。'),
('关注上限与阶段切换','证据确认','E04','R08','COLLECT','M3','尚未实现','观测到关注已满即停止收藏转向关注阶段；上限数值未知，不在实现硬编码。','关注满额事件只触发一次阶段切换，并保存未完成游标。','误识别或低可信提示先复核；来回提示不导致两阶段抖动。'),
('关注列表与卖单关联','证据确认+重构设计','E04 E08','R06','LISTING','M1','当前关注是商品层演示','商品与卖单分离；无稳定卖单ID时用 observation association 并标注不确定性；位置只是瞬时属性。','同款不同价格/磨损的两条卖单可独立展示并关联观测。','刷新后坐标交换或两条特征完全相同，不误合并为同一可操作订单。'),
('时钟偏移估计','证据确认+重构设计','E03 E04','R08 R16','CLOCK','M3','尚未实现','估计服务端偏移及不确定度；单调时钟计算经过时间；不修改系统时间，失败标明 degraded。','时间回拨5秒时既有超时长度保持不变；偏移估计单独更新。','所有时间源失败或延迟异常时不继续声称时间精准。'),
('公示倒计时与队列','证据确认+用户补充','E04 E12','R08 R09','SCHED','M3','尚未实现','倒计时采用区间和代次；长等待不持续取帧，到点唤醒再观察；临近阶段只开启有界观察。','新观测更新候选并取消旧定时器；长等待的截图/OCR请求为0，唤醒后获取新帧。','反跳/条目消失/迟到不盲用旧坐标；取消后待唤醒任务不会重开采集。'),
('固定购买延迟','用户约定+样本参数线索','E03 E08 E11','R08 R09','SCHED','M3','purchaseDelayMs 默认830，仅保存','新字段显式毫秒；触发基准必须在适配器规范中定义；BBZPS clicktime 单位未定，暂不换算。','本地测试事件t0加830ms形成计划时间，日志记录计划/实际误差。','原值0.847的导入不自动转换成847ms或0.847ms。'),
('动态延迟区间/自调节','推断+用户约定','E03 E04 E11','R08 R09','SCHED','M3','dynamicDelay 仅保存','原程序精确公式未知；先定义策略接口及上下限/步长/证据记录，第一版默认关闭。','测试策略在明确触发事件后调整且始终处于拟定边界内。','缺少单位、未确认触发或样本不足时维持原值并显示未启用原因。'),
('队列已满减延迟','用户约定','E08 E11','R08 R09','SCHED','M3','queueFullTrigger/StepMs 仅保存','与BBZPS参数映射未证实；拟每达到独立连续触发次数后减一个步长，重复帧不重复计数；待决D04。','测试注入两个独立触发及threshold=2，减一个步长并记录原因。','同一个提示连续20帧只计一个事件；成功后计数复位。'),
('公示期提示加延迟','用户约定','E08 E11','R08 R09','SCHED','M3','publicityTrigger/StepMs 仅保存','拟以确认尝试后仍显示公示期的回执计数，而非日常倒计时画面；策略待决D04。','独立尝试达到设定次数才增加一个步长。','只是在等待公示期的普通画面不触发增量。'),
('连点、间隔与次数','证据确认+用户约定','E03 E04 E11','R08 R09','ACTION','M4','burstClick/clickIntervalMs 仅保存','拆分用户连点参数与动作策略；停止/失焦/变页立即清空余项；未知结果不作为无限重试条件。','测试窗口执行有界意图序列，记录计划次数和真实发起次数。','第2次前页面改变或取消，后续意图全部失效。'),
('数量目标与品级限购','用户约定+样本参数线索','E03 E08 E11','R13','LEDGER','M3','Task.quantity 和橙紫蓝限购仅保存','数量根据已确认成功+未决预留计算；限购周期、颜色到品阶映射待决D05；99999不默认解释为无限。','目标3且成功2/预留1时，第四个意图被阻止。','未知结果不释放预留；重启恢复后不重复占用或丢失额度。'),
('确认与二次确认','证据确认','E04','R08','ACTION','M4','尚未实现','动作前复核卖单/价格/窗口代次；确认弹窗是独立状态；发起和成功分别记账。','模拟窗口依次给出首确认和二次确认，产生同一个attempt下的不同阶段事件。','弹窗对应名称/价格不一致时终止该意图并要求复核。'),
('成交、已售、下架与未知结果','证据确认','E04','R13','LEDGER','M3','simulateTick 只产生演示成功','明确成功/明确失败/结果未知三分；已售或下架不是本人成交；购买全部结束不是成功依据。','可关联成功回执只增加一次成功数；下架只写失败原因。','仅有已点击二次确认或流程结束事件时成功数维持不变。'),
('幂等账本与崩溃恢复','重构新增','E04 E08','R13','LEDGER','M1','尚未实现','持久化意图/发起/回执；重启进入核验而不是重发；无订单ID时不承诺外部操作exactly-once。','重复回执ID只入账一次；发起后进程退出，恢复为Unknown待核验。','回执写库失败时不更新UI成功数，也不立即重新发起。'),
('挂机：刷新页面','用户约定','E08 E11','R08','NAV','M4','refreshPage 仅保存','只在配置允许且无未决确认时刷新；刷新使旧观测、坐标和意图失效。','本地测试页面刷新后创建新frame/viewport generation。','有待核验结果时不通过刷新抹去证据。'),
('挂机：跳过抽奖页','用户约定','E08 E11','R01 R06','NAV','M4','skipLotteryPage 仅保存','需独立抽奖页样本及退出控件锚点；与购买确认页分开。','测试抽奖页且开关开启时生成退出意图。','只有近似文字或开关关闭时不生成退出动作。'),
('挂机：跳过成功页','用户约定','E04 E08 E11','R06 R13','NAV','M4','skipSuccessPage 仅保存','先核验/留证/提交账本，再关闭成功页；关闭不能替代成交确认。','成功回执入账成功后才生成关页意图。','存储失败或回执关联不明时保留证据，不先关弹窗。'),
('异常、重试、超时与卡屏','证据确认+用户补充','E04 E12','R08 R09','RECOVERY','M3','尚无真实运行恢复','观察重试有截止时间/帧预算；暂停停采集；错误留结构化记录但不自动落图；外部副作用不盲重试。','预算耗尽进入暂停并释放观察资源；最后已识别字段保留，图片标记未保存。','正常倒计时等待不误报卡死；错误风暴不产生截图文件、逐帧日志或无界内存积压。'),
('OSD 收藏状态叠层','用户约定','E08 E11','R05 R10','UI','M4','collectOsd 仅保存','叠层只展示状态与停止入口；不覆盖关键识别区域，不参与自身OCR；多显示器DPI自适应。','测试帧不含叠层或已排除叠层区域；显示任务/阶段/识别状态。','叠层被捕获时识别器不把自身文字当作目标应用回执。'),
('记录、按需导出与日志轮转','证据确认+用户补充','E04 E08 E11 E12','R13','OBS','M1','有演示日志及统计','只持久化必要事件；截图默认关闭，包括成功/失败；用户手动或有界调试才保存图片；不逐帧记录OCR全文。','各阶段计数独立，attempt可追到字段/规则版本；默认全流程图像写入字节数和图片文件数为0。','未保存图像不显示可回看；显式调试到期后恢复默认零图片写入；账本失败仍明确提示。'),
('价格历史图与数据导出','用户约定+重构设计','E08 E11','R12 R13','UI','M1','图表和CSV当前为演示','观测价不等于成交价；曲线标注来源、成色、时间与缺口；CSV保留数据来源标签。','商品有多卖单时曲线标注聚合方式并能查看样本数。','数据源断开不补出虚构价格，也不把演示点混入真实曲线。'),
('配置导入导出与版本迁移','用户约定','E08 E11','R12','CFG','M1','schema_version=1，demo=true','新schema v2独立加载；旧版导出仍可回退；未知字段有保留容器；整份校验后原子提交。','v1缺condition时按不限迁移，原文件保留；v2往返不丢未知字段。','新版文件不交给0.6解析器直接覆盖；失败导入不改变当前状态。'),
('优化入口','用户约定+待决','E11','R12','UI','M1','入口已保留，内容未定','继续保留入口；先显示未配置的说明，不臆造清理/系统调参/自动提速功能。','入口可到达且清楚显示功能尚待定义。','点击入口不会执行脚本、改变系统参数或悄悄开启动态延迟。'),
('帮助与诊断信息','用户约定','E08 E11','R15','UI','M4','已有帮助入口','帮助解释字段/单位/模式；诊断导出版本、模型哈希、DPI和错误摘要，不含凭据或整屏隐私。','诊断包可复現捕获布局和配置版本，帮助说明演示与观察的区别。','文本配置的凭据字段在诊断预览和导出中均被排除。'),
('旧授权、依赖与附加功能的边界','证据确认+重构决定','E02 E03 E05 E07','R14 R15','PACK','M0','项目不依赖样本组件','复用业务事实而不复用原EXE/DLL、授权/远控链；无证据的后端账户业务不列为枪皮必要功能。','文档/安装清单只列独立依赖，所有旧授权原值排除。','安装包扫描不包含BBZPS.sp.exe、HD_BBZPS.sp.exe或样本gdi32.dll。'),
('演示、回放与观察模式隔离','重构新增+用户补充','E08 E11 E12','R11 R12','RUN','M1','当前只有演示','Replay只读已有图片/JSON且不写中间图；Observe按步骤取内存新帧；模式切换撤销观察需求，前端不直接发输入。','相同回放产生相同决策；实时模式没有文件路径图像输入或临时帧写入。','切换模式后旧观察申请/帧/worker结果失效，既有测试图片不会误作实时新画面。'),
('视觉 worker 生命周期与兼容','重构新增+用户补充','E08 E12','R10 R11 R14','VIS','M2','尚未实现','控制消息与内存像素分离；实时共享内存不转临时文件；1处理槽+1待处理槽；按租约归还、步骤/会话代次取消。','重启握手一致；租约确认后才复用内存，步骤完成停止取帧，UI可停止。','共享内存失败明确报错而不落盘；旧代次回包丢弃，仍被读取的缓冲区不提前复用。'),
]

def dump(path, obj):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')

def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    catalog=[]; cases=[]
    for i,row in enumerate(ROWS, 1):
        name,basis,ev,refs,module,phase,current,rule,positive,negative=row
        bid=f'B{i:02}'
        tids=[f'T{i:02}-A', f'T{i:02}-B']
        catalog.append(dict(id=bid,name=name,basis=basis,evidence_ids=ev.split(),reference_ids=refs.split(),module=module,
                            phase=phase,current_implementation=current,rebuild_requirement=rule,acceptance_ids=tids,
                            implementation_status='specified_not_implemented'))
        for tid,scenario in zip(tids,[positive,negative]):
            cases.append(dict(id=tid,business_id=bid,scenario_and_expected_result=scenario,status='planned_not_run',
                              environment='离线数据或自有测试窗口；不是样本运行',evidence_ids=ev.split()))
    dump(DOC/'business_catalog.json',catalog)
    dump(DOC/'acceptance_cases.json',cases)
    text=['# 01 · 全量可见业务清单与追踪矩阵','',
          '> “全量”指本次配置、历史日志、静态接口和用户入口约定所覆盖的业务；不等于已恢复受保护主体的全部源码。44 项由证据确认、用户约定、合理推断和重构新增共同组成，类别逐行标注。',
          '', '本表的“阶段”是开发安排，不是已交付状态。M0 为文档，M1 为模型/回放，M2 为视觉/只读观察，M3 为双阶段流程回放，M4 为自有测试窗口闭环与发布准备。测试编号的用例均为**待实现验收规格**。',
          '', '证据 E 编号见 [证据与参考资料](06_evidence_references.md)；R 编号是官方技术资料，不能代替样本行为证据。',
          '', '| ID / 业务 | 依据 / 当前实现 | 重构规则 | 模块 / 阶段 | 证据 / 技术参考 / 验收 |','|---|---|---|---|---|']
    for b in catalog:
        text.append(f"| **{b['id']} {b['name']}** | {b['basis']}<br>{b['current_implementation']} | {b['rebuild_requirement']} | {b['module']} / {b['phase']} | {' '.join(b['evidence_ids'])}<br>{' '.join(b['reference_ids'])}<br>{' '.join(b['acceptance_ids'])} |")
    text += ['', '## 入口覆盖检查', '',
             '| 原界面入口 | 业务编号 |','|---|---|',
             '| 方案、运行快捷键、定时 | B01–B03 |',
             '| 购买延迟、动态延迟、队列已满减延迟、公示期加延迟 | B23–B26 |',
             '| 连点、间隔、品级限购 | B27–B28 |',
             '| 挂机刷新、跳抽奖、跳成功页 | B32–B34 |',
             '| 自动收藏与 OSD、收藏任务含成色 | B04、B11、B17、B36 |',
             '| 优化、记录、帮助、右上角导入导出 | B37、B39–B41 |',
             '| 关注列表、条件编辑、底部价格图 | B04、B20、B38 |',
             '', '## 开发所需机器可读数据', '',
             '- [business_catalog.json](business_catalog.json)：业务、证据、参考资料、开发阶段和验收 ID。',
             '- [acceptance_cases.json](acceptance_cases.json)：每项至少一个正常情形、一个边界/异常情形，共 88 条验收规格。',
             '- [全部验收用例](acceptance_cases.md)：便于人工逐条审查。','']
    (DOC/'01_business.md').write_text('\n'.join(text),encoding='utf-8')
    rows=['# 验收用例目录','', '> 状态全部为 planned_not_run：这里只完成了规格和追踪完整性检查，尚未执行后端业务验收。','',
          '| 用例 | 业务 | 场景与预期结果 |','|---|---|---|']
    rows += [f"| {c['id']} | {c['business_id']} | {c['scenario_and_expected_result']} |" for c in cases]
    (DOC/'acceptance_cases.md').write_text('\n'.join(rows)+'\n',encoding='utf-8')

    source_defs=[
        ('E01','初始样本清单',['artifacts/bbzps_static/source_manifest.json']),
        ('E02','静态 PE / OCR / 授权导出',['artifacts/bbzps_static/pe_summary.json']),
        ('E03','脱敏配置和13列任务',['artifacts/bbzps_static/task_schema.json','artifacts/bbzps_static/configs_redacted.json']),
        ('E04','历史日志索引、消息与连续片段',['artifacts/bbzps_static/log_index.json','artifacts/bbzps_static/business_evidence.json','artifacts/bbzps_static/business_passages.json']),
        ('E05','外层嵌入关系',['artifacts/bbzps_static/embedded_payload.json','artifacts/bbzps_static/nested_pe.json']),
        ('E06','上一轮已完成的载荷静态摘要',['artifacts/bbzps_static/decoded_pe.json','artifacts/bbzps_static/decoded_api_calls.json']),
        ('E07','上一轮静态代码证据索引（不纳入实现）',['artifacts/bbzps_static/ida_survey.json','artifacts/bbzps_static/ida_decompile_0x10004960.json']),
        ('E08','当前工程结构与前端行为',['src/domain.h','src/domain.cpp','src/mainwindow.cpp','CMakeLists.txt','docs/STORE_UI.md','build.ps1']),
        ('E09','上一轮目录复核记录',['artifacts/bbzps_static/source_audit_verification.json']),
        ('E10','OCR 库字符串偏移',['artifacts/bbzps_static/gdi32.dll.interesting.json']),
        ('E11','最新用户交付与入口约定',['AGENTS.md']),
        ('E12','用户补充：按步骤采集与图像默认不落盘',['docs/business_rebuild/evidence/2026-10-06_capture_requirement.md']),
    ]
    index=[]
    for eid,title,paths in source_defs:
        index.append(dict(id=eid,title=title,observed_at='2026-10-06',source_type='user_requirement_record' if eid=='E12' else 'existing_static_artifact_or_project_file',
                          files=[dict(path=p,sha256=digest(ROOT/p),bytes=(ROOT/p).stat().st_size) for p in paths],
                          repro_command=f'python -I -X utf8 docs/business_rebuild/scripts/validate_docs.py --evidence {eid}',
                          linked_workitem='WI06' if eid=='E12' else ('WI01' if eid in ['E03','E08','E11'] else 'WI03'),supersedes='none'))
    dump(DOC/'evidence/index.json',index)
    msg=json.loads((EVD/'business_evidence.json').read_text(encoding='utf-8'))
    # Preserve all normalized business patterns plus the first sample locator; no binary or full log included.
    excerpt=[dict(json_pointer=f'/'+str(i),pattern=x['pattern'],historical_message_count=x['count'],example=x['examples'][0]) for i,x in enumerate(msg)]
    dump(DOC/'evidence/business_excerpts.json',excerpt)
    dump(DOC/'evidence/task_schema.json',json.loads((EVD/'task_schema.json').read_text(encoding='utf-8')))
    interesting=json.loads((EVD/'gdi32.dll.interesting.json').read_text(encoding='utf-8'))
    wanted={'0x4af010','0x5045b0','0x5d9238','0x61f9b4','0x61f9c2','0x61fa2d','0x61fa46','0x61fa50'}
    dump(DOC/'evidence/ocr_identity.json',[x for x in interesting if x['offset'] in wanted])
    shutil.copyfile(ROOT/'docs/2026-10-06_BBZPS-业务逻辑与静态风险分析.md',DOC/'evidence/static-analysis-report.md')
    print(f'CATALOG={len(catalog)} ACCEPTANCE_SPECIFICATIONS={len(cases)} EVIDENCE_GROUPS={len(index)}')

if __name__=='__main__':
    main()
