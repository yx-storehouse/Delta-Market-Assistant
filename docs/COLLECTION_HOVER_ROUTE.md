# 收藏后停顿：绕开悬浮说明区域（2026-10-08）

**当前状态：新版实机 run01 已完成，IDE 已恢复。** 用户“可以开始吧”授权的这一批次已执行完毕，不再等待或重复同一批次。用户随后指定下次改测 S11 系列；新批次需要前台时仍先协调。解压目录仍为 `C:\Users\Administrator\Desktop\price\dist\RelinkStudio`，普通 UI 的“运行”没有在本轮新增接通真实收藏 CLI。

## 最新实机结果

run01 使用发布目录的执行链，完成原指定 0–7 共 8 条规则，25 个候选，新增 10 次收藏、保留 7 个黄星、8 个超价停止边界，61.141 秒。它是优化试段，不是重新跑完 21 条规则。前置和恢复各一次，未决动作为零。

| 记录指标 | 上一版 run07 | 本版 run01 |
|---|---:|---:|
| 收藏回执数 | 15 | 10 |
| 局部回执后还需完整布局识别 | 7 / 15 | 0 / 10 |
| 点星 → 下一卡中位数 | 360 ms | 344 ms |
| 点星 → 下一卡全部范围 | 343–1110 ms | 328–532 ms |
| 点星 → 下一卡超过 800 ms | 7 / 15 | 0 / 10 |
| 点星 → 回执中位数 | 282 ms | 266 ms |
| 选卡 → 点星中位数 | 625 ms（8 次直接链） | 640 ms（7 次直接链） |

本次 27 次实际选卡/点星移动中，25 次执行绕行，2 次沿用本就不穿过详情区的原曲线；起点在详情区内的降级分支为零。逐条复算确认终点不变、计划预算与冻结旧版相同、绕行的每段线段均在区域之外。10 次回执都只采集一次完整布局，记录中未再读到该区域的成色说明文字。**这是不同实时列表的观测比较，支持减少这条长停顿分支；不是受控 A/B，也不代表所有场景永不遮挡或整体速度翻倍。** 选卡后的识别链仍偏慢，17 次选卡读取各包含 3 次尝试和合计 70 ms 动态重读等待；没有将它冒充固定点击延迟或删除现有检查。

冻结业务函数逐条回放 10 次商品/成色/价格/磨损/几何/白星→金星成功回执通过。原 293 条 journal 字节不变，新增 10 条后为 303（297 confirmed + 6 reconciliation）；配置哈希不变。四 ROI 当前语义路径 26 / 27 成功，1 次同帧 full fallback；条件边线复读本次仍为零。没有购买或游戏图片落盘。

结束恢复 IDE 成功；恢复鼠标原位置时记录了 `CURSOR_INTERFERENCE`，原异常字段保留，不标成鼠标恢复成功，不因此重放已完成收藏。

证据链：`run01/session.json` 与逐步文件 → `analyze_run01_continuous_speed.py` 的同 Python monotonic 计时 → `audit_run01.py` 的冻结业务/路径/账本回放 → `run01/offline_receipt_audit.json` 与 `live_result.json`。

## 这次定位到的具体原因

上一版 run07 中，7 次收藏后进入 `receipt_only`，星标回执成功后还要额外完整识别一次，星→下一卡达 875–1110 ms。原始数值 profile 表明：

- 不是白色滑块消失。7 帧的真实滑块仍在 y304 起始的一段高对比区域里。
- 滑轨中部的几十到约一百像素被覆盖，检测器选中的连续轨道片段从 y450 或 y739–746 开始，因而片段内找不到顶部滑块。
- 其中 5 帧的当前 OCR 明确读到成色参数、外观表现等悬浮说明文字；另 2 帧有上部覆盖，但没有把未识别文字猜成某一种提示。
- 这 7 次点星前的原轨迹全部穿过右侧详情交互区。轨迹与提示遮挡的因果关系最初属于有记录支持的推断；本次 run01 的避让和回读结果进一步支持该定位，但不将非受控实机比较写成严格因果证明。

证据链：`run07/session.json` 原始记录 → `analyze_hover_obstruction.py` 原阈值数值复算与旧轨迹回放 → `hover_obstruction_evidence.json` 的 7 条逐帧结果 → 新轨迹规划和 fake-cursor 测试。

本轮没有放宽滑块阈值，也没有把被遮挡的滑轨补成连续线。原检测器和回执后完整布局要求均保持。

## 实现

只有 `collection_continuous`、已验证的收藏/选卡输入、当前客户区为 2560×1440 时启用右侧详情区避让。区域是客户区物理像素 `[1900,350,2449,1295]`，每次都按当前 `ClientToScreen` 原点转换，不把显示器原点假定为零。

1. 起点是实际当前鼠标位置，终点仍是原来通过业务检查的真实输入点。
2. 原三次曲线的控制凸包完全避开该区域时，保留原曲线。
3. 可能进入区域时，通过带 16 px 间距的矩形外侧可见节点选择短路线；每一段连续线段都检查与区域的交集，不只检查稀疏采样点。
4. 路线由分段三次缓动组成，所有转点均显式采样。**共用原来的 30–60 ms 总预算**，不是每段各加一次延迟，也没有中途固定停顿。
5. 最多 12 ms 的检查间隔、用户鼠标/按键干扰、前台归属、准确终点、提交前配置/帧龄核验照旧。
6. 如果原导航已经把鼠标留在这个区域内，记录该情况并使用原来受检查的移动，不伪称全程避让。任何实际截图仍按原识别门槛处理。

普通导航、滚动、neutral、restore 路线不使用避让参数。价格/磨损/成色匹配、白星收藏、黄星保留、历史仅统计、未决动作不重复派发、成功回读和超价停止均未修改。

附带修正窗口枚举：使用 `%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe` 的绝对路径，避免上次 package-only PATH 中找不到 PowerShell 的测试启动失败。最终 IDE 返回窗口的 PID 和客户区核验不变。

## 可复现离线验证

```powershell
.tools/ocr-runtime/Scripts/python.exe -B -X utf8 artifacts/collection_scrollbar_continuity/analyze_hover_obstruction.py
.tools/ocr-runtime/Scripts/python.exe -B -X utf8 -m unittest discover -s tests/manual -p test_collection_hover_route.py -v
.tools/ocr-runtime/Scripts/python.exe -B -X utf8 tests/release/verify_collection_scrollbar_continuity_package.py verify
```

已观察到的结果：

| 验证 | 实际结果 |
|---|---|
| 7 次旧慢分支轨迹回放 | 全部旧轨迹穿过详情区，新计划全部绕开，计划总时长不增加 |
| 新避让单测 | 16 项通过，覆盖负显示器原点、两列多行正反方向、边界相切、无可达路径、旧 profile 不变、篡改计划、干扰停机及后端启用条件 |
| 收藏 Python 回归 | 681 项通过 |
| 原基础轨迹 / foreground / packaging | 35 / 13 / 7 项通过 |
| Native 构建与测试 | 41 CTest 通过 |
| 发布依赖 | 17 个实际 Python 模块，AST 闭包包括新 `collection_motion_route.py` |
| 离屏 UI / SQLite / 隔离回滚 | 通过，回滚仅作用于副本，新增受管模块移除；正式目录保留修改态 |
| 基线 Python 测试 | 冻结树实际 611 项，18 skipped，不能说与项目当前完整回归数量相同 |

本轮原生 EXE SHA256 仍为 `c733a612eb1c7f15570cf291e14abc82361cf865c107e5a0b33fcdc4f1dc90ac`；产品变化在发布目录中的 Python 执行链。`collection_live_session.py` 已发布 SHA256 为 `b29df9957c7dac73f24fbb719af14fa7156ef60669e34ab0512d97b59087361b`。

## 数据与下一次实机

原 21/21 完成来源保留，run07 和本次 run01 各自作为后续优化试段，不重复计账。`current_checkpoint.json` 的最新速度试段已指向本次回放证据，历史账更新到 303。本次并未进行滚动，不能从这一批证明多页连续收藏提速。

用户在本次测试后明确要求下次使用 **S11 系列**，旧的天命列表条目少，不适合观察连续速度。当前真实配置的 S11 黑银先锋 AUG / AS Val / P90 已启用行号为 **12、14、16**，成色均为 S，价格上限均为 300，磨损上限均为 5。后续继续采用当前配置，不为测速提高价格或磨损上限；列表实际数量由新画面确认，不猜测库存。

`next_live_selection.json` 已保存这项选择和尚未执行的下一批命令。新批次应新建 run02，不覆盖 run01，也不采用脚本原默认的 0/1 行；整段仅前置/恢复各一次，运行前重新核验当前配置哈希和 S11 行映射。重点测连续选卡→点星、星→下一卡、翻页后衔接及尾部停顿；本次先不自动占用前台重跑。

只读复核命令：

```powershell
.tools/ocr-runtime/Scripts/python.exe -B -X utf8 artifacts/collection_scrollbar_continuity/analyze_run01_continuous_speed.py
.tools/ocr-runtime/Scripts/python.exe -B -X utf8 artifacts/collection_scrollbar_continuity/audit_run01.py
```

本事务四件和逐条命令、输入、字面输出、退出状态位于 `artifacts/collection_scrollbar_continuity`。旧 `collection_local_hotpath` 四件不覆盖。
