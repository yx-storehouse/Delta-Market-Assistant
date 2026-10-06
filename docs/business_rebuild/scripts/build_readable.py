"""Build reference chapter, editable diagrams and offline HTML. Does not touch app files."""
from pathlib import Path
import html
import json
import re
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
DOC = ROOT/'docs/business_rebuild'
sys.path.insert(0, str(ROOT/'.tools/doc-render'))
import markdown

def references():
    refs=json.loads((DOC/'references.json').read_text(encoding='utf-8'))
    for r in refs:
        r.pop('marker_excerpt',None)
    (DOC/'references.json').write_text(json.dumps(refs,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    text='''# 06 · 证据与参考资料

## 1. 证据来源与复验

本轮复用上一轮已完成的静态资料，没有重新扫描约1.77GB日志、没有启动样本或加载其组件。证据索引保存的是实际文件SHA-256；本轮工作为开发设计，旧报告里的静态载荷分析不作为新程序代码来源。

| E编号 | 来源 | 可支持的业务发现 |
|---|---|---|
| E01 | source_manifest.json | 初始文件身份、cv2依赖线索；不证明所有依赖均执行 |
| E02 | pe_summary.json | TomatoOCR导出/资源、授权层接口、Python/Qt生态 |
| E03 | task_schema.json / configs_redacted.json | 15条13列任务、可见运行参数、脱敏配置 |
| E04 | log_index.json / business_evidence.json / business_passages.json | 页面/筛选/收藏/关注/倒计时/确认/异常与原文件行号、字节偏移 |
| E05 | embedded_payload.json / nested_pe.json | 旧外层与内层及额外载荷的关系；仅用于解释不搬二进制的决定 |
| E06 | decoded_pe.json / decoded_api_calls.json | 上一轮静态恢复的证据摘要，不纳入实现 |
| E07 | ida_survey.json / 原反编译证据 | 旧载荷代码能力已静态确认；开发沿用结论，不延伸载荷分析 |
| E08 | domain.h/.cpp / mainwindow.cpp / CMakeLists.txt / STORE_UI.md / build.ps1 | 当前前端/模拟实现、字段、构建工具链 |
| E09 | source_audit_verification.json | 上一轮目录结束复核情况；34张历史PNG缺失，原因未核实 |
| E10 | gdi32.dll.interesting.json | OpenCV、ppocrv5、TomatoOCR字符串偏移 |
| E11 | AGENTS.md | 最新Win11浅色白灰、全部功能入口和交付路径约定 |
| E12 | 用户补充的采集要求记录 | 按步骤触发，图像默认只走内存；取代临时帧文件IPC与常驻全速采集方案 |

原文件完整路径与哈希见 [evidence/index.json](evidence/index.json)；业务消息及原日志定位见 [business_excerpts.json](evidence/business_excerpts.json)；动态参数区间等补充片段定位见 [passage_locators.json](evidence/passage_locators.json)。[13列配置快照](evidence/task_schema.json)、[OCR身份摘录](evidence/ocr_identity.json) 和 [上一轮完整静态报告](evidence/static-analysis-report.md) 随文档附带，均为文本资料。

v0.2新增来源：[用户的步骤采集与不落盘要求](evidence/2026-10-06_capture_requirement.md)。该项是产品要求，不是样本已具备低IO实现的证明。

复验一组证据（只读哈希，不执行被检查文件）：

```powershell
python -I -X utf8 docs\\business_rebuild\\scripts\\validate_docs.py --evidence E04
```

索引的source_ref以项目根目录为基准；上述复验需在本项目中进行。压缩包中的离线文档与摘录可直接阅读，但不包含原始日志或任何样本EXE/DLL。原始scope/timeline可在项目 `artifacts/bbzps_static/` 查阅。本轮入口与文档检验记录在 `artifacts/business_rebuild_docs/`。

## 2. Evidence → Finding → Path

| 发现 | 证据 | 结论与强度 | 开发影响 |
|---|---|---|---|
| BF01 | E03+E04 | 高：存在筛选收藏/关注调度两阶段，不只是价格展示 | WI01/03/09/10 |
| BF02 | E01+E02+E10 | 高：包含OpenCV及TomatoOCR接口；具体启用模型仍未知 | WI06/07；不复制OCR DLL |
| BF03 | E08+E11 | 高：现版本为配置与合成数据模拟，原入口需要保留 | WI01/05；不以演示成功充真实业务 |
| BF04 | E03+E04 | 中：价格列、延迟区间存在语义候选；边界/单位/公式未确定 | WI02；D01–D04 |
| BF05 | E04 | 高：结果超时、下架、识别失败、市场暂停是主流程分支 | WI04/09/10/11 |
| BF06 | E05+E07 | 高：旧发行包有独立远控代码静态证据 | WI12使用独立依赖，不打包原二进制 |
| BF07 | E12 | 用户确认：按步骤采集、实时图片默认不落盘 | ADR10、WI06–WI08/WI10；协议、统计与验收同步更新 |

业务路径 BP01（历史行为还原）：E03任务 → E04页面/筛选 → E04价格和磨损匹配 → E04收藏回执 → E04关注/倒计时 → E04确认/结果；支撑BF01/BF05。新实现的账本、取消代次、进程隔离等是设计加固，不冒充上述历史代码路径。

识别路径 BP02（接口/日志综合判断）：E02 OCR接口 + E10 OpenCV线索 → E04文字/框/分数 → E04页面分类与字段判断；支撑BF02。此路径是组件/数据关系，不代表已经恢复每一个函数调用。

## 3. 官方技术参考资料

以下是原始维护者的开发文档/标准，不是搜索摘要或二手博客。访问日期：2026-10-06。16项均实际取得HTTP 200并检查主题关键字；URL、最终地址、页面标题、获取时间、响应长度和SHA-256见 [references.json](references.json)。Qt链接属于6.8文档分支，当前页面可能显示6.8.x较新补丁号，**不代表本机Qt已升级**。

资料解释重构技术，不证明样本实际调用该API，也不是OCR性能达标证据。页面响应哈希是当次取证标识，不是将来网页永不变化的承诺。

| 编号 | 维护者 / 文献标题 | 对应业务与用途 | 核查 |
|---|---|---|---|
'''
    for r in refs:
        text+=f"| **{r['id']}** | {r['title']} | {r['use']} | HTTP {r.get('http_status','?')} / {r['status']} |\n"
    text+='\n## 4. 资料地址\n\n'
    for r in refs:
        text+=f"- **{r['id']}**：`{r['url']}`\n"
    text+='''
## 5. 对应关系怎样使用

- 业务矩阵每行都有E/R/T：E说明为什么列此业务，R说明实现时参考什么，T说明做完怎样验收。
- 同一R可能为多项业务提供工程背景；它不会替代未恢复的业务公式。例如R08/R09只能解释时钟/定时器，不能证明clicktime单位。
- 参考实现由本项目重写；本轮没有咨询样本授权网站、连接控制端或下载样本依赖。
- 主要未确定项见 [决策表](07_decisions.md)，按文档明确缺口继续开发，不把检索资料填成样本事实。
'''
    (DOC/'06_evidence_references.md').write_text(text,encoding='utf-8')
    passages=json.loads((ROOT/'artifacts/bbzps_static/business_passages.json').read_text(encoding='utf-8'))
    loc=[dict(json_pointer='/'+str(i),pattern=x['pattern'],source=x['source'],window_byte_start=x['window_byte_start']) for i,x in enumerate(passages)]
    (DOC/'evidence/passage_locators.json').write_text(json.dumps(loc,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

def diagram(name,title,subtitle,nodes,edges):
    dest=DOC/'diagrams';dest.mkdir(exist_ok=True)
    # Nodes on a vertical swimlane allow a direct, compact SVG without layout-engine dependencies.
    width=1060;height=130+len(nodes)*96
    out=[f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
         f'<title id="title">{html.escape(title)}</title><desc id="desc">{html.escape(subtitle)}</desc>',
         '<defs><marker id="arrow" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="#696969"/></marker></defs>',
         f'<rect width="{width}" height="{height}" rx="20" fill="#f7f7f7"/>',
         '<g font-family="Segoe UI, Microsoft YaHei, sans-serif" fill="#202020">',
         f'<text x="32" y="40" font-size="24" font-weight="600">{html.escape(title)}</text>',
         f'<text x="32" y="68" font-size="14" fill="#666">{html.escape(subtitle)}</text>']
    ids={n[0]:i for i,n in enumerate(nodes)}
    for idx,(nid,label,note) in enumerate(nodes):
        y=94+idx*96
        out += [f'<rect x="32" y="{y}" width="340" height="64" rx="12" fill="#fff" stroke="#d5d5d5"/>',
                f'<text x="51" y="{y+27}" font-size="17" font-weight="600">{html.escape(label)}</text>',
                f'<text x="51" y="{y+49}" font-size="12" fill="#777">{nid}</text>',
                f'<text x="410" y="{y+33}" font-size="15" fill="#4d4d4d">{html.escape(note)}</text>']
    mmd=['flowchart TD']+[f'    {nid}["{label}"]' for nid,label,_ in nodes]
    for a,b,label in edges:
        ia,ib=ids[a],ids[b]
        if ib==ia+1:
            y=94+ia*96+64
            out.append(f'<path d="M202 {y} V{y+29}" fill="none" stroke="#696969" stroke-width="1.6" marker-end="url(#arrow)"/>')
        else:
            y1=94+ia*96+32;y2=94+ib*96+32
            out.append(f'<path d="M32 {y1} H16 V{y2} H29" fill="none" stroke="#969696" stroke-dasharray="4 4" marker-end="url(#arrow)"/>')
        mmd.append(f'    {a} -->|"{label}"| {b}')
    out+=['</g></svg>']
    source='\n'.join(out)
    ET.fromstring(source)
    (dest/(name+'.source.svg')).write_text(source,encoding='utf-8')
    (dest/(name+'.mmd')).write_text('\n'.join(mmd)+'\n',encoding='utf-8')
    (dest/(name+'.graph.json')).write_text(json.dumps(dict(title=title,nodes=nodes,edges=edges),ensure_ascii=False,indent=2),encoding='utf-8')

def diagrams():
    diagram('architecture','现有界面 + 独立业务后端','下面是模块与数据流建议；除现有UI/配置外均待实现。',[
        ('UI','Qt UI / 配置快照','保留 Win11 白灰；开始、暂停、规则与模式命令'),
        ('CAP','步骤按需采集 / 回放源','内存像素；空闲停采集；默认不写图像文件'),
        ('VIS','视觉 worker：OpenCV + OCR','可信独立依赖；文字、框、分数与模型版本'),
        ('SEM','页面 / 卖单语义','正确页面、字段关联、条目新鲜度与歧义'),
        ('RULE','纯规则匹配','Match / NoMatch / NeedsReview + 可解释原因'),
        ('RUN','双阶段状态机 / 调度','步骤申请观察；长等待停采集；有界短时复核'),
        ('ACT','意图适配器（先测试桩）','发起不等于成功；真实输入接入单独决策'),
        ('LOG','回执账本 / UI状态投影','必要结果记录；不逐帧写日志，不自动保存截图')],
        [('UI','CAP','模式/配置'),('CAP','VIS','像素'),('VIS','SEM','识别结果'),('SEM','RULE','观测'),('RULE','RUN','决策'),('RUN','ACT','带代次的意图'),('ACT','LOG','发起与回执'),('LOG','UI','状态投影')])
    diagram('business','两阶段业务与结果核验','历史阶段来自 E04；账本、代次取消、Unknown核验为重构设计。',[
        ('READY','方案 / 任务 / 时间窗口','读取已审规则；市场未开放则等待'),
        ('SCAN','阶段 A：页面与筛选','赛季、拥有、品阶、成色、公示、稀有度、排序'),
        ('MATCH','找商品 → 扫描具体卖单','价格与磨损联合匹配；不符合则下一条'),
        ('COLLECT','收藏意图 → 收藏回执','未见回执不计成功；满额停止收藏'),
        ('WATCH','阶段 B：关注列表 / 倒计时','关联卖单、估计期限；旧帧与旧定时器失效'),
        ('CHECK','再次核验 → 确认流程','对象、价格、页面、窗口、额度共同检查'),
        ('RECEIPT','等待回执：成功 / 失败 / 未知','已售下架不等于本人成交；超时不盲重试'),
        ('PERSIST','结构化记录 / 下一轮或结束','默认不保存图片；未知先核验，完成不等于全成功')],
        [('READY','SCAN','就绪'),('SCAN','MATCH','筛选回读'),('MATCH','COLLECT','匹配'),('COLLECT','WATCH','收藏完成或满额'),('WATCH','CHECK','到点再核验'),('CHECK','RECEIPT','确认'),('RECEIPT','PERSIST','分类与持久化'),('PERSIST','WATCH','下一条/核验')])

CHAPTERS=[('README.md','总览'),('01_business.md','01 业务清单'),('02_vision_architecture.md','02 图像识别 / 架构'),
          ('03_data_config.md','03 数据 / 参数'),('04_state_machine.md','04 状态机'),('05_delivery_tests.md','05 开发 / 验收'),
          ('06_evidence_references.md','06 证据 / 参考资料'),('07_decisions.md','07 决策 / 待确认'),
          ('08_implementation_ready.md','08 开工标准 / 纵切片'),('acceptance_cases.md','88 条验收规格'),('NEXT_IMPLEMENTATION.md','下一轮开发交接')]

def build_html():
    css='''
*{box-sizing:border-box}html{scroll-behavior:auto}body{margin:0;background:#f3f3f3;color:#252525;font:15px/1.85 "Segoe UI","Microsoft YaHei",sans-serif}
aside{position:fixed;inset:0 auto 0 0;width:254px;border-right:1px solid #ddd;padding:32px 22px;background:#f3f3f3;overflow:auto}
.brand{font-size:19px;font-weight:650;letter-spacing:.3px}.sub{font-size:12px;color:#777;margin:6px 0 32px}nav a{display:block;padding:9px 12px;border-radius:9px;text-decoration:none;color:#444;margin:4px 0;font-size:13px}nav a:first-child{background:white;box-shadow:0 1px 4px #00000008;color:#151515;font-weight:600}nav a:hover{background:white}.aside-note{border-top:1px solid #ddd;margin-top:24px;padding-top:20px;font-size:12px;color:#777}
main{margin-left:254px;padding:34px 42px 70px;max-width:1620px}.hero{padding:30px 34px;background:#fff;border:1px solid #e2e2e2;border-radius:20px;margin-bottom:24px}.eyebrow{font-size:12px;color:#777;letter-spacing:1px}.hero h1{font-size:33px;line-height:1.4;margin:12px 0}.hero p{color:#686868;margin:8px 0 22px}.stats{display:flex;gap:14px;flex-wrap:wrap}.stats span{border:1px solid #e3e3e3;border-radius:10px;padding:8px 14px;font-size:12px;background:#fafafa}.stats b{font-size:20px;margin-right:7px;color:#242424}.label{display:inline-block;padding:4px 10px;border-radius:6px;background:#efefef;font-size:12px;color:#555}
section{scroll-margin-top:22px;padding:28px 32px;margin-bottom:22px;background:#fff;border:1px solid #e3e3e3;border-radius:18px;min-width:0}section>h1{font-size:25px;line-height:1.5;margin:0 0 22px}h2{font-size:20px;margin:34px 0 14px;padding-top:6px}h3{font-size:16px;margin-top:24px}p{margin:12px 0}a{color:#252525;text-decoration:underline;text-underline-offset:3px}a:hover{color:#666}blockquote{margin:18px 0;background:#f7f7f7;border-left:3px solid #aaa;padding:10px 17px;border-radius:0 8px 8px 0;color:#5c5c5c}blockquote p{margin:3px 0}
table{width:100%;border-collapse:separate;border-spacing:0;font-size:13px;table-layout:auto;margin:18px 0;border:1px solid #ddd;border-radius:10px;overflow:hidden}th,td{padding:11px 12px;text-align:left;vertical-align:top;border-right:1px solid #eee;border-bottom:1px solid #e8e8e8;overflow-wrap:anywhere}th{background:#f4f4f4;font-weight:600}tr:last-child td{border-bottom:0}td:last-child,th:last-child{border-right:0}tr:nth-child(even) td{background:#fcfcfc}
code{font:12px/1.7 Consolas,"Microsoft YaHei",monospace;background:#f2f2f2;padding:2px 5px;border-radius:4px;overflow-wrap:anywhere}pre{background:#f5f5f5;border:1px solid #e2e2e2;border-radius:10px;padding:16px 18px;overflow-x:auto;white-space:pre-wrap;word-break:break-word}pre code{background:none;padding:0}ul,ol{padding-left:24px}li{margin:7px 0}figure{margin:22px 0}svg{display:block;width:100%;height:auto}footer{padding:10px 4px;color:#777;font-size:12px}
@media(max-width:1050px){aside{position:relative;width:auto;border-right:0;border-bottom:1px solid #ddd;padding:18px 22px}nav{display:flex;flex-wrap:wrap}.sub,.aside-note{display:none}main{margin-left:0;padding:20px}section{padding:22px 18px}.hero{padding:23px}table{font-size:12px}th,td{padding:9px}}
@media print{aside{display:none}main{margin:0;padding:0;max-width:none}section,.hero{border:0;padding:0;break-before:page}section:first-of-type{break-before:auto}a{color:#222}body{background:white}table{font-size:10px}pre{white-space:pre-wrap}.hero{break-before:auto}}
'''
    sections=[]
    for fn,label in CHAPTERS:
        raw=(DOC/fn).read_text(encoding='utf-8')
        rendered=markdown.markdown(raw,extensions=['tables','fenced_code','sane_lists'])
        for f,_ in CHAPTERS:
            rendered=rendered.replace(f'href="{f}"',f'href="#doc-{Path(f).stem}"')
        for name in ['architecture','business']:
            svg=(DOC/'diagrams'/f'{name}.svg').read_text(encoding='utf-8')
            # Namespace IDs because both diagrams are embedded in one document.
            svg=svg.replace('id="title"',f'id="{name}-title"').replace('id="desc"',f'id="{name}-desc"')
            svg=svg.replace('id="arrow"',f'id="{name}-arrow"').replace('url(#arrow)',f'url(#{name}-arrow)')
            svg=svg.replace('aria-labelledby="title desc"',f'aria-labelledby="{name}-title {name}-desc"')
            rendered=re.sub(r'<img[^>]+src="diagrams/'+name+r'\.svg"[^>]*>',lambda m:'<figure>'+svg+'</figure>',rendered)
        sections.append(f'<section id="doc-{Path(fn).stem}">{rendered}</section>')
    nav=''.join(f'<a href="#doc-{Path(fn).stem}">{html.escape(label)}</a>' for fn,label in CHAPTERS)
    document=f'''<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Relink Studio · 业务重构开发文档</title><style>{css}</style></head><body>
<aside><div class="brand">Delta Market Assistant</div><div class="sub">业务重构 / DEVELOPMENT NOTES</div><nav>{nav}</nav><div class="aside-note">文档 v0.3 · 2026.10.06<br>PR01–PR10 已实现<br>下一阶段：PR11 UI 接入<br>Ctrl + F 搜索业务与参数</div></aside>
<main><header class="hero"><div class="eyebrow">业务规则独立重构 · 保留现有界面</div><h1>业务契约与实现进度。</h1><p>PR10 已完成 SQLite 持久化、事务与恢复测试，常规 UI 接入进入 PR11。下方保留原始业务证据和冻结规格；实际代码与测试结果以实施进度文档为准。实时捕获与 OCR 仍属后续阶段。</p><div class="stats"><span><b>44</b>业务 / 工程条目</span><span><b>88</b>冻结验收规格</span><span><b>16</b>已核查官方资料</span><span><b>12</b>证据 / 需求记录组</span></div></header>
{''.join(sections)}<footer>本文是开发规格，不是完成接入的声明。离线阅读无在线脚本和字体依赖；Markdown、JSON与图表源文件在同目录。</footer></main></body></html>'''
    (DOC/'index.html').write_text(document,encoding='utf-8')
    print(f'HTML_WRITTEN={(DOC/"index.html").stat().st_size} BYTES / CHAPTERS={len(CHAPTERS)}')

if __name__=='__main__':
    if '--html-only' in sys.argv:
        build_html()
    else:
        references();diagrams()
        print('REFERENCES=16 / DIAGRAM_SOURCES=2')
