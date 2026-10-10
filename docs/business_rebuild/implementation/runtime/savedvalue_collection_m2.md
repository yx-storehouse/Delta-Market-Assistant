# 使用 savedValue 任务执行收藏

> **后续实机更新（2026-10-07）：** 当前配置冻结与连续协调器试段已完成本轮17次确认新增；优化后最后观察为 AUG 天命 C 第六卡金星，在动态滚动缺口停止。最新事实见 [收藏半程实机记录](../../../COLLECTION_ONLY_TRIAL.md) 和 checkpoint。下文“空关注”校正是此次试跑之前的现场，旧坐标均须重新观察，历史累计42次不等于当前关注数。

2026-10-07。本轮按用户提供的任务数据实际执行“查找商品 → 成色筛选 → 同帧价格/磨损联合匹配 → 加入我的关注 → 读取结果”。不是只读演示；购买业务仍属于后续我的关注阶段。

**现场校正（2026-10-07）：最新独立观察为“空关注”，当前 `cursor_valid=false`；见 `artifacts/m2_savedvalue_collection/current_checkpoint.json` 和 `artifacts/collection_workflow/watchlist_current_confirm.json`。原 25 次已确认收藏是历史动作数，不是当前关注数量。恢复时按原空关注路线重新定位，不沿用旧 P90 卡位。完整流程缺口和慢速根因见 [收藏对齐与性能审计](../../11_bbzps_collection_alignment_performance.md)。**

历史暂停点：曾确认新增 25 个关注卖单（AUG 天命 10 个，P90 天命 15 个），停在 P90 天命成色 B 的第 1 页、第 6 张完整卡片。底部半行和滚动字段尚未覆盖，原停止码为 `COLLECTION_UNSCANNED_SCROLL_REGION`。该历史记录没有把剩余任务标为完成，也不代表现在仍在这个页面。

当前数量与每次尝试以 `artifacts/m2_savedvalue_collection/session_audit.json`、`journal/` 和各 `live_*.json` 为准。失败记录不会被后续成功覆盖。普通桌面 UI 仍是配置/回放前端；实际操作来自显式启动的人工联调驱动，不代表 UI 的完整执行器已经接通。

## 输入与原业务对应

- 输入 `S11新赛季0925.savedValue`：8,926 字节，SHA-256 `cff452bcd4b77a17fb3cc9a23f55e0131304098f7ac0057205f619bbd4406253`。原文件不改写，原始任务与解码快照保存在本地忽略目录 `artifacts/m2_savedvalue_collection/input/`。
- 这是结构化二进制配置，不是 UTF-16 文本。`savedvalue_reader.cpp` 只解释已经观察到的布尔、无符号整数、UTF-8 字符串、数组和映射；深度、字节、节点、容器、重复键与行号都有边界检查。
- 文件中有 12 个小整数沿用了两字节前缀（第一个在偏移 3556，值 230）。最小宽度重编码会缩短 12 字节，不能误报为原字节往返。解码器保留这些非最短前缀的偏移/宽度，测试据此重新编码并逐字节比较；首次未保留宽度而失败的验收记录继续保留。
- 任务部分 350 个键值对，对应 50 行 × 7 字段；21 行启用，共 9 个商品。按数值行号遍历，不能把 `_10` 排在 `_2` 前面。
- 当前启用行的最低价均为 10，最大磨损为 5，限量原值均为 0。原 UI 提示将 0 定义为不限数量；没有明确的拥有/品阶选择时保留 `any`，不根据图标颜色猜条件。
- 产品字典来自既有原程序转储中公开下拉选项的静态数据：偏移 `0x1071e04`，30,894 字节 UTF-16LE 片段，哈希 `6b9a859568f078cd4088bc94dc60fb4ce5f9ccfbc18f3f63c693a967799085ba`。只读取转储字节，没有执行原程序、插件或历史探针。
- 片段并非完整合法 XML。提取器只读取独立闭合的 `combo_group` 及成色选项，不静默修复整个文档。字典含 149 个商品、5 个成色选项。

| 原行号 | 商品 | 条件和最高价 |
|---|---|---|
| 0–3 | AUG突击步枪-天命 | S 600 / A 400 / B 280 / C 230 |
| 4–7 | P90冲锋枪-天命 | S 600 / A 400 / B 280 / C 230 |
| 8–11 | SR-25射手步枪-天命 | S 600 / A 400 / B 280 / C 230 |
| 12、14、16 | AUG / ASVal / P90 黑银先锋 | S 300 |
| 18–19、20–21、22–23 | K437 / 勇士 / AWM 私人定制 | S 330 / A 260 |

原流程定位见 [首次启动重建 S20–S29](../../10_bbzps_first_startup_reconstruction.md)。遇到高于当前阈值的卖单后，沿原日志结束当前成色扫描段，再进入下一成色。记录将其标为 `original_price_stop_boundary`，不会把未观察的后续页假报为逐单穷尽；“默认排序”的全局单调性仍是原始证据缺口。

## 模块与运行方式

| 模块 | 职责 |
|---|---|
| `src/config/savedvalue_reader.*` | 有界数据解码、明确字典映射、行顺序和未知 ID 阻断 |
| `src/diagnostics/savedvalue_preview.*` | 无可见窗口的任务预览 CLI |
| `src/diagnostics/live_capture_check.cpp` | DXGI 内存帧、当前页、限定字段 OCR、选中边框和星标颜色 |
| `tests/manual/collection_labels.py` | 同帧精确名称/位置匹配，不模糊改写中文名称 |
| `tests/manual/collection_candidate.py` | 商品、所选卡片、成色、价格、磨损关联与 Decimal 阈值判断 |
| `tests/manual/collection_journal.py` | 输入前持久化、待确认状态、禁止重发星标切换 |
| `tests/manual/run_foreground_batch.py` | 一次激活、批次内连续操作、finally 一次恢复 IDE |
| `tests/manual/build_collection_plan.py` | 从已解析任务生成有限、可核查的成色/卡片操作计划 |
| `tests/manual/run_collection_rows.py` | 顺序推进同赛季任务，首次异常停止；不自动重试变更 |
| `tests/manual/reconcile_collection_attempt.py` | 用新观察对账既有待确认动作，完全不发送点击 |

数据预览（不会显示窗口或操作游戏）：

```powershell
dist\RelinkStudio\RelinkStudio.exe --savedvalue-preview C:\Users\Administrator\Desktop\S11新赛季0925.savedValue
```

纯离线回归：

```powershell
python -X utf8 -m unittest discover -s tests/manual -p test_collection*.py -v
```

实机计划是有状态的，不能把文档中的旧添加计划直接再跑一遍。恢复前先核对最新页、任务行、卡片位置和 journal；曾经点击但缺回执的条目必须先对账，禁止重试星标。

## 字段与回执契约

1. 当前校准为 2560×1440、144 DPI。页类、ROI、选中卡片四边、标题、价格和右侧详情必须来自同一个帧哈希；不拼接不同时间的页面。
2. 卖单索引 0–5 为两列三行中完全可见的卡片。底部半行和滚动后的新布局尚不由此几何直接覆盖；需要它们时驱动停止，不能跨过去并声称整页完成。
   亮色第三行的整条字段 OCR 曾返回空数组；现在仅在空结果时，对同帧成色和价格两个独立区域分别重读。`live_series_s6_v5/live_1.json` 已实测恢复 P90 A 第三行的 408 高价边界，未猜测空 OCR 内容。
3. 价格必须构成右对齐的完整连续数字串。货币图案的微小前置逗点与数字分开；脱离的数字仍视为歧义，不擅自丢掉可能的高位数。
4. 磨损必须实际观察到小数点。中文 OCR 缺点时使用同帧放大英文数值 ROI；若英文只给出数字后缀，可与相邻中文 `A(` 等实际前缀框组合。两个完整读数矛盾时停止，不按固定六位数猜小数点。
5. `collect_selected` 在确认规则命中、白星标、新鲜度和回读时间预算后，先以独占文件和 fsync 写入 `prepared`，再发送星标点击，随后记录 `dispatched`。输入部分送达记为 `input_uncertain`。
6. 回执需要同一商品/成色/价格/磨损的新帧和金色星标。成功文字与金星可确认；若短暂提示未显示，已持久化的同一卖单“白星 → 金星”变化也可确认，分别标记回执类型。
7. 仅看到金星但缺少本次变更的完整前后绑定时，不直接伪造普通回执；保留原失败，使用独立 `confirmed_reconciliation` 记录。确认后禁止重复点击，防止取消关注。
8. 收藏诊断帧龄上限为 5,000 ms；原正式运行时/购买相关的新鲜度契约没有放宽。批次最多 20 步、30 秒，动作前至少预留 7 秒做回读。
9. 原有关注不清空。游戏图像只在内存/标准输出中流转；本地保存的是字段、帧哈希、动作与结果 JSON，不是游戏图片。离屏 UI 验收图片与游戏采集明确分开。

## 已发现的异常及证据

| 证据 | 事实 | 处理 |
|---|---|---|
| `live_first_add.json`、`first_add_reconciliation.json` | 首条 AUG 实际变成金星；原自动提示词断言失败 | 保留失败；九个中文单字超过通用八 token 上限，只将中央提示匹配上限增到 16；另行对账，不再次点星 |
| `live_aug_b_remaining.json`、`b_add_reconciliation.json` | B 条目成功后，货币图案残点打断金额字段匹配 | 修复严格金额分组；回读异常也保存整帧字段；通过独立观察对账 |
| `live_p90_s_resume*.json` | 大号 P90 标题的中文 OCR 漏掉 90；原色及英文局部试验未解决 | 保留自动标题失败；本轮仅在已查看截图后人工批准进入卖单页面，收藏前仍强制完整卖单标题匹配，不宣称大号标题自动识别通过 |
| `live_series_s6/live_3.json`、`live_series_s6_v2/live_1.json` | A 磨损的小数点丢失 | 保留失败；同帧英文数值 ROI 实际读到 `0.426873)`，与实际中文前缀组合后通过 |
| `live_series_s6_v3/live_1.json`、`p90_a_add_reconciliation.json` | 短暂成功提示缺失，但同一卖单已经金星 | 不重复切换；对账后增加有前后绑定的颜色变化回执，不再把提示文字当唯一成功依据 |

## 验收与继续开发

固定发布目录仍为 `C:\Users\Administrator\Desktop\price\dist\RelinkStudio`，保留 DLL、`platforms`、`sqldrivers` 和 OCR helper。白灰 Win11 UI 不改布局。验收脚本记录基线、当前包、隔离副本回退及恢复后的原行为；回退程序包不撤销游戏中的收藏，且正式发布目录保持修改后版本。

```powershell
python -X utf8 tests/release/verify_savedvalue_collection_package.py build
python -X utf8 tests/release/verify_savedvalue_collection_package.py verify
python -X utf8 tests/release/verify_savedvalue_collection_package.py finalize
```

继续开发仍需处理：底部半行/列表滚动后的卡片几何、默认排序的稳定性证据、大号标题独立可靠识别、字典 `疾风魅影` 与当前菜单 `疾光魅影` 的版本差异，以及把联调驱动接入正式异步执行器。这些都不是用测试通过数替代的业务完成状态。

### 接续提示

```text
读取 SESSION_START.md、savedvalue_collection_m2.md 和本轮 session_audit.json。
先核对最新游戏页、当前任务行、卡片索引和 journal 待确认项。
继承原任务顺序与已有关注；所有已确认金星都保持不动。
逐步修复真实阻塞并继续收藏；不要重放旧添加计划，不输出例行进度。
批次结束将游戏留在后台、IDE 恢复前台。只在实际卡住时简短告知用户。
```
