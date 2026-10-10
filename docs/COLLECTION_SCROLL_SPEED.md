# 收藏列表滚动提速：测量、适用范围与交付入口

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

## 最新业务状态：62 px 校准已补，发布 CLI 推进到 13/21（2026-10-08）

**用户已明确：以当前新帧星标和当前规则匹配结果决定动作；历史 confirmed 记录只作统计，绝不因为“以前收藏过”而拦截当前符合条件的白星。** 这项决定取代下文旧版 `COLLECTION_CONFIRMED_STATE_CONFLICT` 的业务策略，不再等待用户选择重新收藏或跳过。

| 当前观测/执行状态 | 已确定的处理规则 | 历史记录的作用 |
|---|---|---|
| 新帧完整确认白星，商品/成色/价格/磨损符合配置，且没有未决输入 | 执行本次收藏，并用后续新帧确认成功 | 旧 confirmed 不阻拦，不据旧坐标直接重放 |
| 新帧确认黄星（金星） | 保留，不点击切换星标 | 保留历史统计，不以历史与当前差异强制取消或再收藏 |
| 当前不符合配置，或已触发正式价格停止边界 | 继续遵守不符合条件/超价停止规则 | 旧记录不把不符合条件的候选变成可收藏 |
| 当前有尚未取得回执的 `prepared` / `dispatched` / 不确定输入 | 先对账或有界新帧回读，不盲目再派发同一次点击 | 这是未决动作去重，不是历史 confirmed 拦截 |
| 星色、标题、字段或新帧绑定尚未证明 | 继续观察或明确停止，不凭历史猜当前状态 | 历史不替代当前观测 |

**实现状态：当前星标优先策略与追加式（append-only）attempt 记录已接入；扩充 bank 后的 550 项收藏离线测试及 38 项 CTest 通过。** `commands.json` 中 `EXPANDED_BANK_REGRESSIONS` 的实际命令为 `python.exe -B -X utf8 -m unittest discover -s tests/manual -p test_collection*.py`，结果 `Ran 550 tests in 9.258s / OK`、退出 0。这些测试包含当前白星、当前黄星、历史同身份追加尝试、pending 防重复及记录字段回放，不是 550 次游戏测试。新的同身份尝试保留旧 confirmed 并另建 v2 attempt；未回执 pending 仍阻止同次输入盲重派发。构建/CTest 原始输出在同一命令记录中；最终交付验证仍以最后一次 `VERIFICATION.txt` 为准。

### 发布入口实际续跑结果

最新正式运行是 [white_star_run_02/summary.json](../artifacts/collection_scroll_speed/white_star_run_02/summary.json)，从 `white_star_run_01` 续接，入口来自重新打包的 `dist/RelinkStudio/collection/run_collection_observed.py`，并非仅运行源码目录的测试替身。前一段的 8 次收藏记录及其 [white_star_live_review.json](../artifacts/collection_scroll_speed/white_star_live_review.json) 保留，不覆盖成第二段结果。

| 项目 | 已核实事实 | 结论边界 |
|---|---|---|
| 规则进度 | **13/21**，已完成行索引 `0..12`；本段完成行 12 | 下一条启用规则为行 14，行 13 本来禁用；整份任务仍未完成 |
| 本段候选处理 | 检查 13 条，新增关注 **9 次**、保留已有黄星 3 次、不匹配 1 次 | 9 条新 journal 均为 `confirmed / toast_and_gold`；不把仅派发点击计为成功 |
| 历史记录 | 初始 **189** 条与第一段新增 8 条哈希均不变；本段再加 9 条，总 **206** 条 | 200 confirmed + 6 历史 reconciliation；journal 数量不是当前游戏关注列表大小 |
| 本段真实分支 | 9 次均为 `unseen_white_requires_candidate_and_journal`，使用首次身份键记录 | 两段合计 17 次均不是 v2 同身份追加；**v2 历史同身份白星仍只由离线测试覆盖** |
| 已完成价格边界 | 行 12 AUG 黑银先锋 S 经真实 `-480` 翻页回读后，读到 320 超过上限 300 | `original_price_stop_boundary`，不是未观察尾部已穷尽 |
| 整段前台与未决状态 | enter/leave 各 1，IDE 已恢复；`pending_collection=null`，但 **`pending_geometry.kind=scroll`** | 最后一次行 14 滚动已派发，不能把缺少几何回执写成成功 |
| 运行结束 | `blocked`、退出 1，外层 `BATCH_STEP_FAILED`，末次捕获内层 `COLLECTION_SCROLL_BATCH_BOUNDARY` | 当前问题是行 14 的滚动边界回读，不再是行 12 缺少 62 px profile |

旧 `collection_speed/run_04` 中 SR-25 天命 A、磨损 `0.739214` 的历史 230 confirmed / 当时 320 白星停机记录保留为历史。新实现已经取消该旧策略否决，正式续跑也已推进过 SR-25 规则；但不能把两段 17 个首次身份键的成功，写成那张历史同身份白星卡已经在实机重新收藏。最新完成前缀和计数应读取 `white_star_run_02/summary.json`；旧 checkpoint 若仍指较早摘要，应由交接主流程同步，不能把进度退回 9/21 或 12/21。

### 行 12 的两档独立测量已加入 bank

用户已回复开始，行 12 的旧校准缺口已由两次独立实际输入补齐，不再处于等待用户准备状态：

| 独立测量 | 实际输入 | 两列内容位移 | 滑块位移 | 新 profile 范围 |
|---|---|---|---|---|
| [probe_row12_delta480_retry2](../artifacts/collection_scroll_speed/probe_row12_delta480_retry2/result.json) | `-480` | 591 / 587 px | 25 px | 滑块高 61–63；内容位移 584–594；滑块位移 24–26 px |
| [probe_row12_delta600](../artifacts/collection_scroll_speed/probe_row12_delta600/result.json) | `-600` | 735 / 733 px | 31 px | 滑块高 61–63；内容位移 730–738；滑块位移 30–32 px |

两次测量均为 `measured`，没有点星或购买，journal 未变、pending 为空、IDE 已恢复。`calibration_bank.json` 现为 **4 条 profile**：原 P90 C 两条精确文件哈希保留，新增两条分别绑定自己的测量文件，绝非将原 70 px 滑块的位移按比例套到 62 px。早期 [probe_row12_delta480/result.json](../artifacts/collection_scroll_speed/probe_row12_delta480/result.json) 的 `CURSOR_INTERFERENCE`／零步骤失败保留为历史，不能代替后来成功测量。

新 bank 仍只覆盖已测几何与窄滑块范围。正式第二段中 AUG 行 12 的新 profile 已实际滚动回读通过；同样为 62 px 的 AS Val 行 14 虽通过 profile 选择，随后仍因边界回读不符停止。这说明滑块高度相同不等于所有商品/纹理/窗口相位均已验证，不得扩写为全商品自动泛化。

### 当前行 14 失败段怎样接续

当前未完成规则为 **AS Val 黑银先锋 · 成色 S，行 14，上限 300**。该行当前 6 张完整卡已取得收藏回执，随后 `-480` 输入已派发；新的捕获未通过 `COLLECTION_SCROLL_BATCH_BOUNDARY`，失败段的 pending scroll 证据保留。正在处理真实可见尾部纹理峰导致的边界选取问题，此处不预先宣称修复或补测通过。

修复后可从 `white_star_run_02/summary.json` 的完成前缀/计数创建**新会话**，重新执行 startup → open product → condition → ensure list top，在未完成行 14 规则头重新观察。恢复器不把历史摘要的 pending geometry/旧 lease 导入为下一次输入；新会话仍检查自身 pending 与真实 journal。无需手工清 journal 或伪改旧摘要，也不能在旧会话直接重发末次滚动。原成功关注应由当前黄星保留。

最终构建、`verify`、`finalize` 的**验证命令与哈希见最终 `VERIFICATION.txt`**。此处不填写预计 PASS，不把以上发布 CLI 的实机事实替代最后一次重新打包验收。

---

## 最新结论：四窗口翻页通过，正式业务与发布验收分开记录

更新日期：2026-10-08。用户已确认本次翻页效果并要求继续推进。**`windows_03` 已完成 4 个窗口、20 条候选观测和 3 次滚动；这是 P90 天命成色 C 的翻页专项通过，不是全商品收藏完成。** 后续 `windows_04` 在开始阶段丢失前台而停止，没有形成新的窗口验收。本文新增段优先于下方初版的“连续窗口仍待完成”状态，早期失败证据不覆盖。

本轮最终程序包尚由构建/发布验收流程确认。固定解压入口仍为 `C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`；**普通 UI“运行”按钮仍未连接实际收藏 CLI**。以下翻页实测、价格回放与最新发布包验证是三类独立结论，不相互替代。

### 四个窗口实际读到了什么

证据目录：[windows_03](../artifacts/collection_scroll_speed/windows_03/result.json)。独立复核：[final_independent_review.json](../artifacts/collection_scroll_speed/final_independent_review.json)；本轮 `live_result.json` 指向这次成功专项，不把后来的失败试验改写为成功。

| 窗口索引 | 完整卡候选观测数 | 随后实际滚动 | 选择方式 | 与以前窗口重复的两种观测组合数 |
|---|---:|---|---|---:|
| 0 | 6 | `-600` | 显式首窗实测 profile，仍需通过当前覆盖/形状/距离检查 | 0 / 0 |
| 1 | 4 | `-480` | 正常逐窗自适应 | 0 / 0 |
| 2 | 6 | `-600` | 正常逐窗自适应 | 0 / 0 |
| 3 | 4 | 无；达到本次固定窗口数 | 不再滚动 | 0 / 0 |

- 3 次输入顺序为 **`-600 → -480 → -600`**，每次均通过新的滚动回读。`--first-delta -600` 只约束首窗从已测 profile 中选择，不是对全部窗口强制 5 格；后两次仍由当前窗口的边界决定。
- 20 条分别按“商品/成色/价格/磨损”以及“商品/成色/规范化磨损”核对，两个组合口径均为 20 个不同值，跨窗重复数均为 0。它们是本地观测组合，**不是服务端唯一挂单 ID**；不据此宣称永久身份已证明或所有未来市场变化下都不会重读。
- 总耗时 `50405.465 ms`，包含进入、导航、逐卡选择/识别和退出，不是单次滚轮耗时。没有同环境运行 BBZPS 的对照，不能据此给出相对 BBZPS 的提速倍数。
- 实际点星、购买和游戏图像落盘均为 0；189 条既有 journal 字节未变，最终 `pending_collection=null`、`pending_geometry=null`。整段 `enter_calls=1`、`leave_calls=1`，IDE 已恢复；常驻采集 worker 的 33 次请求完成并正常退出。
- 这次结束是“已读完指定 4 窗”，不是行情列表到尾、不是价格规则完成，也不是 20 条新增关注。后续动作必须重新采集，旧 frame/card 坐标不继承为可操作位置。

`windows_04/result.json` 保留为独立记录：`blocked / BATCH_FOREGROUND_LOST`，0 个完成窗口、0 条候选观测，journal 未变，IDE 已恢复。它没有追加通过数量，也不抹去 `windows_03` 的既有测量。

### 为什么价格超过 230 后专项还继续读到 798

本次运行的是 `verify_collection_scroll_live.py --windows 4 --first-delta -600`：先把窗口内完整卡逐个选中读值，再验证覆盖和滚动，**故意按窗口数推进，不调用正式 `CollectionTrial.scan_row` 的价格停止路径，也不调用收藏动作 `collect_selected`**。冻结 P90 C 规则上限仍为 230；观测从 300 开始到 798，记录中的候选均为不符合规则，并没有为了翻页测试修改真实任务价格。

[price_boundary_review.json](../artifacts/collection_scroll_speed/price_boundary_review.json) 分开保存了正式路径的离线回放：

| 回放层级 | 原配置与真实字段投影 | 实际结果 | 未发生的动作 |
|---|---|---|---|
| 正式 session 价格门 | 上限 230，记录帧识别价格 300 | `eligible=false`、`segment_finished=true`，后续滚动被 `price_limit_segment_finished` 跳过 | 点击/滚动 0，新 journal 0 |
| 正式 `scan_row` 调度 | 未修改该行规则；假后端省略导航 | 只检查 1 条，生成 `original_price_stop_boundary`，边界价 300 | 后续选卡 0，点击/滚动 0 |

这是**记录字段 + 假后端的正式价格路径回放通过**，没有在该回放中重新运行游戏，也不代表新一轮完整收藏已经跑通。正式收藏仍在首次完整识别到 `price > price_max` 后结束该规则段；`unobserved_tail_exhaustive=false`，不把未观察尾部算成已扫完。

### 最新实现保持哪些约束

1. `collection_scroll.py` 的实测 profile bank 按当前窗口逐一核对视口、滑块、卡片列形状、完整卡处理覆盖及首个未扫裁切边界，只选合适的最短实测位移。4 格和 5 格不是全程固定常数；无适用 profile 时仍明确停止，不静默退回单格重扫。
2. `windows_02` 暴露的右半卡缺左边问题已按真实观测边处理：先验证同帧完整卡作为列锚点，再用裁切卡**已观察的右边**唯一归列。搜索框的虚拟左边不再当作真实左沿；未观察的边仍为 false，半卡没有字段条、不变成可点击对象，原宽度/覆盖门槛保留。
3. 选卡后的就绪检查允许有界新帧重读。共同小位移分支要求多个非目标完整卡、两列/两行及所有可见成员一致证明至多 2 px 的位移；它只解释“继续读”，不重新点卡、不确认已选中、不放宽最终 `rebind_selected`，耗尽仍保留未决状态。
4. 逐卡识别、关注成功回读和未决输入对账不因翻页提速而取消；跨窗口不按旧坐标、价格相同或行号相同跳过当前复核。历史 confirmed 已改为只统计，不否决当前符合条件白星；新的同身份尝试另建 v2 attempt，尚未回执的 pending 仍防止同次点击盲重派发。该 v2 分支离线测试通过，本次 8 次实机收藏均为首次身份键，不宣称已实机覆盖 v2。翻页/就绪实现参考 [combined_readiness_partial_review.json](../artifacts/collection_scroll_speed/combined_readiness_partial_review.json) 与 [adaptive_shape_and_phase_review.json](../artifacts/collection_scroll_speed/adaptive_shape_and_phase_review.json)。

**适用范围仍是局部校准。** 已测对象包含 P90 天命成色 C 与 AUG 黑银先锋成色 S、`2560 × 1440` 客户区、列表 `[108,300,1765,903]`。旧 profile 保持 4 格 69–71 / 5 格 68–71 px 滑块范围，新增两条为实测 62 px 对应的 61–63 px 范围；每条仍分别校验内容位移、滑块位移和当前行边界。它不证明全部商品、全部 21 行、其他分辨率/DPI或其他列表长度均已校准。原 BBZPS 精确 wheel delta 尚未恢复；这里的档位都是本机独立测量，不是原程序常数。

### 业务断点和交付下一入口

最新正式进度以 `artifacts/collection_scroll_speed/white_star_run_02/summary.json` 的 **13/21** 为准。行 12 的 62 px 两档校准已补并在正式执行中通过翻页；当前记录的是行 14 AS Val 的滚动边界回读失败及保留的 pending geometry。白星/黄星新政策已实现，两段 8+9 次首次身份键收藏有实际回执，历史同身份白星 v2 追加仍只完成离线验证。修复后从新会话规则头恢复，不清旧 journal，不重发旧 lease。普通 UI“运行”仍未接完整实机 CLI，购买仍属于后续“我的关注”业务阶段。

本次源码收口后的发布验证入口如下；**本段只给入口，不预先声明命令已经通过**：

```powershell
Set-Location 'C:\Users\Administrator\Desktop\price'
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test -Package
.tools/ocr-runtime/Scripts/python.exe -B -X utf8 tests/release/verify_collection_scroll_speed_package.py verify
.tools/ocr-runtime/Scripts/python.exe -B -X utf8 tests/release/verify_collection_scroll_speed_package.py finalize
```

发布结论应以最后实际生成并重开的 `artifacts/collection_scroll_speed/VERIFICATION.txt`、`package_verification.json`、发布目录 `runtime_manifest.json` 及各命令退出状态为准。检查固定 EXE、DLL/`platforms`、发布 Python 与源码哈希一致、离屏 UI、SQLite、隔离副本回退；保持正式发布目录为修改后版本。此次文档更新没有执行构建、包验证或回退，也没有启动可见 UI 或游戏。

| Evidence | Finding | Path |
|---|---|---|
| `windows_03/result.json`、`session.json`、独立复核 | 本地 4 窗 20 条、3 次适选滚动及新帧回读通过 | 翻页模块验收；不改正式业务 checkpoint |
| `windows_04/result.json` | 默认首窗模式的新尝试因前台丢失而停止 | 保留失败，重新运行须取得新的整段前台租约 |
| `price_boundary_review.json` | 读到 798 是固定窗专项；正式价格门与调度回放保留超价停止 | `collect_selected` → `segment_finished` → `original_price_stop_boundary` |
| `white_star_live_review.json`、`white_star_run_01/summary.json` | 第一段新增 8 次金星回执，旧 189 条未改，推进到 12/21 | 保留第一段事实，不覆盖为第二段或 v2 分支成功 |
| `white_star_run_02/summary.json`、`session.json`、9 条实际新 journal | 第二段新增 9 次、13/21，旧 197 条未改、总 206，v2 实机仍 0 | 修复行 14 边界读回后新会话规则头恢复，保留旧 pending scroll 证据 |
| `probe_row12_delta480_retry2`、`probe_row12_delta600` 的测量与 profile | 实测 62 px 两档位移，4 条 bank 保留旧 2 条哈希 | 独立校准，不按比例外推，不泛化全部商品 |
| `collection_speed/confirmed_state_conflict.json` 与 2026-10-08 用户明确决定 | SR-25 9/21 是旧策略记录；confirmed 只统计的新规则已实现 | 保留历史，以当前新帧星色与配置决定动作，pending 仍先对账 |
| 本轮最终发布验证文件 | 最终包是否通过由实际命令和哈希决定 | 构建 → 离屏/存储/模块核对 → 隔离回退 → 重开证据 |

---

## 历史初版：以下“当前”仅指初版审计时点

下方保留最初的测量说明、`windows_01` 失败与发布设计。涉及“连续三窗仍待完成”的旧状态已由上方 `windows_03` 结果取代；其中关于模块交付、四件事务文件或验证动作的描述是验证设计，不能替代本轮最终实际文件。

更新日期：2026-10-08。本文记录本机收藏列表滚动修复，不把本机测量值当作 BBZPS 原常数，也不把离线测试通过当作实机完整收藏通过。

**当前验收边界：`-480` 和 `-600` 的独立实机位移测量已落证据；连续三个窗口的运行验收仍待完成。** 本文初版检查的 `windows_01/result.json` 只有第一个窗口的 6 张完整卡读数，随后以 `BATCH_STEP_FAILED` 停止并恢复 IDE。这次只读模块试验没有点星，没有执行购买，不代表整个收藏任务已完成。后续真实结果以独立的新运行目录及本事务最终 `live_result.json` 为准，旧失败记录保留。

## 先确认打开的是什么

前端固定路径：

```text
C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe
```

本轮实际收藏协调器的发布路径：

```text
C:\Users\Administrator\Desktop\price\dist\RelinkStudio\collection\run_collection_observed.py
```

**前端“运行”按钮尚未接入这套完整实机收藏 CLI。** 打开 EXE 查看界面，与启动发布版 Python 收藏协调器是两件事。本轮发布的是实际使用的 Python 模块副本；即使原生 EXE 的某次构建哈希不变，也必须证明发布 Python 文件与当前源码一致且相对冻结基线确有修改。

当前是依赖固定项目目录的交付方式，不是单目录便携 CLI。发布模块、Python 运行环境、OCR 模型和校准证据保持以下路径关系：

| 内容 | 相对 `C:\Users\Administrator\Desktop\price` 的位置 | 发布方式 |
|---|---|---|
| 前端与原生采集诊断入口 | `dist/RelinkStudio/RelinkStudio.exe` | 解压程序及 Qt DLL/plugin |
| 收藏协调器及依赖 | `dist/RelinkStudio/collection/*.py` | AST 递归收集本地 import 后逐文件复制 |
| Python 环境 | `.tools/ocr-runtime/Scripts/python.exe` | 使用现有项目环境，不重复复制 |
| OCR 模型 | `.tools/ocr-models/` | 使用现有三个固定哈希的模型，不重复复制 |
| 默认滚动校准库 | `artifacts/collection_scroll_speed/calibration_bank.json` | 保留原证据位置；发布 manifest 记录外置依赖哈希 |
| 校准 profile 与测量记录 | `artifacts/collection_scroll_speed/probe_02/`、`probe_03/` | 保留 profile → measurement 的原路径和哈希关联 |

开发模块通过 `tests/manual` 定位项目根；发布模块通过 `dist/RelinkStudio/collection` 定位同一根。`RELINK_PROJECT_ROOT` 可显式指定绝对根目录，但必须包含 `.tools/ocr-runtime`、`tests/manual`、`dist` 三个目录标记；相对路径或缺失标记不会被悄悄接受。

只读检查发布入口与资源，不启动游戏采集、模型或鼠标输入：

```powershell
$Root = 'C:\Users\Administrator\Desktop\price'
$Python = Join-Path $Root '.tools\ocr-runtime\Scripts\python.exe'
$Entry = Join-Path $Root 'dist\RelinkStudio\collection\run_collection_observed.py'
& $Python -B -X utf8 $Entry --verify-runtime-only
```

该命令验证根目录、EXE、Python、OCR worker 和三个模型。默认校准库的实际加载、逐级哈希与测量语义另由发布验收器的 `MODIFIED_RUNTIME_BANK` 命令验证；不把资源检查命令本身误写成校准实机验收。

## 为什么原来滚动后仍反复识别旧卡

旧实现每次发送 `delta=-120`。已有真实记录中的首格边界相位变化约为 146–147 px，小于约 274–275 px 的一行间距。原先底部半卡在这一格后仍不是完整卡，随后又从当前完整卡的第一张开始处理，因而出现大量重复识别。滑块确实移动，并不等于已经翻到有效的新窗口。

这个判断来自本机 `artifacts/collection_speed/run_03/session.json`、`run_04/session.json` 的动作与新帧记录，不是对 BBZPS 输入参数的猜测。原日志、配置和已有静态提取材料的复核见 [reference_review.md](../artifacts/collection_scroll_speed/reference_review.md)。其中原程序的“翻页继续查找”主要能对应商品目录循环，不能反推出卖单列表的 wheel delta。

## 本机实际测得的两档距离

| 实际输入 | 独立测量记录 | 左右列观测位移 | profile 接受区间 | 滑块位移区间 |
|---|---|---|---|---|
| `-480`，4 个 `-120` 单位 | `probe_02/measurement.json` | 589 / 587 px，约 588 px | 584–592 px | 29–31 px |
| `-600`，5 个 `-120` 单位 | `probe_03/measurement.json` | 735 / 733 px，约 734 px | 730–738 px | 36–39 px |

这些是独立发送对应批量输入后的测量，不是把单格结果乘以 4 或 5。测量在 `2560 × 1440` 客户区、列表区域 `[108,300,1765,903]` 中完成，记录包含前后帧 ID/hash、实际输入、两列参考候选的商品/成色/价格/磨损、各自观测帧及匹配后的边界位移。参考候选读数来自单独的参考观测帧，原始半卡帧本身并未提供完整候选字段；文件保留了这层区别。

**BBZPS 的原始一次滚动常数仍未知。** `-480`、`-600` 都是本机实测后建立的 profile，不称作“已恢复的 BBZPS 原值”。也未进行运行原程序的同环境速度对照，因此没有 BBZPS 加速倍数或整周期提速倍数结论。

两份 profile 的原始文件哈希：

```text
probe_02/calibration.json
7e74b8d340554e28114b3a749f259da0aa11478892da617b9f280bad70c6c424

probe_03/calibration.json
7e5c608320b82d60ce4c24494511b7921eb5d4caf60225a3e481dd947a08d112
```

## 校准库怎样选择，而不是固定猛滚

`calibration_bank.json` 绑定两份 profile 的精确文件哈希。每个 profile 又绑定测量记录的文件哈希。加载器逐级检查路径、大小、结构、哈希与测量语义；任何已存在但损坏的条目都会报错，不会被静默丢弃。

每次滚动前，协调器会：

1. 确认当前商品、成色、筛选以及新帧几何，收集这个窗口每张完整卡的已处理证据。
2. 为每个候选 profile 核对视口、卡宽/高、字段条、列位置、行距、滑轨及滑块高度。
3. 计算本窗口的实际覆盖边界：已处理完整行必须退出完整可选区域，首个未处理的裁切行必须保持可观察，不能跨过去漏扫。
4. 从满足全部条件的实测 profile 中选择最短的可用位移区间。并非永久选择 4 格，也并非永久选择 5 格。
5. 发送一次受限滚动，再采新帧验证位移、滑块和裁切边界；后续选卡必须绑定新帧坐标。

滚动后的坐标相同、价格相同或行号相同，都不是“还是同一挂单”的充分证据。跨窗口不会拿旧坐标或单纯几何位置当作挂单身份缓存。只读探测的 `read_only_probe` coverage 与实际收藏完成的 `collection_completed` coverage 分开，前者不生成收藏完成结论。

### 这两份 profile 的适用范围

| 条件 | 当前测量适用范围 | 不匹配时 |
|---|---|---|
| 客户区 / 列表区域 | `2560 × 1440` / `[108,300,1765,903]` | 拒绝该 profile |
| 滑轨 | `[1878,304,1,899]` | 拒绝该 profile |
| 滑块高度 | 4 格为 69–71 px；5 格为 68–71 px | 不按比例推算新位移 |
| 行距 | 270–280 px | 拒绝该 profile |
| 字段条高度 | 42 px | 拒绝该 profile |
| 商品列表长度及滚动相位 | 当前完整卡和未处理边界满足覆盖约束 | 选择另一个已实测 profile，或明确停止 |

因此，这个 bank 不是“全游戏、所有数量列表、所有分辨率都已校准”的声明。没有适用条目时会报告 `COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE` 及每个候选被拒绝的原因；不会自动退回单格慢扫来掩盖问题，也不会按滑块长度线性制造新 profile。

### 配置选择顺序

三个显式选项互斥：

| 参数 | 含义 |
|---|---|
| `--scroll-profile <json>` | 使用一份明确测量过的 profile |
| `--scroll-bank <json>` | 使用指定的哈希绑定校准库 |
| `--legacy-scroll` | 明确选择旧单格方式 |

没有显式选项时，程序查找项目内的 `artifacts/collection_scroll_speed/calibration_bank.json`；文件存在则必须完整加载成功。代码对不存在 bank 的旧环境保留 legacy 入口，但本次发布验收要求默认 bank 实际存在且被选中，不能靠缺文件悄悄变回旧模式通过交付。

## 如何验收实际滚动

本轮的目标是连续三个窗口，而不只是看到滑块动了一次。每个窗口需要保存完整卡读取记录、相对以前窗口的已观测身份重复数、覆盖边界、选中的实测 profile、实际滚动和新帧回读。三个窗口结束时，必须确认没有残留未确认输入，游戏回到后台且 IDE 恢复前台。

初次 `windows_01` 的实际状态：

| 项目 | 记录 |
|---|---|
| 模式 | `read_only_module_test` |
| 完成读数的窗口 | 1 个窗口，6 张完整卡 |
| 已发滚动 | `-480` |
| 结果 | `blocked` / `BATCH_STEP_FAILED` |
| IDE | `ide_restored=true` |
| 图像文件写入 | `0` |
| 收藏完成声明 | `false` |

这一记录要保留，不覆盖成成功。后续修复的三窗口结果与收藏业务验收必须各自记录；连续翻页识别通过不等于点星回执流程已经重跑通过。

完整收藏仍沿用典藏筛选 → 商品查找 → 标题复核 → 成色/价格/磨损联合匹配 → 添加关注 → 新帧成功回读。原关注和 prepared/dispatched/confirmed 账本保留；购买继续属于后续“我的关注”阶段。

## 发布与回退怎样证明交付的是新代码

`build.ps1 -Package` 使用 `package_collection_runtime.py` 读取 AST，递归收集入口的本地 import，包括函数内的 `navigate_lobby_to_warehouse` 导入。目前闭包为 16 个 Python 模块；打包过程中不导入这些应用模块，不启动游戏，不复制模型。`runtime_manifest.json` 同时列出源码映射、逐文件哈希及默认 bank 的完整外置文件依赖。

在最终源码定稿并重新构建后执行：

```powershell
Set-Location 'C:\Users\Administrator\Desktop\price'
.tools/ocr-runtime/Scripts/python.exe -B -X utf8 tests/release/verify_collection_scroll_speed_package.py verify
.tools/ocr-runtime/Scripts/python.exe -B -X utf8 tests/release/verify_collection_scroll_speed_package.py finalize
```

验证器核查模块集合没有缺失或多余、发布字节与源码一致、固定原生包依赖齐全、发布入口实际解析到正确项目根、默认 bank 的加载语义成立。离屏界面、SQLite 和离线合同测试分别记录，原生 EXE 是否变化与 Python 模块是否变化分别记录。

回退在独立的发布副本上测试，恢复冻结的原程序文件，并移除本次新加入的受管 Python 文件；用户 JSON 和 SQLite 哨兵保留。回退不删除游戏关注，不修改外置用户配置，不把源代码基线测试通过误称为已经恢复了一次实机业务。

四件事务文件位于 `artifacts/collection_scroll_speed/`：`MODIFIED_FILE.py`、`DIFF_FILE`、`VERIFICATION.txt`、`ROLLBACK.sh`。`VERIFICATION.txt` 保存全部实际命令、输入、原始输出和退出状态，早先失败的尝试也保留；发布目录保持修改后版本。

## Evidence → Finding → Path

| 证据 | 可以支持的发现 | 实现/验收路径 |
|---|---|---|
| `reference_review.json`、`reference_review.md` | BBZPS 原滚轮常数没有在已检查资料中恢复；旧本机单格未露出新完整底行 | 不伪造原值；改为本机独立测量 |
| `probe_02/measurement.json`，SHA256 `f86c384c25080564175c74ec53262bbcba110f7d054cac58890b07d77e0d6f53` | 本次 `-480` 两列位移 589/587 px | `probe_02/calibration.json` → bank 候选 |
| `probe_03/measurement.json`，SHA256 `cfea9dbd1ed00cf931ae8ee5f6d9fb47629a8b5d01adf76125e8af93d904c1d9` | 本次 `-600` 两列位移 735/733 px | `probe_03/calibration.json` → bank 候选 |
| `test_collection_scroll_adaptive_bank.py`、`test_collection_scroll_bank.py` | 选择、路径、哈希、语义和不伪造 fallback 的离线合同 | `select_scroll_configuration` → `select_window_scroll_calibration` → `scroll_plan` → 新帧 `rebind_after_scroll` |
| `windows_01/result.json`、`session.json` | 初次连续窗口试验仍失败，已回 IDE | 保留失败记录，修复后在新目录继续三窗口验收 |
| `runtime_manifest.json`、事务 `commands.json` | 实际交付代码/依赖与离屏测试事实 | 发布验证 → 独立副本回退 → reopen 四件事务文件 |

以上内部证据均可从项目目录重新读取；游戏像素只在运行内存中，本报告不补造或保存游戏截图。此文不改写主 checkpoint；业务进度由协调器维护。
