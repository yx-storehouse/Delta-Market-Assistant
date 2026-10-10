# Relink Studio — new session entry

> 当前状态快照（交付路径、未解决问题、下一步、接手命令）先看根目录的 HANDOVER.md；本文件是按时间倒序的历史记录。

## 最新：接手购买模块，观察/倒计时/只读演练已发布，待协调实机（2026-10-09）

- 按用户提供的 `HANDOVER.md` 接续购买模块，不重跑收藏测速。新增 `--purchase-observation` 原生同帧文字投影，关注页动态卡片证据复用；Python `match_watchlist_selected` 复用原价格/成色/磨损函数，原收藏入口仍拒绝关注页。
- `purchase_observation.py` / `purchase_plan.py` / `run_purchase_rehearsal.py` / `run_purchase_probe.py`：严格倒计时、源帧/配置/代次绑定、窗口到期复核、结果观察。所有输出为只读；真实购买/确认输入和额度策略没有接入，F2 仍只收藏。已有 C++ ReplayReducer/IEventStore 继续作为后续订单事务层，不另造账本。
- BBZPS 只读取 `artifacts/bbzps_static` 原文本，223 段日志、30 类 OCR 字面量保存在新事务 `artifacts/purchase_readonly/bbzps_purchase_evidence.json`。没有运行原程序、没有恢复出完整购买函数；原“0.839ms/0.833~0.848”不擅自换算成新购买延迟。
- 发布 EXE `1ae0e1fefe233b506f40de7f9ff54acf2ef2d97659d91189efbbb3d392bf62e0`，25 个运行模块；47 CTest / 942 Python（购买 28）及离屏 UI/SQLite、依赖、副本回滚检查。旧 Esc 假 API 测试使用真实时钟偶发出现 16 ms 量化，现仅测试注入假钟/假 sleep，生产保持 30 ms 的实现未改。
- 已正常关闭旧主窗口用于发布，未替用户重开。配置和收藏记录不变。本轮没有游戏前台激活/采集/购买；已询问用户能否占用约一分钟读我的关注，**尚待本次回复**。用户同意后用发布入口 `run_purchase_probe.py --output artifacts/purchase_readonly/live01 --samples 3`，不要发 F2（它会执行收藏），不要借用昨天已用过的许可。
- `artifacts/purchase_readonly/rehearsal_input.json` 是合成契约样本，配当前真实配置演练通过不等于实机。真实确认弹窗和结果字段、延迟触发基准、动态公式、限购周期仍需补证。详细技术文档在 `docs/PURCHASE_READONLY.md`。

---

## 最新：皮肤目录去掉菜单颜色，改记游戏真实品阶（2026-10-09 中午，已重建 EXE）

- 用户要求“把颜色等级去掉改成真实识别需要的等级”。内置目录 `src/assets/catalog/skins.json` 升为 `relink-skin-catalog-v2`，每款皮肤记 `grade`（传说品阶 28 / 史诗品阶 33 / 稀有品阶 88，按核对表原菜单颜色换算：橙/红＝传说、紫＝史诗、蓝＝稀有）；普通品阶不录入。旧 v1 文件（menu_color）仍可读并自动换算，手填的旧品质名是品阶名才采用，否则按颜色；首次改写旧扩展文件前留 `.before-grade-v2.bak`。
- 程序：关注列表“全部品阶”筛选与“品阶”信息、皮肤资料表“品阶”列和追加表单品阶下拉（未记录/传说/史诗/稀有）、导入预览“品阶 / 标记”。domain Skin 去掉 menuColor，目录皮肤的品阶放 rarity（未记录＝待核对）；旧配置里的 menuColor 键读入时丢弃。
- 收藏运行时：直接按目录品阶勾选；读发布目录里与 EXE 同批的 `dist/RelinkStudio/catalog/skins.json`；旧程序保存的配置（待核对＋颜色）也认。显示名称两端统一按“赛季|武器 - 系列 - 版本”生成，追加/导入不再因缺前缀被拒。
- 4 代理复核采纳问题已修；CTest 46/46，收藏 Python 910，发布件测试、verify_delivery、verify_standby 通过。EXE `20d3ed757e53c7dbf46277901753ea8261c04515143b6f124199937f5a81d89e`。用户需自己重新打开程序。

---

## 最新：筛选勾已拥有+未拥有+皮肤品阶，返回改用 Esc（2026-10-09 上午，已实机跑通）

- 用户要求两件事已做并实测：典藏筛选勾“已拥有”“未拥有”和皮肤品阶（任务品阶优先，否则按目录颜色：橙/红=传说、紫=史诗、蓝=稀有（用户确认），普通不录入；找不到或目录为空时改全部品阶再找）；列表/关注页/筛选框的返回改按 Esc（保持 30 ms，鼠标在动就停）。
- 第一次实测停在勾选回读：鼠标停在刚勾的框上，悬停高亮读不出；已改为先移开鼠标再读，读不出只重读。第二次 `20261009-095850` 第 14–21 条全部完成，Esc 6 次全部成功。
- 4 代理复核（3 审查 + 1 反驳）采纳的问题都已修。收藏 Python 903 项、发布件测试通过；只改 Python 运行时，EXE 未重建。

---

## 最新：代理前台实测 21 条全部跑通；修了滚动条 1 像素抖动导致的停（2026-10-09 上午）

- 停因：AS Val 翻页时滚动条中心线测在 x 1879，已测量档位都在 1878，全部被拒（`COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE`）。现允许 ±1 像素中心线抖动，纵向条件不变。修后 `20261009-092047` 完成剩余全部任务；用户随后自测反馈“没有卡住”。
- 待用户确认：部分已确认收藏后来又变白星（今天仍在列表的 11 把里 2 把；昨晚 10/10），程序再次经过时会重新收藏。疑似关注上限或游戏端未生效，需看“我的关注”。
- 只改了 Python 收藏运行时（发布目录 collection/ 与 file_manifest.json 已更新），EXE 未重建，仍是 `bcf20b59…`；收藏 Python 878 项、发布件测试 48 项通过。

---

## 最新：开发日志已上线；第五次实测问题已修，独立复核后又加固 6 处（2026-10-09 夜间，用户睡觉时完成）

- 用户要日志：`artifacts/logs/YYYY-MM-DD.log`（收藏进程逐步/逐事件/异常堆栈）、`.gui.log`（主程序），每次非 F2 停止在运行目录写 `failure/`（摘要、完整失败步、只读失败截图、裁剪图）。排错先跑 `python dist/RelinkStudio/collection/collection_devlog.py --latest`。程序“日志”页有“打开日志文件夹”。
- 第五次实测（20261008-224416，成色空读）已修；4 个审查代理复核：点击前光标再核对、列表起始/刷新读取也做字段空读重读、复查独立预算、OCR 池坏会话退役+调用方重读、助手 4000 次优雅重启、备用截图服务死亡替换、步数上限在卡片边界结束。未做：自动核对未确认收藏；游戏外 F2 跨段切窗。
- 已发布 EXE `bcf20b592e2f66547527d601c6a7206f8a66f469f64a17c890c1f616cfdf39a0`；CTest 46/46，收藏 Python 871，发布件 verify_delivery / verify_standby（0.91 s 就绪）通过。未实机。下次 F2 从第 13 条（行 12 S11 AUG）续跑。

---

## 历史：第四次实测停在 E_OCR_SESSION_POISONED，根因是待命时 OCR 助手空闲退出（2026-10-08）

- `hotkey_runs/20261008-223526` 续跑行 12 收藏 2 把后停（点击前）。PowerShell OCR 助手空闲 30 s 自退，无事件循环的 QProcess 状态未刷新，请求写进已退出进程 → 会话永久 poisoned。修复在 `WindowsOcrSession::run`：请求前 `waitForFinished(0)` 刷新、正常退出则重启、接近上限优雅退役；助手空闲上限 10 分钟（ps1 上限放宽到 3600000）。`windows_ocr_tests` 新增两项真实助手测试，CTest 46/46，Python 843。已发布 EXE `65421b9a5a123d39012bc8a2dd4d80338225b6abfa2d6ed88767ac76adf13449`，发布件检查通过。以后打包需要时由代理自行正常关闭 RelinkStudio（用户已授权），但不自动重开。

---

## 历史：F2 第三次实测跑通 12 条、新收藏 53 把；复查误报已修（2026-10-08）

- `artifacts/hotkey_runs/20261008-222601`：S6 12 条到价格边界（79 秒），S11 AUG 收藏 53 把后停在 `COLLECTION_RECEIPT_LAYOUT_CHANGED`。实为点星后几何复查帧里价格置信度 < 0.99 被当成整帧失败；列表没动。已改为该复查只看几何（`_recheck_layout`），并用原始两帧做回归测试。收藏 Python 843 项、CTest 46/46，已发布（EXE 未变 `cc5c0bad…`，只更新 collection Python）。用户需重开 RelinkStudio 生效；下次 F2 从第 13 条（行 12）续跑。

---

## 历史：F2 首测卡在“加载识别模型”已查明并修复，改为程序打开即预加载（2026-10-08）

- 用户首测（`artifacts/hotkey_runs/20261008-213818`、`-213913`）两次都卡在“正在加载识别模型…”，再按 F2 才继续并立即停止，0 步 0 点击，没碰游戏。根因：收藏进程里等 `stop` 的线程阻塞读标准输入，Windows 下会让 `import numpy`（OpenBLAS 的 C 运行时初始化）一直等到下一行输入。已改为 PeekNamedPipe 轮询，并有真实子进程回归测试。
- 按用户要求改为 RelinkStudio 一打开就后台预加载（`--standby`）：识别引擎 + 截图识别服务预先就绪（真实检查 1.2 秒），F2 立即开始；加载中按 F2 会自动排队开始，再按取消；跑完引擎常驻，下一个截图服务后台预备。找游戏窗口改用 EnumWindows，不再起 PowerShell。
- 第二次实测在第一张截图就停（`hotkey_runs/20261008-2216*`，0 点击）：原生检查要求返回窗口是 Mirasim 且不同于游戏，快捷键运行必然被拒（`E_DIAGNOSTIC_ARGUMENTS`）。已改为 caller-owned 时返回窗口可选，快捷键不发送；停止原因附带原生错误码。已发布 EXE `cc5c0bad6fcdca7d129efebb7aba9ada75e129cd20bb1ffa019cd7899a04108f`，发布件离线检查通过。
- 上一版 EXE `26110c2a8850bf4f7f4b75228242b3e60b7780470a4948a3fac1fe43a2c6e832`。CTest 46/46，收藏 Python 840 项；发布件 `verify_delivery.json`、`verify_standby_dist.json`（0.94 秒就绪）通过。设计与根因记录在 `docs/COLLECTION_HOTKEY.md`。

---

## 历史：F2 快捷键启动收藏 + 顶部状态栏已发布，等用户自己实测（2026-10-08）

- 用户要求：做顶部悬浮小日志（左侧详情 OSD 先不做），并把运行快捷键接到现有收藏业务，用户自己按键测试。已发布到 `dist/RelinkStudio/RelinkStudio.exe`（`a4bc692a913c8332c80b83fca75f37c9546251335fb8d2afea128e543eacfd58`）。
- 用法：打开 RelinkStudio，在游戏里按 F2 开始，再按 F2 在当前步骤完成后停止；关程序也会停。跑的是任务页**所有启用任务**（当前 21 条：S6 12、S11 3、S5 6），开始前先保存界面配置；同一份任务没跑完则从第一条未完成任务续跑。只收藏不购买，在游戏里按的就留在游戏里（不切 Mirasim、不挪鼠标）。
- 状态栏：`RelinkStudio.exe --status-overlay`，置顶、点击穿透、不抢焦点，只在顶部 4% 带内；截帧遮挡检查只放行它，整屏 OCR 丢弃它范围内的文字。显示“[k/N] 正在执行收藏任务：筛选皮肤[…] → 筛选成色[…] · 价格 · 磨损”、每把符合/成功/跳过原因、翻页、本条完成、停止原因。
- 新增权限检查：游戏完整性级别高于收藏进程时（游戏管理员权限、RelinkStudio 普通权限），在任何输入前停下并提示管理员运行。代理 shell 是 High，此前实测都在 High 下跑，普通双击启动（Medium）这条路径没实机验证过。
- 每次运行记录在 `artifacts/hotkey_runs/<时间>/`（task_snapshot、events.jsonl、summary.json、segment_NN/session.json、result.json）。CTest 46/46，收藏 Python 831 项，离线验收 `artifacts/status_overlay/verify_delivery.json`。设计见 `docs/COLLECTION_HOTKEY.md`。
- 没有做本轮实机。用户测试后如有停止，先看对应 run 目录的 result.json / events.jsonl 末尾。

---

## 历史：收藏流水线提速已实测，S11 三条全部跑完，单把 1000 ms → 约 480 ms（2026-10-08）

- 用户同意全部 6 项，并明确“有未决就禁止任何输入”不是他的规则。SESSION_START/docs 里不少“不能/禁止”是先前代理自加的工程约束。用户真正的约束是：价格/磨损/成色不放宽，收藏后成功回读，不购买，保留原关注，占前台前先协调。
- 用户同意后测了 S11（`artifacts/collection_pipeline_speed/run01–run06`），共新收藏 66 把，全部像素金星回执。run02–run06 收藏周期中位 469 ms（n=49），跳过金星 375 ms，含翻页周期 1188 ms；旧版 run06 为 1000 ms。行 12 AUG 已到价格边界 320。用户随后要求跑完其余两行：run07 一次通过（AS Val 到 308、P90 到 310，48 把新收藏，10 次翻页，59 s，无异常），S11 三条已全部完成；不要自动重跑。
- 六段停止原因都已修复并补回归测试：半截卡边缘抖动、滑块变短、滚动条 ±1 px、翻页稳定判定改按时间（1.6 s）且只重读不重滚、尾行间距成对包络加 3 px 容差、点星前被打断时自动释放未发送的预留。最后一段是外部移动光标导致停止；那把 AS Val 的预留没有发出点击，已移到 `artifacts/m2_savedvalue_collection/released_unsent/`。journal 现在 567 条，无未决。
- 发布 EXE `7e331c5d1f185e5814a7895edd21bd9a0fe471ce49eb1eb9c266447cc28f3e9d`，CTest 44/44，收藏 Python 757 项。实机入口 `artifacts/collection_pipeline_speed/run_s11.py --name runNN`，时序拆解用 `cycle_breakdown.py`。结束时切回的是 Mirasim 窗口（程序只认 Mirasim 作为返回窗口）。
- 下一步可做：导航点击每次固定等 0.6 s（每行约 10 s）。新批次仍要先和用户协调前台。详情见 `docs/COLLECTION_PIPELINE_SPEED.md`。

---

## 历史：S11 三条规则已补测完成，21 次滚动通过，速度仍约 1 秒/把（2026-10-08）

- 最新事务 `artifacts/collection_tail_latency` 已执行 run01–run06。run04 完成 AUG 行 12、42 新收藏、10 次滚动；run06 完成 AS Val / P90 行 14 / 16、51 新收藏、11 次滚动，97.172 秒，`passed`。三条原价格上限 300 均在读到 310 后停止。不要再把这三条当成本轮未完成，也不要自动重跑。
- 修复了原 11 / 15 px 双列间距导致的尾行误拒绝，新共享实测间距分支已在 run04 真正触发；不补旧坐标、不放宽价格磨损成色阈值。单纯移动卡片落点没有解决漏选，后续改为选卡单次按下/抬起分开发送（请求保持 24 ms）；run04 / run06 共 96 次选卡经新帧确认，没有再因漏选停止。点星仍无重复发送，原回执保留。
- 最新连续点星 34 段中位 **1000 ms**，890–1234 ms；选卡→星 609 ms，星→回执 266 ms。每秒多把 / 两倍目标未达到。标题单行识别与 journal 决策字段投影只证明各自热路径减负，不夸大端到端收益。
- run04 在已确认收藏后补全布局时遇到价格 `300 / 0.88659`，后续独立帧是 `300 / 0.99998`。已补限次新帧重读，0.99 阈值未变；run06 该分支未触发，只有真实数值回归验证，别说它在实机成功重试过。
- 当前发布 EXE `3941939104975062625d7e038f7735e71188a225de895ed944c3c1a7960b7243`；session Python `7c736a34d8bf4f1466eb7e024dcce7039926d6e15427da7e775de203ffef8e4c`。726 收藏 Python、44 CTest、离屏 UI/SQLite/依赖与隔离回滚通过。普通 UI“运行”仍未在本轮接入真实 CLI。
- 本事务累计新收藏 104，journal349→453，原记录和正式配置不变；旧21/21完成来源及业务计数保留。run05 接管时光标变化，0次业务动作，原失败保留；run06 结束已恢复 IDE，点星/几何未决均0，鼠标位置恢复另记 `CURSOR_INTERFERENCE`，不要写成鼠标也已恢复。
- 新前台批次重新协调，不能沿用本轮已消费的“开始”。详细证据：`docs/COLLECTION_TAIL_LATENCY.md`、本事务 `live_result.json` / `VERIFICATION.txt`。用户不要例行长报告；已解压程序路径仍为 `dist/RelinkStudio/RelinkStudio.exe`。

---

## 最新：六段S11实测已结束，速度未达标，卡在半截行边界（2026-10-08）

- 用户授权的run03/04/05/06已经执行，别再当作等许可或直接重跑。run03/04启动识别失败0收藏；run05新增6；run06新增18、保留5黄星、5次滚动，47.469秒后停止。前台各段整批进出各1次，均已恢复IDE。run06点星和几何未决均0。
- 当前仍约1秒一把：run06连续点星10段984–1140ms，中位1031.5ms；选卡→星656ms；星→回执297ms。目标未达，不声称两倍或每秒多把。主耗时是读取与完整回执，不是只需减鼠标轨迹。
- 当前真实阻塞：完整回执成功后整页布局连续3帧报 `E_COLLECTION_ROW_EDGES`。同帧两列行间距11/15px，尾行两列间距15px，左列超过原3px一致性限制。详见run06/blocked_layout_review.json。选中边框是待视觉核实的解释，原像素没保存，不能直接放宽阈值或用旧坐标继续。
- 已发布：EXE `fbc63018a389f177439e9ef34c56c9e7b3a5bf1448f63290dfae296a5d309a06`；当前17个Python模块hash与run06实际执行一致，697收藏Python、43CTest、离屏UI/SQLite/依赖及独立副本回滚通过。最新修正含同帧空关注页精读、S11子集进度、覆盖改绑实际候选且保留旧下沿约束；原金额/磨损/成色/白星及回执函数保持。
- 本事务共新增46，journal303→349，原303哈希和正式配置保持；旧21/21完成来源不变。当前三条S11规则本轮尚未完成，不能拿旧完成记录给这次测速背书。普通UI“运行”仍未在本轮接入CLI。
- 下一轮需先协调前台，计划纯内存视觉复核尾部测量；原生最新帧通道/完整候选回执继续保留。新段用run07，不能覆盖此前六段。速度结构下一项是局部回执和识别流水衔接，而非再减固定点击等待（已为0）。
- 用户不要例行输出；详细记录留 `docs/COLLECTION_READY_STREAM.md`、`artifacts/collection_ready_stream/live_result.json`、`VERIFICATION.txt`。交付路径保持 `dist/RelinkStudio/RelinkStudio.exe`，不自动启动可见UI。

---

## 历史：S11 流式读取两段后的交接（已被下列六段结果取代）

- 实机 `collection_ready_stream/run01` 新增6条，翻页后残余约6px位移触发原几何拒绝；加入滚动稳定窗口后，`run02` 新增16条、保留1黄星、完成3次翻页，38.063秒，随后右侧目标仍读到左侧旧选中而停。两段都已恢复IDE，未决点星0；各自未完成选卡几何留在session中。不要把原failed结果改成passed。
- 当前又加了内部 `collection-selection-point`：只有本次点击目标包含在新选中区域内，原生就绪观察才结束。它是负提示，原候选/价格/磨损/星标/完整几何关联全部保持。原生43 CTest、688收藏Python、44流式/19滚动稳定/90服务协议断言及离屏/副本回滚通过。当前EXE `06f69e03dcc5786d23d6f1755be644f64068609455dde3268638c9760284f62b`，session Python `2a81efb4fb813cc87631f218e7c0f730a64742241a7a46d555ed7dad4146412f`，backend Python `0f0765d9631bec56ac3cc04b5b9a9f3386242e9c578b87d536742ca9c84715f6`。
- **已再次询问用户方便占前台测S11否，正在等回复，别自动启动run03。** 最新目标就绪补丁尚未实机。获回复后入口 `artifacts/collection_ready_stream/run_s11.py --name run03 --rows 12 14 16`，只用真实S11已启用条件，不回天命，不放宽阈值。整段只进出前台各一次；旧22收藏保持。
- 当前仍约1秒一把。run02纯连续点星11段938–1188ms，中位1063ms；选卡→星609ms，星→回执312ms。只是采集并行还没达到每秒多把，完整回执尚未缩成局部确认。后续优化必须继续实测，不把一次Python请求当成端到端提速证明。
- 冻结业务函数逐条回放22次真实回执通过；原303条hash保持，现325（319 confirmed+6 reconciliation），正式配置不变。checkpoint保留旧21/21完成来源和业务总计，最新试段及失败另附。原生run01/run02实现分别保存在事务目录，不覆盖。详细证据留 `docs/COLLECTION_READY_STREAM.md` / `live_result.json`，用户不要例行报告。

---

## 历史：实时最新帧通道首版已发布、实机前（2026-10-08）

- 用户要求开始优化读取。已实现会话级 DXGI 生产线程 + 单个最新帧待消费槽；采集与原生 OCR 并行，仍要求源帧在当前请求之后，原真实时间戳/窗口/DPI/前台/lease 校验保持。没有改白星判断或删除成功回执。选卡就绪改在原生服务内最多16帧/900ms连续观察，没有两次35ms固定等待；原检测器与最终候选规则保持。
- 已构建发布：42 CTest、686收藏Python、44新原生断言、5新协议测试、离屏UI/SQLite/依赖和副本回滚通过。EXE `d268553803f439afe1ae6c9c413599ce1115f46c1ea7f29384205226ee419266`，发布后端 `run_collection_observed.py` 为 `0f0765d9631bec56ac3cc04b5b9a9f3386242e9c578b87d536742ca9c84715f6`。新事务 `artifacts/collection_ready_stream`。
- **尚未实机，不声称每秒多把。** 已向用户问“接下来需要用游戏前台测 S11，约1–3分钟，现在方便吗”；等新回复后再开始，别沿用以前已经结束的许可。当前无新游戏输入，303条journal/配置/旧21条完整循环保持。用户不看例行汇报，证据留文件。
- 新实机入口：`artifacts/collection_ready_stream/run_s11.py --name run01 --rows 12 14 16`，默认也是当前启用S11；不可回天命、不可覆盖旧run。使用发布模块，先核正式配置哈希；整段只进出前台各一次。`--no-ready-stream`保留本版本串行对照。准备好的命令未执行。
- **后续仍待优化/测量：点星后的完整回执目前未缩成局部星色确认。** 先在S11测新采集/就绪段的收益及资源竞争，不能把本轮写成整套回执已完成。源码和注意事项在 `docs/COLLECTION_READY_STREAM.md`。普通UI“运行”仍未接真实CLI。

---

## 最新：收藏避让版 run01 已实测完成；下次用 S11（2026-10-08）

- 用户“可以开始吧”授权的批次已经完成：`artifacts/collection_scrollbar_continuity/run01/result.json` passed，0–7 共 8 规则/25 候选/10 新增/7 黄星保留/8 超价停止，61.141 秒。前置/恢复各一次，IDE restored=true，pending0。不要继续等这次开始，也不要重复该批次。结束后鼠标位置恢复记录 `CURSOR_INTERFERENCE`，但 IDE 已恢复；保留这一字段，不伪称全部恢复动作成功。
- 实际 27 次 continuous 移动中 25 次绕行，2 次保持本已避开区域的原曲线，总预算未加长。10 次回执全部完整布局、一次采集，receipt_only 为 0；原 run07 是 7/15。星→下一卡全部 328–532 ms，中位344；原版343–1110 ms、中位360。长尾明显降低，选卡→星中位仍640 ms；不同实时列表不是受控AB，不声称整体两倍。
- 新增10条冻结matcher/geometry/receipt逐条回放通过，原293条hash不变，现303条（297 confirmed+6 reconciliation）；配置不变。全局checkpoint保留旧21/21完成来源与业务总计，当前run01只是优化试段。未购买、未运行原BBZPS、未保存游戏图像。本段没有滚动，不据此宣称多页通过。
- **用户最新指定：下一次测 S11 系列，不再默认测数量少的棱镜攻势 S2 天命。** 当前已启用 S11 黑银先锋 AUG / AS Val / P90 为快照行12/14/16，条件沿用真实配置，不为测速放宽。`artifacts/collection_scrollbar_continuity/next_live_selection.json` 保存未执行的下一批选择。新批次前台先协调；本次结束后不自动开启第二批。
- 原生包仍 `c733a612eb1c7f15570cf291e14abc82361cf865c107e5a0b33fcdc4f1dc90ac`，发布session Python `b29df9957c7dac73f24fbb719af14fa7156ef60669e34ab0512d97b59087361b`；41 CTest、681收藏Python和离屏/回滚证据有效，正式包保持修改态。详情在 `docs/COLLECTION_HOVER_ROUTE.md` 和本事务 `live_result.json`；普通UI“运行”仍未接真实CLI。

---

## 历史：收藏悬浮提示避让已离线发布，等待实机前（2026-10-08）

- 用户要求继续优化。后台定位run07的7次慢分支：滑块仍可见，但中部滑轨被57–103px左右的覆盖区切断；5帧OCR明确读到成色说明。7次原点星轨迹都穿过右详情区；不能把原因说成滑块亮度不足，也不要放宽检测或填补被挡像素。
- 已新增收藏专用 pointer-only 避让路线：当前2560×1440/continuous时，绕开右详情区，保持原真实点击终点、原30–60ms总预算，不分段加等待。原曲线无交集则保持；起点在详情区则保留原受检查移动并如实记录。前台/干扰/帧龄/商品价格磨损/星标回读仍在；导航滚动不变。
- 已实际构建发布：41 CTest、681收藏Python、35cursor/13foreground/7packaging和离屏UI/SQLite/隔离副本回滚通过；新模块17文件闭包已部署。native仍 `c733a612eb1c7f15570cf291e14abc82361cf865c107e5a0b33fcdc4f1dc90ac`，发布主session Python为 `b29df9957c7dac73f24fbb719af14fa7156ef60669e34ab0512d97b59087361b`。
- **新避让版尚未实机，游戏前台先等用户再次明确开始。** 本轮0游戏输入/0前台激活；journal293与配置不变；原21/21和run07实际结果保留。不要把旧7路径的离线避让成功说成真实消除了提示或达到2倍速度。
- 新事务 `artifacts/collection_scrollbar_continuity`，文档 `docs/COLLECTION_HOVER_ROUTE.md`；旧local_hotpath四件保留。下一次用户让出前台后，用新事务run_live.py开run01，不覆盖旧段、不要使用inspect-start。新的进程枚举已改为绝对PowerShell路径，不依赖host PATH。普通UI运行仍未接实机CLI。

---

## 最新：用户已让出前台，run07 真实收藏完成，IDE 已恢复（2026-10-08）

- 用户回复“ok开始吧”后已执行，不再等待这次开始。`artifacts/collection_local_hotpath/run07/result.json` passed：0–7 共8规则/23候选/15新增/8超价停止，66.734秒；前置/恢复各一次，IDE restored=true，pending0。下次新前台批次仍按用户要求先协调，不自动重复本段。
- 当前包仍为 `c733a612eb1c7f15570cf291e14abc82361cf865c107e5a0b33fcdc4f1dc90ac`。四ROI 29/30实际成功，1次同帧full fallback；同帧详情投影29次、continuous轨迹30次。条件边线复读本次0触发，不声称实机通过该分支。
- 选卡→星 / 星→回执 / 星→下一卡中位625 / 282 / 360 ms，较run05为735 / 344 / 484ms；不是受控AB，整体两倍目标未达。7次 receipt_only 来自 `E_COLLECTION_SCROLLBAR_THUMB`，随后额外完整布局采集，使这7次星→下一件875–1110ms，其余343–360ms。下步分析这些实测profile，不能把老坐标或局部回执升级成可继续点击的完整布局。
- run06因wrapper包测试PATH缺PowerShell、窗口枚举前失败，0输入0前台切换；其false恢复状态保留。run07继承真实宿主PATH成功。没有购买、没有原BBZPS执行、没有游戏图像落盘。
- 原278条账哈希保持，新增15条冻结matcher/geometry/receipt回放通过，总293（287 confirmed+6 reconciliation）。checkpoint保留旧21/21完成来源，不以本8条替换。详见 `docs/COLLECTION_LOCAL_HOTPATH.md`、`run07/offline_receipt_audit.json`、`run07_continuous_speed_analysis.json`。

---

## 最新：连续收藏提速版离线交付，用户要求先等前台（2026-10-08）

- **现在不要切游戏前台。** 用户最新说需要前台时先暂停，他正在用窗口。后台开发、构建和离线复核已做；下一次实机须先等他明确让出前台，不能沿用历史“开始”。
- 新版 `dist/RelinkStudio/RelinkStudio.exe` SHA256 `c733a612eb1c7f15570cf291e14abc82361cf865c107e5a0b33fcdc4f1dc90ac`。已接校准后当前四 ROI 页面识别、同帧 controls/detail 去重复、条件左边线同帧像素复读、IDE 返回窗口核验，以及收藏专用 30–60 ms 曲线。导航/滚动时序不变。不要声称 UI“运行”已接真实 CLI。
- 已通过 41 CTest、664 收藏 Python、13 foreground、35 cursor、7 packaging、离屏 UI/SQLite 和隔离回滚；最新代码尚未游戏实测。轨迹计划减半不是完整收藏速度翻倍。
- 最新实际试段仍是 `artifacts/collection_local_hotpath/run05`：3 条规则完成、14 候选/11 新收藏，row3 的 `|成色C` 失败前停止；IDE restored=true、pending 0；旧267条保持，现278条。run04 自动恢复失败照旧保留，随后独立恢复不覆盖原失败。当前 checkpoint 保留原21/21完整来源，另附run05试段。
- run05 的选卡→星 / 星→回执 / 星→下一卡中位为 735 / 344 / 484 ms。精确页面 guard 22/22 miss、标题缓存31/31 miss，不把它们说成已验证提速。此次原选中检查静态分析仍证据不足，不猜BBZPS没有检查。
- 继续入口与证据：`docs/COLLECTION_LOCAL_HOTPATH.md`、`artifacts/collection_local_hotpath/live_result.json`。用户让出前台后再跑新 `run06 --business-only`；不使用旧 `--inspect-start`，不执行原BBZPS、不买、不清关注、图像不落盘、整段仅进出前台各一次。

---

## 最新：按用户要求去掉选卡/点星后的固定等待，实机试段已结束（2026-10-08）

- 用户要求先调点击延迟看效果。本轮仅将自研 `fast_settle` 的选卡后 120 ms、点星后 80 ms 固定等待改为 0，立即读取新帧；轨迹仍为 60–120 ms 曲线，滚动 180 ms、普通导航 600 ms 保持原值。新帧/候选/价格磨损/星标回执/未决记录保护没有删除。
- `BBZPS/config.ini` 的相关字段仅按原值摘录在 `artifacts/collection_click_wait/bbzps_click_settings.json`；原收藏调用点及这些字段单位尚未恢复，没有声称把 `clicktime` 当作收藏延迟，更没有运行原程序。
- `artifacts/collection_click_wait/run01` 实际完成 AUG/P90 S、A、B、C 共 8 条当前规则、23 个候选；新增 9 次收藏、保留 6 个黄星、8 个超价停止边界；整段 60.828 秒。前置/恢复各一次，IDE 已恢复，pending 为零，无购买、无游戏图像落盘。
- 独立审计确认所有实际选卡/点星动作的固定后等待为 0，曲线移动保留；9 次新帧回执与冻结候选/几何/星标规则回放通过。旧 258 条 journal 哈希不变，新总账 267（261 confirmed + 6 confirmed_reconciliation）；历史计数不等于当前关注总数。
- 本次选卡 API 返回→点星 API 开始中位仍为 797 ms；点星 API 返回→回执中位为 344 ms，上次是 766 ms。两轮候选和画面不同，不是同物品受控 A/B，也不据此保证百分比加速。用户指出的选卡后等待主体尚未解决，后续应继续对齐原程序的局部价格→详情读取业务路径。
- 固定解压目录保持 `dist/RelinkStudio`。原生 EXE SHA256 仍为 `8f5ba63a4eab429f5ba9761b93a5998b6c3a4441a7e20eacda4bd76849ce41cf`，变化是发布目录中的 Python 执行器；未声称普通 UI“运行”已新增接通。
- 全局 checkpoint 保留原 `white_star_run_08` 的 21/21 完成来源和总计；本段单独作为后续试段，不把 8 条写成新的完整循环。下次必须新帧识别，不能重用本段坐标。四件与离线记录见 `artifacts/collection_click_wait/VERIFICATION.txt`，实机独立审计见 `run01/independent_audit.json`。

---


## 最新状态：被打断的 action-speed 事务已核账，下一步仍按用户要求参考原流程（2026-10-08）

- `artifacts/collection_action_speed/run01` 是此前已结束的自研优化试段，不是 BBZPS 流程复刻完成，更没有 BBZPS 同机速度对照。审计没有重放收藏、运行原样本或改产品代码。
- 实际执行当前配置的 AUG / P90 S、A、B、C 共 8 条规则，21 个候选；新增 11 次收藏、保留 2 个已有黄星、8 个价格停止边界；总时长 63.454 秒。回执为 8 条 toast+金星、3 条同物品白星→金星。冻结的候选/几何/回执函数逐条回放通过。
- 历史 journal 从 247 条增加到 258 条（252 confirmed + 6 confirmed_reconciliation）；原 247 条字节哈希不变，11 条新增记录都保留。历史次数不是当前关注总数。正式配置 SHA256 仍为 `7c317fc1850e2ca85eeba394f73f9c231499f7a781dad570f95f0e4287b8f5b9`。pending 为零，整段前置/恢复各一次，结束已回 IDE；未购买、未保存游戏图像。
- 全局 checkpoint 继续保留之前 `white_star_run_08` 的 21/21 已完成来源和业务总计；上述 8 条只是后续测试。最新可见页面记录来自 run01，不能作为当前画面或可直接操作的坐标，下次必须新帧识别。
- 事务包 `8f5ba63a4eab429f5ba9761b93a5998b6c3a4441a7e20eacda4bd76849ce41cf` 于 12:38 完成独立副本回滚和四件；591 项收藏 Python、35 项轨迹、13 项 foreground、7 项打包测试及离屏 UI / SQLite 通过。四件证明对应这一版本；后续新修改另建事务，不覆盖此证据。
- 剩余方向是用户已纠正的 BBZPS 主业务调用顺序核实与对齐，而非继续用哈希或轨迹测速冒充原程序流程。普通 UI“运行”接实机没有在本轮得到新验证。详细证据：`artifacts/collection_action_speed/run01/independent_audit.json`、`VERIFICATION.txt`、`live_result.json`。

---

## ??????????????????????????2026-10-08?

- ?? `docs/COLLECTION_HOTPATH_SPEED.md` ? `artifacts/collection_hotpath_speed/live_result.json`????????? BBZPS ????????? EXE/DLL?
- ?? `probe06` ??? EXE `272bf1491d5ae94f419513cbf75014496250d32fa6aaa08fff43c6016750602c`????? AUG ?? S/A/B/C ??????10 ????? 5????? 1?4 ?????????? 38.391 ?????????????? 21/21 ??????????????
- ???????? 8?probe02 2?probe05 1?probe06 5??? 239 ? journal ?????? 247 ????????????????????? IDE ????probe04 ?????????????????????????????????
- ????GPU ??/duplication/staging ?????????????????/DPI/???/?????????????? duplication??????????????? SHA256 ?????????????????????????????????? OCR?????????????????????????lease ? journal ???
- probe06 ? 6 ??????????? 141 ms?? 0 ? OCR??????????????????????????5 ???? 3 ??????2 ????????? API ?????? 531/937/516/532/969 ms??? 3 ??? API ?????? 1.2 ???????? BBZPS?
- probe05 ?? native ????????? 192?37.5 ms????????? 632.5?429.5 ms?-32.09%???????? EXE ???????? EXE/??/BBZPS/??????????????????????
- ???????????? `artifacts/collection_hotpath_speed`????????? `VERIFICATION.txt` ? `reopened_artifacts.json` ????????? `dist/RelinkStudio/RelinkStudio.exe`??? UI????????? CLI???????????
- ???????????????????? ROI ?????????????? full-client OCR ?????????????????????????????? BBZPS DLL??? probe01/03/04/05 ?????????????

---


## 最新：BBZPS 收藏短路径已深入核实，不再用单帧 25% 替代收藏响应分析（2026-10-08）

- 用户强调原程序“马上就点收藏”。本轮只做静态与历史记录分析，没有启动 BBZPS，没有操作游戏，没有改动产品源代码/发布包。新增报告 `docs/2026-10-08_BBZPS-收藏热路径分析.md`，证据目录 `artifacts/bbzps_collection_hotpath`。
- 全量原日志 18,079 条双 True 匹配中，价格匹配→联合匹配间隔中位 **96 ms**，每段仅一条已记录详情 OCR，无夹入的原点大范围 OCR。**不是鼠标点击延迟，也不是纯 OCR 推理耗时**。详情输出→判断通常为同一毫秒。95,961 个原行定位与全日志哈希复验通过。
- 新取得原 OCR DLL 实际代码：`ocrScreen @ 0x180058C90 → 0x180051FC0` 直接对请求矩形 BitBlt、取内存像素并调用持有的推理对象；`initTomato/startLock/unlock` 为门控初始化及实例池。原 DLL 仍逐次创建 GDI DC/位图，不能宣称完全复用抓屏资源。原主程序选择 Screen/Window/数字模型/fast 模式和鼠标参数仍未直接还原；不猜测。
- **最重要的新发现：最近真实新增收藏 run06..08 的 26 次回执全部采两次，首帧均缺 default_sort 锚点并报 `E_PAGE_INSUFFICIENT_ANCHORS`。** 被丢弃首读中位 531 ms，完整回执步骤中位 1,640.5 ms。这是去预览前的实机数据；新版 ab05 没点星，不能把这些数写成新版实测，但全屏分类/串行回执结构仍未优化。
- 后续重点应是：稳定页面上下文下按步骤小 ROI 识别、价格先行、点星后的局部回执短轮询、抓屏资源复用。保留匹配、真实回执、日志、帧龄和轨迹，不通过删校验或不等回执冒充提速。准确下一轮计时需区分“选卡后到点星”“点星后到回执”“整项到下一项”，不再只测固定画面单帧。
- 原保护主体中鼠标调用尚无直接函数证据；原日志下一次读取先于成功提示也只证明已记录的顺序，不证明没有任何未记录的回执。当前发布 SHA 仍为 `623f380f3a293b87fafe145e012a4f640a1540a88fba45d7a299fa219d503a1c`，上轮 21/21 完成记录不变。

---

## 最新：同屏采集识别 A/B/C 已完成，中位耗时降低 25.21%（2026-10-08）

- 用户反馈仍比 BBZPS 慢。本轮用 `white_star_run_06..08` 的常驻 OCR 真实计时定位：run06 采集识别占 69.058%，鼠标轨迹占 12.991%；不要再引用旧版“每 ROI 冷启动”。仅阅读 BBZPS 原日志，1,192 条定位复核通过，没有运行原程序。
- 已修改并发布：正常收藏默认关闭整屏 PNG/Base64 预览；固定本地文字模型时不再先跑一遍会被替换的 native 标题/目录 OCR。必要小图仍在内存；业务匹配、价格停止、白星策略、轨迹、回执和日志未删减。实际新版 EXE SHA256 为 `623f380f3a293b87fafe145e012a4f640a1540a88fba45d7a299fa219d503a1c`。
- 新事务 `artifacts/collection_roi_speed`；38 项 native、570 项收藏 Python、离屏 UI、SQLite 和隔离副本回滚均通过。旧 `collection_scroll_speed` 四件与冻结文件保持不变。详见 `docs/COLLECTION_ROI_SPEED.md`。
- 用户已回“开始”，最终 `artifacts/collection_roi_speed/ab05/result.json` 为 passed：三种模式各暖机 1 次、正式 6 次，21 个成功样本用原候选 matcher 回放一致。A 完整采集识别步骤中位 898.5 ms，B 去预览后 703.0 ms，C 再去重复文字后 672.0 ms，A→C 中位耗时降低 **25.21%**。这是同 native、同条目开关对照，不是完整收藏速度、旧 EXE 对照或 BBZPS 同机对照。
- `ab04` 被测速脚本的“一次尝试才算稳态”条件中断，生产重读其实已成功；原脚本和失败记录均保留。只修正统计，使全部正常重读和等待计入样本，未改变产品逻辑。`ab05` 的 A 正式组 6 样本用了 7 次采集，B/C 各 6 次；均值降低 32.99% 有单侧重读影响，不作为主要提速口径。
- `ab05/independent_audit.json` 逐步复核通过：IDE 已恢复，前置/恢复各一次，零点星、零购买、零游戏图像落盘；239 条 journal 与配置逐字节不变。选中条目价格 500 > 上限 260、`eligible=false`，仅作同屏读取测量，没有启动超价扫描或收藏。`live_result.json` 已改为实测完成，不再等待“开始”。
- 上轮 21/21 收藏规则完成状态继续有效；本轮测速等待不覆盖该完成记录。普通 UI“运行”仍未接真实 CLI，不宣称此按钮已经可执行。

---

## ????????? 21/21 ??????????????2026-10-08?

- ???????`artifacts/collection_scroll_speed/white_star_run_08/summary.json`?`completed`??? 0?`task_file_fully_completed=true`???????? **21 ???????????????**???????????????
- ??????? `white_star_run_01..08` ? **50 ?????**?????? `sent=2`???????????/??/??/?????????? **189 ?** journal ?????? **239 ?**?233 confirmed + 6 ?? reconciliation??????????????????????????????????? 148 ????? / 74 ??? / 53 ??????? / 21 ?????
- **?????????**?12 ?????? 11 ???????????????????? **15 ?????**???? run02 ????????????????????4 ??? profile ??????/??/?????????????????????
- ??????????????????????????????????????????????? v2 ???????????**???? v2 ???? 0**??????????? reservation ????????
- ??????????????????????????????????? 150/35 ?????????????????????????????????????????????????0.99 ????????????????
- run07 ?????????????????? ROI ????????????????????????????????????????????????????????????????`home_return_final_package/result.json` ????????? 1 ? `after_ui_anchor_refinement`?6 ????????0 ????journal/config ???????/??????
- ???????? native `51eb1022ffc3e0997482c8b014f4c03fd7646093e4a276e5ff79ef9a7ddaca6a`????? native `3fe79e8410fc06df30838d5f3e41a1fe401e1de831b8d7f57fa12a9c2d8cd41b` ?????????????????????????????**???????????????? 21 ?**?
- ???????`C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`??? UI??????????? CLI?????????? `collection\run_collection_observed.py`??????????????????????????????
- ???????????? IDE??????? 0??????????? K437 ?????checkpoint ? `latest_observation` ?????????????????????????? config SHA256 ?? `7c317fc1850e2ca85eeba394f73f9c231499f7a781dad570f95f0e4287b8f5b9`?
- ???`combined_white_star_live_review_v4.json`?????????`live_result.json`?`artifacts/m2_savedvalue_collection/current_checkpoint.json`?????/??/????/??/?????/??????????? `VERIFICATION.txt` ? `package_verification.json` ?????????????

### ????

??????????????? 13/21?14/21 ? 17/21 ????????????????????????????????????????????? UI ??????????????

??????????????/??/??????????????????????

---

## 最高优先级更新：62 px 两档已校准，正式推进到 13/21（2026-10-08）

- **用户已经决定：当前新帧确认符合配置的白星直接收藏；当前黄星（金星）保留，不点击。历史 confirmed 只作统计，绝不以“以前收藏过”拦当前白星。** 商品/成色/价格/磨损联合匹配及正式超价停止不变，识别未知不能用旧记录补成已知。
- 当前星标优先与追加式（append-only）attempt 记录已经实现；扩充 bank 后 `commands.json` 的 `EXPANDED_BANK_REGRESSIONS` 为 **550 项纯测试通过**（9.258s，退出 0），构建记录含 38 项 CTest 通过。旧 confirmed 保留，新的同身份尝试另建 v2 attempt；**未回执 pending 仍先对账/新帧回读，防止同一次不确定点击盲重派发**。
- 用户已回复开始，62 px 的旧缺口已补，不再等待用户准备。`probe_row12_delta480_retry2` 实测内容位移 591/587 px、滑块 25 px；`probe_row12_delta600` 为 735/733 px、滑块 31 px，两次 journal 未变、IDE 恢复。bank 现 4 条 profile：旧 2 条精确哈希保留，新 2 条独立测量的滑块高度范围为 61–63 px，不是将旧 70 px profile 按比例套用，也不是所有商品都已校准。
- 最新正式运行是 **`artifacts/collection_scroll_speed/white_star_run_02/summary.json`，13/21 已完成，行索引 `0..12`**；从 run01 续接，使用重新打包的发布 CLI。row12 AUG 黑银先锋 S 经实际 `-480` 翻页回读后，以价格 320 > 上限 300 完成原价格停止边界。下一启用行为 row14，row13 本来禁用。
- 第二段检查 13 条，**新增收藏 9 次、保留黄星 3 次、不匹配 1 次**。9 条新 journal 均 confirmed/toast_and_gold、均 `unseen_white` 首次身份键；第一段新增 8 条与原 189 条哈希保持，总 **206** 条（200 confirmed+6历史reconciliation）。两段 17 次仍未实机走 v2 历史同身份白星追加；该分支只由离线测试覆盖，不能称旧磨损 `0.739214` 白星已重新收藏。
- 当前失败段位于 **row14 AS Val 黑银先锋 S，上限300**：该行6张完整卡已取得收藏回执，随后 `-480` 已派发，但新帧 `COLLECTION_SCROLL_BATCH_BOUNDARY` 使外层 `BATCH_STEP_FAILED`、退出1；`pending_collection=null`，**`pending_geometry.kind=scroll` 保留**，IDE已恢复、整段enter/leave各1。正在处理可见尾部纹理峰导致的边界选取问题，不把末次滚动写成回读成功。
- 修复后从 `white_star_run_02/summary.json` 读取完成前缀和计数创建**新会话**，重新 startup/open product/condition/ensure list top，在未完成row14规则头新帧观察。恢复器不导入历史pending_geometry或旧lease；新会话检查自身pending和真实journal。无需手工清日志/修改旧摘要，不得在旧段直接重发滚动。若全局checkpoint仍指更早摘要，由主流程同步来源；本次文档代理没有改checkpoint、bank或journal。
- 这项政策已取代旧版冲突暂停说明，**不再要求用户决定重新收藏或跳过**。普通 UI“运行”仍未接真实收藏 CLI。最终构建、`verify`、`finalize` 的验证命令与哈希见最终 `artifacts/collection_scroll_speed/VERIFICATION.txt`，不填写预计 PASS；详见 [收藏滚动提速](docs/COLLECTION_SCROLL_SPEED.md) 顶部。

---

## 最新：翻页专项已通过四窗，正式价格停止已回放核对（2026-10-08）

- 用户已确认翻页效果并要求继续推进。先读 [收藏滚动提速](docs/COLLECTION_SCROLL_SPEED.md) 的最新段，不沿用下方初版“连续窗口未完成”的旧状态。`artifacts/collection_scroll_speed/windows_03/result.json` 为 passed：4 窗 `6/4/6/4` 条完整卡观测，共 20 条，3 次滚动 `-600/-480/-600` 均有新帧回读。首个 `-600` 来自显式已测首窗选项，后两次为正常适选；不把它描述成全程固定 5 格。
- 本次为 `read_only_module_test`，没有点星、购买或游戏图像落盘，189 条既有 journal 原样保留，pending 均为空；整段前置/恢复各一次，IDE 已恢复。两个观测组合口径（含价格、排除价格并规范化磨损）跨窗重复均为 0，但不是服务端唯一挂单 ID。`live_result.json` 与 `final_independent_review.json` 指向这次专项，不代表新增 20 条关注或 21 行业务完成。
- 后续 `windows_04/result.json` 是独立的 `BATCH_FOREGROUND_LOST` 失败，0 个完成窗口、0 条观测；IDE 恢复、journal 未变。不要把这次失败覆盖成成功，也不要用旧实测坐标继续点击。
- 价格超过 230 仍读到 798 是**固定 4 窗翻页专项**按窗口数推进，并未调用正式 `scan_row` / `collect_selected` 价格停止链；真实任务上限没有改成 798。`price_boundary_review.json` 已分别回放正式 session 价格门和实际 `scan_row`：记录识别价 300、上限 230，只检查 1 条即 `original_price_stop_boundary`，后续选卡/滚动/点击均为 0。该结果是离线记录字段+假后端回放，不是又跑完一轮实机收藏。
- 最新滚动实现保留逐窗完整卡覆盖、首个未扫裁切边界和实测 profile bank；缺左边的裁切卡只用其真实右侧与同帧完整列锚点唯一归属，不补造左边、不允许点半卡。选卡就绪分支只允许有界新帧重读（包含严格共同至多 2 px 小位移证据），不重复派发点击、不绕过最终选中绑定。参见 `combined_readiness_partial_review.json`、`adaptive_shape_and_phase_review.json`。
- 本段四窗专项的原校准对象是 P90 天命 C；后续已补 AUG 黑银先锋 S 的实测62px两档，均在2560×1440及各自窄滑块范围内。仍不宣称全部商品/21行/其他分辨率适用。原 BBZPS 精确 wheel delta 未恢复，`-480/-600` 是本机独立测量值。
- **本段所引用的 9/21 SR-25 停机已是历史，最新正式进度为顶部 13/21。** 当时磨损 `0.739214` 的历史 230 confirmed / 当前 320 白星错误记录仍保留；白星/黄星新策略已经实现。两段17个首次身份键的实机成功不等于历史同身份白星v2分支已覆盖，不删旧journal、不直接重放历史点击。
- **普通 UI“运行”仍未接完整实机 CLI。** 固定解压入口为 `C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`；发布 Python 入口为同目录 `collection\run_collection_observed.py`。本轮最终包由主流程重新构建/离屏/存储/源码一致性/隔离回退验收；本段不预先声明通过。以最终 `artifacts/collection_scroll_speed/VERIFICATION.txt`、`package_verification.json`、发布 `runtime_manifest.json` 的实际退出状态和哈希为准，不把 `windows_03` 的专项通过当作新包已通过。
- 接续交付命令见上述文档；不自动启动可见前端，不逐动作切屏。当前段只更新文档，没有运行游戏、构建或修改业务源码。

---

以下保留此前阶段记录。下面“最新/当前”均只指该段当时；旧9/21及第一段12/21运行文件没有被覆盖，新的独立发布CLI运行已推进至顶部所列13/21，旧confirmed拦白星政策已由新实现取代。

## 最新：收藏已提速，遇到已收藏记录与当前白星冲突

- 先读 [收藏提速验收](docs/COLLECTION_SPEED.md)。最新 `artifacts/collection_speed/run_04/summary.json` 为 blocked，完成9/21启用规则，IDE已恢复、pending均为空，`exhaustive_market_scan=false`。新整份周期尚未全过。
- 当时阻塞是SR-25天命A/磨损0.739214历史230已确认关注、画面320白星；该次没有再点，也没有删除历史journal。2026-10-08用户已确定改为“当前符合条件白星收藏、黄星保留、历史只统计”，这条保留为旧策略失败记录，不再作为待用户决策事项。见 `artifacts/collection_speed/confirmed_state_conflict.json`；新实现/实机验收状态以上方段落为准。
- 新整份周期新增 24 条；含上轮恢复段，本事务新增 26 条，原 163 条 journal 原样保留。历史总数不是当前关注数。
- 480 价格故障已由固定本地数字 provider 在真实新帧解决；成色小写 s 只做严格 ASCII 大小写标准化，再核对同帧详情。跨capture OCR常驻、动态120/80/180ms+新帧屏障已实际启用；不是只调鼠标速度。
- 发布程序仍在固定解压目录，普通UI“运行”尚未接该实机CLI。入口新加 `--local-price-ocr --fast-capture`（保留 `--numeric-price --local-title-ocr`）。每段一个前台租约，游戏图像不落盘。
- 最新 checkpoint 已重新生成；不用旧 frame/card 坐标。原 failure 和旧 full_cycle/run12 均保留为历史，不能覆盖上面最新事实。

---

以下为历史阶段记录，日期、状态和“最新”均只指该段当时。

## 最新实机：本次完整收藏 CLI 已完成，普通 UI Run 尚未接通

- 最后 `artifacts/collection_full_cycle/run_12/summary.json` 为completed、task_file_fully_completed=true，退出0、IDE恢复。冻结正式配置50行/21启用/29禁用/9商品；21条规则全部按原业务价格边界完成，**不是未看尾部也已扫描或全市场永久穷尽**，exhaustive_market_scan=false。
- 12段累计293次候选检查、新增98条、174次保留已有金星、21个不符合规则候选。98个唯一星标动作均有白星到金星的新帧机器回执；历史journal144条（138 confirmed+6历史reconciliation）不是当前关注数。174是事件数，含重复读取；最终pending收藏和geometry均空，未购买或移动库存。
- 正式config/snapshot字节hash未变。独立 `artifacts/collection_full_cycle/independent_final_review.json` 核对resume链、累加计数、逐条sent=2/同身份/不同帧/金星回执及最终边界。旧错误保留，不把早期遮挡、run04磨损断点或历史卡片坐标当作当前状态。
- 标题与目录通过运行前显式固定的RapidOCR3.9.2/PP-OCRv6-local-CPU读取同帧原ROI，模型SHA、词框/真实score、固定0.97门槛均保留；不传期待商品、不按配置替换输出。Windows原P/PSB/p"和ASVa[反例留档；真实AS Val黑银先锋与其他非目标皮肤见local_catalog_provider_review.json。
- 价格仍为Windows实际数字ROI；磨损保留实际直读或中文前缀+英文真实带点小数尾的严格同帧关联，不借配置补数字/点。run12真实“气象感应”由Windows3x窄区读回，再跑原CatalogFilterReader与六checkbox，见season_readback_provider_review.json。
- **普通UI“运行”按钮仍未接通这条实机链路。** 实际入口为独立.tools/ocr-runtime/Scripts/python.exe tests/manual/run_collection_observed.py --numeric-price --local-title-ocr；购买、非零限量、仅磨损、满额及其他分辨率不能借本轮宣称全通过。
- 每结束段前置/恢复各一次，游戏图像写盘0；wrapper stdout含内存图像，勿写日志文件。固定解压路径仍为 `C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`。业务成功不等于最新包已验证，最终包/回滚以最新VERIFICATION发布哈希为准。
- 先读docs/COLLECTION_FULL_CYCLE.md；最终v2 checkpoint已发布到 `artifacts/m2_savedvalue_collection/current_checkpoint.json`，草稿保留在 `artifacts/collection_full_cycle/doc_drafts/checkpoint.final.draft.json`。不要合并旧frame/card/错误字段；完成记录不授予历史位置继续动作，新动作重新观察。

---

以下为历史阶段记录，其中的“当前”“最新”仅指当时状态，均由上方run12结果覆盖；保留用于故障追溯。


## 最新状态：新一轮全配置测试被外部覆盖层阻断

- 先读 `docs/COLLECTION_FULL_CYCLE.md` 与 `artifacts/collection_full_cycle/price_probe_01/result.json`。本轮首次原生采集返回 `E_WINDOW_OCCLUDED`，无游戏帧、无新收藏，已恢复 IDE。已告知用户关闭 NVIDIA 覆盖层和原助手悬浮条，当前不能把上轮页面当成新观察。
- 新增数字字块预处理与严格逆映射、已派发收藏的 receipt-only 局部新帧回执、全配置断点续跑；这些是离线实现，不是全游戏流程成功。普通 UI“运行”按钮仍未接通。
- 继续时先验证真实同帧价格识别，再从本事务冻结配置执行；完整21启用规则/9皮肤尚未跑完。不得补数字或重放46条历史确认记录。后续 `run_01` 使用新事务目录，旧摘要没有续跑指纹，不当作新resume来源。
- 详细验证与原包隔离回滚在 `artifacts/collection_full_cycle/VERIFICATION.txt`，不把文件包通过等同于业务通过。固定解压路径不变。

## 最新状态：全配置调度与鼠标轨迹（最终核对 2026-10-08，开始于 2026-10-07）

- **全配置业务未跑通。** 当前配置仍为 50 行/21 启用/9 皮肤；已改全量调度，但五次整段运行均被真实识别不稳定中断，尚无一条完整规则结束，更没有全部枪收藏完成。不要把225项离线用例或鼠标测试当成全业务验收。
- 鼠标已从直接定位改为确定性 Bézier + 缓入缓出，计划120–280ms，进入/点击/滚动定位/恢复光标都经实际多点移动。游戏操作仍按整段前台，一次开始一次恢复IDE；末次已恢复IDE。
- 本轮实际点星4次：3条机器新帧字段回读确认，1条为独立内存画面视觉确认。历史journal46条/pending0，不是当前关注页数量。本轮未重放已点星，未清空原关注，未购买，游戏图像未落盘。
- 末条 `AUG 天命 S / 300 / 0.217900` 已由内存preview明确核对金星，但机器OCR仍会把300漏读成30，右对齐校验拒绝。这是**真实阻塞仍在**，视觉对账没有把机器识别判成通过。当前停在该卡，旧C第六卡游标已失效；恢复必须重新采集。
- 动态几何已实测完整/半卡、选择、滚轮+6/-6像素绑定；仍偶发底部分隔缺失。新帧有界复采不重点击；价格同帧补读不借配置、不替字。参见 `docs/COLLECTION_ALL_RULES.md`、`docs/business_rebuild/12_dynamic_collection_geometry_tolerance.md` 和 `artifacts/collection_all_rules/execution_review.json`。
- 固定程序仍是 `dist/RelinkStudio/RelinkStudio.exe`。普通UI的运行按钮仍未接完整自动收藏，这轮实际执行入口为显式Python协调器。构建、离屏验收、保留用户数据的隔离回退详见本轮 `VERIFICATION.txt`；程序回退不撤销游戏关注。

## 最新实机：仅收藏试段与 OCR 提速（2026-10-07）

- 已实际执行当前正式配置的收藏半程，不购买：`artifacts/collection_live_trial/run_02` 原包连续段确认新增13，`run_03` 新包段新增4、保留11个可见已有金星；本轮17次新增全部新帧回读，历史journal累计42次不是当前关注总数。每段前置/恢复各一次，最后已回IDE。
- 最新观察为 **AUG 天命，成色 C，第六张完整卡片已金星**。`run_03` 在真实底部未覆盖区域停止 `COLLECTION_UNSCANNED_SCROLL_REGION`，不是21规则/9商品全部完成。旧“空关注”及P90断点已过时；以新checkpoint为准，恢复仍重新观察。
- 新 `collection_run_config.py` 从当前Task冻结执行快照，21启用/9商品；当前价格/成色/磨损/数量压过旧importSource，字节哈希固定，运行中变更即停。`collection_live_session.py` 提供整段前台会话和逐步日志，补全编码/传输尾段的年龄上界，5秒门槛未放宽。普通UI运行按钮仍未接完整业务。
- Windows OCR在单次诊断内共享隐藏helper及中/英引擎缓存，仍不是跨capture EXE常驻。Qt异步stdin显式适配，真实合成连读128断言、全36CTest通过；新包实机OCR.session已输出真实启动/调用计时。详情 `docs/COLLECTION_ONLY_TRIAL.md`，事务 `artifacts/collection_live_trial/VERIFICATION.txt`。
- 固定发布目录已更新；用户打开的旧前端没有被强关，其旧EXE保留在事务目录。目录/DLL完整及六张离屏UI与基线一致，隔离包回退通过。剩余真实阻塞是动态卖单几何/滚动；非零限量、仅磨损/不限成色、满额分支也仍未贯通，不把离线或局部成功冒充全流程。

## 最新核对：收藏全流程与速度（2026-10-07）

- 先读 `docs/business_rebuild/11_bbzps_collection_alignment_performance.md`：按原 S04–S31 对齐每项分支、当前覆盖与缺口，未把诊断脚本当成已接通的完整执行器。本轮只改文档及离线审计工具，没有改动发布程序、用户配置、游戏关注或当前 checkpoint。
- 历史 v5 两卡新增批次前台驻留 20.937 / 20.718 秒；20 次采集的 capture_ms 中位 213.5 ms，全部识别结束的帧龄中位 3028.5 ms。帧龄不是纯 OCR 耗时。主要结构问题是每观察冷启动程序、每 ROI 冷启动 PowerShell/引擎；另有通常每输入 0.6 秒、每短批 0.9 秒的实际等待。7 秒是回读预算，不是等待。
- 当前还缺跨季、目录翻页、完整组调度、公示/排序执行、仅磨损计划、非零限量、动态滚动和满额交接；UI 真实任务导入不等于 runtime 已消费当前编辑，旧联调仍使用固定历史 task_snapshot。完整分支表见新文档。
- `WorkerProcess` 只是传输框架，synthetic fixture 不是 OCR 服务；真实接入前先解决 Windows OCR 无 score、系统 provider 身份协议，并修正 preview/输出尾段未计入动作帧龄的问题。不要填假置信度、放宽门槛或直接删回执提速。
- 最新现场仍为空关注、旧游标失效。复算 `python -X utf8 docs/business_rebuild/scripts/audit_collection_timing.py`；事务 `artifacts/collection_alignment_audit/VERIFICATION.txt`。这轮审计不是提速已完成或新的实机跑分。

## 最新功能：收藏任务接入程序（2026-10-07）

- 任务页新增导入真实 `.savedValue` 的入口，全局导入也识别该类型；严格按真实目录和原字典关联，逐行预览，确认后原子保存再发布到 AppState。数量 0 在编辑、列表、配置往返统一表示不限；稳定来源去重保留导入后的编辑，不覆盖原关注。
- 本机正式 config.json 已通过新入口实际导入，重开校验为149皮肤/50任务/21启用；重复导入零新增且配置哈希不变。导入前原配置已备份，未启动任何任务。
- 真实输入核实为 50 行 / 21 启用 / 13 全部商品 / 9 启用商品。总商品 13 包含停用10100、10702、10703、10704，之前猜测10仅为新增测试预期错误，已独立核对原始字节纠正。
- 源码入口 `application/collection_task_import.*`、`collection_task_commit.*`、`ui/dialogs/collection_import_dialog.*`。GUI保存失败保留预览；CLI `--import-collection-tasks <file> --config <explicit file>` 为同一数据提交，输入失败不覆盖配置。测试全部隔离，真实来源不改。
- 新 `--collection-import-self-test` 使用真实主窗导入入口及持久化链路；`--task-source` 可只读指定实际原文件。程序路径不变，详细说明 `docs/COLLECTION_TASK_IMPORT.md`；本轮事务 `artifacts/collection_workflow/VERIFICATION.txt`。
- **当前游戏现场与旧断点不同：** 本轮最初观察为大厅；新批次沿大厅→曼德尔砖→典藏→我的关注，现页显示空关注。原25次已确认收藏是历史事实，不能当作当前在售列表；未清空/取消/添加关注，未购买。当前页不能使用旧P90 B第六张坐标。批次结束已回IDE。
- 新 `tests/manual/collection_scroll.py` 只落实同帧布局/半行拒绝/滚动位移证明/对账契约和离线测试，C++动态边界采集与runner动作尚未接通。别把41项离线回归或原固定六卡几何当作已实测滚动支持。

## 最新开发节点：真实皮肤目录接入（2026-10-07）

- 本轮请求为替换全部模拟皮肤、暂留空缩略图、支持以后追加新赛季。用户已允许最大数量子代理，本轮 3 个子代理并行完成数据层、目录 UI 与真实数据默认行为。
- 内置用户核对资料 S1–S11 共 149 项；来源 `docs/business_rebuild/skin_reference/skin_reference.json`，运行数据 `src/assets/catalog/skins.json`。保留原商品 ID、武器、皮肤系列、极品/优品、菜单颜色；正式游戏品质未知，不按颜色猜测。没有扩充超出用户模板的后续游戏目录。
- 正常启动使用真实目录，清除严格识别的旧默认模拟商品/任务，旧配置迁移前做逐字节备份；保留真实用户业务条件及非模板条目。无报价/磨损/涨跌时显示未知，不生成曲线、成交或统计。诊断 fixture 仅由显式 `--self-test` 等测试入口加载。
- 关注页的“皮肤资料 / 追加赛季”进入查看/搜索、单条追加、JSON 批量追加与模板导出。用户扩展保存到配置同级 `catalog_extensions.json`；冲突/重复/损坏/提交验证失败均不部分写入。新商品可直接用于任务配置，扩展独立于程序升级目录。
- 使用说明 `docs/REAL_SKIN_CATALOG.md`；新增 `--catalog-self-test` 走同一正常启动路径，使用临时目录离屏验证完整追加与迁移链路。交付和回滚证据 `artifacts/real_skin_catalog/VERIFICATION.txt`，程序仍为固定解压路径 `dist/RelinkStudio/RelinkStudio.exe`。
- 本轮未运行原 BBZPS/Relink 程序、未启动可见窗口、未操作游戏。此前 25 条实际关注及 P90 天命 B 的滚动暂停点不变，继续收藏前仍读下方原 checkpoint；不要重放已完成星标。
- `.savedValue` 真实任务文件的执行参数没有被目录截图或本轮目录迁移覆盖。任务目录与实际游戏关注账本是独立数据；当前 UI 不冒充已同步那 25 条实际游戏关注。

## 最新问题：皮肤品质资料核对（2026-10-07）

用户给出 S1–S11 共 11 张原助手菜单截图，追问模板中的皮肤品质是否已记录。核查确认：任务成色/价格/磨损/限量与商品 ID 已按 savedValue 记录，但原提取器没有保留颜色/独立品质字段；`grade=any` 是筛选不限，不是皮肤本身的品质。现在已整理 `docs/business_rebuild/skin_reference/皮肤基础表.md` 及 `skin_reference.json`，149 项的菜单颜色、极品/优品后缀与来源图分开保存，原有商品 ID 不变。14 红、14 橙、33 紫、88 蓝；当前启用的 9 款均属截图紫色项。正式游戏品质名称映射仍待确认，参考资料尚未接入生产任务快照或收藏校验；不要声称已修好品质筛选，也不要把截图中的 1003 方案价格覆盖 0925 任务文件。此轮没有启动游戏操作，之前 25 条关注及滚动暂停点保持不变。

## 当前实机节点：任务文件已实际收藏 25 单，滚动关联待补（2026-10-07）

- 用户已提供 `C:\Users\Administrator\Desktop\S11新赛季0925.savedValue` 并要求先实际收藏。下文所有“尚无真实任务/等待指定商品/只读观察”的旧安排均已过期。收藏本身包含加关注；购买在后续我的关注业务阶段，本轮未进入。
- 先读 `docs/business_rebuild/implementation/runtime/savedvalue_collection_m2.md`、`artifacts/m2_savedvalue_collection/current_checkpoint.json` 和 `session_audit.json`。原数据 50 行、21 行启用、9 个商品；原始文件及解析快照在本地忽略的 `input/` 目录，不是演示数据。
- 已确认新增 25 单：AUG 天命 10 单（S 3/A 2/B 3/C 2），P90 天命 15 单（S 4/A 5/B 6）。所有 pending 已对账，原有关注保留；**不要重跑首条添加或任何已确认条目的星标点击**。
- 当前游戏在 P90 天命、成色 B、第 1 页；卡片索引 5（第三行右侧）已收藏。六张完全可见卡片都是 230，仍低于该规则 280；底部还有半行，现有几何尚未覆盖滚动后的字段关联。最后系列 `live_series_s6_v5` 停于 `COLLECTION_UNSCANNED_SCROLL_REGION`，不是整份任务完成。下一步先补滚动/半行的定位与同帧关联，再从源行 6 接续；不要直接切到成色 C 或下一个商品。
- 最后一个批次已恢复 Mirasim 前台。游戏图片未写磁盘；窗口身份每次重新解析；批次内连续操作，不逐动作切屏。严禁运行原 BBZPS/Relink 样本或历史探针。
- 发布仍在固定解压目录 `dist/RelinkStudio`，白灰 UI 不变。28 组 CTest 和收藏辅助测试以本轮 `VERIFICATION.txt` 精确记录为准。原生大标题 P90 的数字识别仍有缺口：本轮仅人工核对截图批准过一次导航，收藏前始终复核完整卖单标题，不能假称大标题已自动识别成功。
- `journal/` 先持久化再点击，记录白星到金星及提示回执。首条 AUG、AUG B 和 P90 A 各有一次旧自动回读失败，已另写 reconciliation；旧失败不覆盖、不改标。回滚只验证程序包副本，不撤销游戏收藏。

## 最新沟通要求（2026-10-07）

用户要求持续推进，不逐阶段暂停，不输出例行进度、改动、测试或思考总结。详细证据留在文件中。只在真实阻塞、业务逻辑冲突或需要用户决策时简短说明问题；不要用一个普通里程碑结束开发。

## 当前：总筛选状态已实测，真实任务目标待指定（2026-10-07）

- 入口 `docs/business_rebuild/implementation/runtime/catalog_filter_state_m2.md`。`CatalogFilterReader` 已读取实际赛季及六个三态框；选中样式为白色内嵌方块。普通品阶选中时需要独立文字区同帧3倍OCR，未替换错字。
- 实测已覆盖六个框的选中/未选以及品阶多选；最终发布包完成17步连续批次，前置/回IDE各一次。筛选未确认，赛季/六个框已全部恢复。失败、恢复过程均保留，无购买、库存移动或关注更改。当前游戏仍在筛选页。
- 独立 `CatalogFilterPlan` 处理 S10–S14，先比赛季，再拥有，再品阶残留清理/重选；只产出差异与回读要求，不执行点击。不要从示例任务臆造真实分组。
- 27组CTest、筛选153断言、84组实测数值信号回放；6张UI与基线逐像素一致，正式解压目录已更新并在隔离副本验证回退。事务 `artifacts/m2_catalog_filter/VERIFICATION.txt`。
- 本机 `LocalAppData/RelinkStudio/RelinkStudio/config.json` 仍是 `schema_version=1, demo=true`，5条任务全部禁用。按真实任务继续S15查找及S17标题复核之前，需要用户指定测试枪皮及其赛季/成色；不把历史/演示配置当成当前执行目标。获得目标后继续，不重复校准现有六个框。
- 原StartupObserver默认1秒时效未动；同帧多ROI诊断尚未接进真实异步运行时。当前状态识别不等于完整自动运行按钮已接通。
- 批次上限现在20步，总时限仍30秒。字段断言失败不重放点击；失败后重新观察再恢复临时状态，最终恢复IDE。用户切走时不得抢回继续操作。

## 最新：市场页坐标与原关注预检实机通过（2026-10-07）

- 原规则Unknown的根因之一是Windows OCR `TextAngle`未处理，已按原图中心顺时针变换四角；曼德尔−6.1°、典藏+10.4°是本轮实际值。不是为通过测试篡改页面别名。
- 增加同帧局部OCR：导航/关注入口2x灰度对比度，空状态3x反色；全部内存处理。局部空结果不擦除原全页词；局部精确近邻拼接避开3D背景文字干扰。
- 最终发布包实机完成：空关注两次→首页→我的关注两次→首页→总筛选。7个匹配检查点、9次采集（2次Unknown重读全部保留）、4次导航；整段enter=1/leave=1、子诊断切屏=0，已恢复Mirasim。
- 游戏现在在总筛选页。没有勾选筛选项、增删关注、购买或移动物品。下一步S10–S14读取赛季/拥有/品阶状态；之后才是商品名/成色/价格绑定。
- 先读 `docs/business_rebuild/implementation/runtime/market_page_calibration_m2.md`。事务 `artifacts/m2_market_calibration/VERIFICATION.txt`；26组CTest、OCR75、市场70、原启动250断言、批次10项测试通过。交付仍为固定解压路径。
- `run_foreground_batch.py` 只对预期页Unknown做有界新帧重读（最多3），不重放点击、不重抢用户切走的前台。所有实机使用完整批次，禁止循环单步探针。
- 当前是页面校准与人工联调脚本导航，不是完整前端自动业务；默认StartupObserver的一秒门槛尚未改动，实际多ROI诊断可能超过它，下一步接异步运行时时必须明确页类新鲜度，不能冒充已接通。

下方为此前阶段记录；“曼德尔仍Unknown”由上方实测更新取代。

## 最高优先级纠正：禁止逐动作切屏（2026-10-07）

- 用户明确指出闪屏：一段任务开始才切到游戏，中间连续操作/识别，整段结束或异常时才恢复IDE一次。用户也要求由智能体点击页面导航，不再逐页让用户代点。
- 已实现 `ForegroundPolicy` 与 `tests/manual/run_foreground_batch.py`；子诊断使用 `--focus-policy caller-owned`，不激活/不恢复。原逐动作脚本已转接批次。25/25 CTest、前台策略测试、Python协调器6项通过；一段真实2帧检查仅1次enter/1次leave，两个子诊断焦点请求均为0。
- 最新入口 `docs/business_rebuild/implementation/runtime/foreground_batch_m2.md`。游戏目前停在曼德尔砖市场页，已经观察到“典藏外观”页签；从大厅底部F4市场进入，不是顶部普通交易行。分类器对当前曼德尔页仍Unknown，下一步先后台补最小诊断再在一个完整批次校准，不能假报页面通过。
- 本轮用户授权后有3次人工导航点击，无购买、库存移动、关注修改；初始大厅只读阶段和后来导航阶段分别记账。最后新批次只读检查已恢复Mirasim前台。
- 交付路径不变，事务 `artifacts/m2_lobby_calibration/VERIFICATION.txt`。大厅实测对应的旧二进制保存在 `LOBBY_CALIBRATED_FILE.exe`，不要把旧实测哈希改标为新焦点修复版。

以下历史记录若说“每次检查都回IDE”或“等待用户逐页点击”，均由上方新约定取代。

## 最新实机节点：大厅校准通过（2026-10-07）

- 用户已准备大厅；本轮实机2560×1440、144DPI。原规则因Windows OCR将“开始游戏”读成“开始游观”而Unknown；已做provider+固定区域+仓库/行前备战独立锚点的局部兼容。
- 修复发布包两次独立帧均判为lobby；故意expected-page=warehouse时退出1，不把采集成功当页类成功。所有尝试含一次初始前台变化失败均保留，均恢复IDE；游戏图片未落盘，无游戏输入。
- 应用CTest24/24；大厅专项37、原启动250断言；包内另有1条实机文字投影回放。六张UI基线不变，发布与回滚已验收。
- 先读 `docs/business_rebuild/implementation/runtime/lobby_live_calibration_m2.md`。新事务 `artifacts/m2_lobby_calibration/VERIFICATION.txt`；程序固定路径不变。
- 已请用户手动点顶部“仓库”并回报到页；收到准备消息后用 `tests/manual/live_startup_probe.py --expected-page warehouse --record <新的artifacts内json路径>` 做下一页只读检查。每次重解析PID/HWND，不复用本轮编号；结束检查IDE前台。尚未验证仓库/曼德尔/典藏/商品字段，更没有自动导航。

以下为此前阶段记录。

## 最新开发节点：原启动只读观察器已接入（2026-10-07）

- 用户问实机何时需要：本轮先完成离线开发与回归，没有切换游戏前台或采集游戏画面；下一步需要用户准备后做当前版本页面校准。
- 已有 `SkinPageClassifier` / `StartupObserver` 独立QtCore模块；按原顺序处理关注预检、返回首页再筛选、已有列表接续。只有检查点输出，无采集/导航/购买命令。
- 27条历史OCR锚点投影已嵌入发布程序，`--startup-observer-self-test` 离线检查27页/5分支；不是当前游戏图片识别准确率。应用CTest现为23组。
- 原日志“大战场/应用外观”在部分记录里实际为视频设置页；已保留旧标签但不将其作为页类真值。
- 先读 `docs/business_rebuild/implementation/runtime/startup_observer_m2.md`；本轮事务 `artifacts/m2_startup_observer_transaction/VERIFICATION.txt`。
- 实机准备：游戏停大厅，保留现有分辨率/DPI及关注条目；用户准备后逐页只读校准，结束后恢复IDE前台。不要擅自去普通物资交易行或清空关注。

下方保留前一阶段记录。

## 当前入口：先复刻 BBZPS 原启动流程（2026-10-07）

用户最新纠正：仔细拆解BBZPS首次启动全部流程，按原业务直接复刻，不自行重写业务路径。
先读 `docs/business_rebuild/10_bbzps_first_startup_reconstruction.md`：39个步骤、各入口分派、日志逐行证据，特别是“先检查我的关注”“已有皮肤列表直接接续”，不是普通物资交易行。

- 已全量重查15日志/1,774,992,038字节；97次全自动皮肤和3次发送测试入口；1192条定位重新校验。冷启动UI函数序列尚有明确缺口，不伪称全还原。
- M2新增 `relink_windows_capture` 与 `relink_windows_ocr`；真实游戏DXGI内存3帧通过，Windows OCR已实际识别合成文字。真实枪皮字段和39步导航尚未连接游戏。
- 当前应用CTest 21/21；窗口104、OCR52条断言；原流程证据契约12项；原UI和账本回归保持。新证据在 `artifacts/m2_live_capture_transaction/VERIFICATION.txt`。
- 用户已允许短暂前置游戏做画面验证，每轮结束恢复并校验Mirasim为前台。用户转而要求深拆原流程后，先完成原业务规范，不再临时引导到猜测的页面。
- 固定解压交付 `dist/RelinkStudio/RelinkStudio.exe`，保留DLL、platforms、sqldrivers、vision/windows_ocr_worker.ps1、qt.conf；UI白灰不变，参数仍只保存，未接入点击购买。
- 继续时复用已写的采集/窗口/共享内存接口，不重新做PR12B；先按原流程实现页类与只读启动路径回放。

下文为PR12B历史交接记录，不覆盖以上新进展。

## PR12B 历史开发节点与 M2 非游戏基础层

续接日期：2026-10-07（Asia/Shanghai）。用户要求继续全部非游戏开发和验收，只在需要真实游戏画面验证识别业务时暂停总结。

- PR01–PR11 和 PR12A 已完成；PR12B 增加四个独立服务，历史列表先读摘要、选中运行才读详细事件/账本/审计。
- `src/ui/pages/`、`src/ui/dialogs/`、`src/diagnostics/` 保持页面、编辑器和离屏验收边界；`relink_ui` 不依赖 SQLite。
- `src/application/runtime/observation/` 实现步骤触发、有界内存帧、请求关联、新鲜度、取消和资源回收；来源仍为合成回放。
- `src/application/vision/` 已实现协议/lease，以及独立 `relink_vision_transport` 的 Win32 共享帧和隐藏合成子进程；fixture child 不是 OCR worker，不随桌面包发布，桌面程序未链接传输模块。
- 当前源码已有完整 CTest 19/19 记录；工作区 432、观察 828、协议 646、进程内链路 279、共享帧 65、隐藏合成进程传输 206 条断言通过；独立负向包验收记录为 41/41。最终发布、哈希与副本回退以本轮构建后更新的 `VERIFICATION.txt` 为准，不沿用旧包证据。
- 快照单独运行不得改写配置/工作区。`tests/verify_delivery.py` 参数化 build/release/output，使用新目录验证全部包文件清单、工作区字节保持和真实回退脚本。
- 优先读 `docs/business_rebuild/09_m1_progress.md`、`implementation/runtime/observation_adapter_pr12b.md`、`implementation/runtime/worker_protocol_pr12b.md`、`implementation/runtime/vision_transport_pr12b.md` 和 `NEXT_IMPLEMENTATION.md`；不重复重做 PR12A/PR12B。下一步需要真实游戏窗口/帧、ROI 与 OCR provider 校准，通用内存/进程传输已验证。
- 保存仍不等于启用：`enabled=false`、`activation_required=true`；真实画面识别不等于确认成交，更不自动接入购买。
- 构建发布：`build.ps1 -Test -Package`；发布验收：`python -I -X utf8 tests/release/verify_pr12b_package.py modified --build-dir build_relocated`。
- 固定交付为 `dist/RelinkStudio/RelinkStudio.exe`，保留 DLL、`platforms`、`sqldrivers`、`qt.conf`；只离屏验收，不弹出可见窗口。

## 当前 UI 版本：0.6 Windows 11 浅色 · 微软商店布局

用户喜欢微软商店 / Win11 的界面，2026-10-06 明确要求“不要蓝色”“按 Win11 同款白灰配色”（系统为浅色模式），取代 10-05 的深色要求。当前是 Win11 浅色主题、中性近黑强调色、商店式顶部搜索、左侧图标导航和左上圆角内容层。用户还提供了原程序“Relink枪皮助手”主界面截图，要求全部功能入口：已集中在“运行”页与“任务”页（含成色）。

- 更新后的程序仍在 `dist/RelinkStudio/RelinkStudio.exe`。
- 当前说明与原程序入口对照：`docs/STORE_UI.md`。
- UI 0.6 样式改版的历史验收与恢复记录：`artifacts/store_ui/VERIFICATION.txt`；0.5 备份在 `artifacts/store_ui/baseline/`。当前 PR12B 发布状态看本轮事务报告。
- 直接修改当前 `src/`，不重复执行一次性补丁脚本；不自动开启前台窗口，验收继续离屏。
- “优化”入口的具体功能待用户确认；运行参数只保存配置，不执行点击或购买。

这个目录是迁移后的工作根目录：

```text
C:\Users\Administrator\Desktop\price
```

## 先从这里继续

- 可运行前端：`dist\RelinkStudio\RelinkStudio.exe`
- 源码：`src\`
- Qt/CMake 工程：`CMakeLists.txt`
- 构建脚本：`build.ps1`
- 前端交付说明：`docs\FRONTEND_DELIVERY.md`
- 迁移校验：`artifacts\MIGRATION_VERIFICATION.txt`

## 构建和验证

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Package
```

迁移后的 `build.ps1` 会检测复制前留下的旧 CMake 缓存；如果 `build\CMakeCache.txt` 仍指向旧目录，会自动使用 `build_relocated\`，不会把新工程写回旧路径。

## 普通前端模式的功能边界

正常双击启动为前端和本地演示；显式 `--live-capture-check` 是独立只读诊断，不属于下列演示引擎：

- 不连接游戏进程。
- 不读取游戏内存。
- 不采集游戏画面。
- 不发送键盘或鼠标输入。
- 不执行收藏、购买或交易。
- 价格、皮肤、统计和运行结果都是演示数据。

后续接入市场页面时，继续从这个目录的 `src/` 和状态模型扩展即可。

## 迁移说明

原项目目录保留在：

```text
C:\Users\Administrator\Desktop\ida\relink_studio
```

原目录作为回滚副本保留；当前新会话使用 `C:\Users\Administrator\Desktop\price`。
