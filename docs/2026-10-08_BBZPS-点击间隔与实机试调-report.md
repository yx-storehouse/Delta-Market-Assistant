# BBZPS 点击间隔核查与收藏试调

## 本轮结果

2026-10-08，用户要求先核查 BBZPS 延迟、调整后实际看收藏节奏。本轮静态读取了原配置和收藏日志，没有执行原程序或加载其 DLL。原配置存在多个不同节的时间字段，收藏点星究竟读取哪一个、其单位怎样换算，主调用点仍未恢复。本轮没有把猜测映射写成原程序参数，而是只取消自研收藏动作之后额外的固定等待，保留轨迹、当前画面判断、回执与统计。

实际执行了当前配置的 AUG/P90 共 8 条成色规则：23 个候选，新增收藏 9 个，保留已关注 6 个，8 条规则均完成；耗时 60.828 秒。整段仅前置游戏一次、结束恢复 IDE 一次，两个 pending 均为空。该结果不代表本轮重新完成全部 21 条规则，也不代表已经完成 BBZPS 局部识别路径重构。

范围沿用 `../artifacts/bbzps_collection_hotpath/scope.md`，本报告 flavor=null；本轮只分析收藏性能，不延伸既有载荷研判。

## 1. 原配置实际值

来源：`BBZPS/config.ini`，SHA-256：

`f55ec11eddabce027303e417fec574a72f75c34f3977e5e809c8e2255c181cc4`

| 配置节 | 字段 | 原值 |
|---|---|---|
| GlobalSettings | addtime | 0.1 |
| GlobalSettings | clicktime | 0.0005 |
| SkinTasks | click_mode | 轨迹运行 |
| SkinTasks | clickinterval | 0.005 |
| SkinTasks | clickinterval2 | 0.05 |
| SkinTasks | clicktime | 0.847 |
| SkinTasks | clicktime2 | 0.857 |
| SkinTasks | clicknum | 3 |
| SkinTasks | parameter | 0.005 |
| SkinTasks | autoparameter | True |

数值是原始配置文本，不附加未经确认的毫秒/秒单位。尤其两个节中的 `clicktime` 值不相同，不能只按键名混用。历史 `已点击二次确认，本次延迟…ms` 出现在购买流程，不是收藏 mouse-down 时间戳。敏感配置字段没有复制进本报告。

可重查摘录：`../artifacts/collection_click_wait/bbzps_click_settings.json`。

## 2. 原收藏顺序证据

此次复核说明见 `../artifacts/bbzps_source_alignment/log_sequence_review.md`；其中 197 条摘录均附原行、字节定位与哈希。

- `BBZ_20260924_090240.log:295–331`：连续四卡只记录价格小区域与右侧详情区域；未逐项重读全屏、商品标题或排序控件。
- 同文件 `86–97`：价格匹配 → 详情/磨损 → 联合匹配 → 下一价格超价结束 → 成功关注提示出现在后续控件读取中。
- 同文件 `116198–116215`：底行两卡匹配后重新读取列表区域，再继续顶行价格；鼠标滚动调用及滚动量未记录。
- 明确超价可以提前结束；一般价格失败不能与超价混淆，全部日志中仍有 6 条价格 False/磨损 True。

本次修正旧统计：双 True 总计 **18,081**，旧 18,079 条的统计遗漏了 2 条科学计数法磨损。旧 96 ms 中位数只覆盖原已配对的 18,079 个区间，且不是点击延迟。

## 3. 这次实际调整

分支：`main`。实际字段位于 `tests/manual/collection_live_session.py` 的动态动作等待表。

| 项目 | 冻结基线 | 当前发布 |
|---|---:|---:|
| 选卡输入返回后的固定等待 | 120 ms | **0 ms** |
| 点星输入返回后的固定等待 | 80 ms | **0 ms** |
| 滚动后的固定等待 | 180 ms | 180 ms，不变 |
| 收藏专用曲线规划时长 | 60–120 ms | 不变 |
| 普通导航等待 | 600 ms | 不变 |

0 ms 表示取消主动 sleep，**不表示选卡或收藏已经即时成功**。下一步仍必须采到输入之后的新帧，选卡通过原 selected-geometry rebind，收藏通过当前同物品白星→金星/成功提示核验；未就绪时只做有界重读，绝不重发点星来提速。滚动、普通导航和默认非快速模式均未调整。

该策略名为 `immediate_fresh_readback_v2`。这是自研试调，不是已恢复的 BBZPS `clickinterval` 映射。

## 4. 实际计时，不混用指标

计时来源：`../artifacts/collection_click_wait/timing_comparison.json`；时间点是 `SendInput` API 调用边界，不是物理硬件到达时刻。

| 指标 | 上一实机 | 本轮实机 |
|---|---:|---:|
| 选卡 API 返回→点星 API 开始 | 7 次，中位 **797 ms** | 7 次，中位 **797 ms** |
| 点星 API 返回→成功回读 | 11 次，中位 766 ms | 9 次，中位 **344 ms** |
| 点星步骤开始→成功回读 | 11 次，中位 968 ms | 9 次，中位 547 ms |
| 扫描候选 | 21 | 23 |
| 新增收藏 | 11 | 9 |
| 保留黄星 | 2 | 6 |
| 8 条规则总耗时 | 63.454 秒 | 60.828 秒 |

两次市场内容、黄星比例与样本集合不同，不是随机同挂单 A/B，不据此给出整体加速百分比。实验表明：本轮点星后的回读间隔缩短；用户关注的选中后到点星这一段，取消 120 ms 固定等待后中位数仍为 797 ms，不能继续宣称仅剩点击延迟。

当前源码仍含逐帧 full OCR/页面分类、native 详情先于 local 价格等路径，定位见 `../artifacts/bbzps_source_alignment/current_path_review.md`。它们与原日志的局部内循环存在差别；本次没有混入尚未验证的上下文缓存或伪造完整页面覆盖。

## 5. Evidence → Finding → Path

| Evidence | Finding | 本轮实际动作 |
|---|---|---|
| E1：原 config 哈希与白名单字段摘录 | 原值可核对，收藏调用映射仍缺失 | 保留原文件；不把抢购字段映射到收藏 |
| E2：197 原行/95,961 热路径定位复核 | 稳定内循环以价格、详情局部读取为主 | 把真实顺序与未恢复输入调用分开记录 |
| E3：冻结源码及当前发布副本 | 自研额外 120/80 ms sleep 确实存在 | 改为立即请求新帧，不改变原判定/回执 |
| E4：run01 的逐动作、回执、journal | 9 次真实收藏确认，页面未就绪不能重复点星 | 原账本保留，新增记录单独审计 |
| E5：两轮 SendInput 边界计时 | 选卡→点星中位仍 797 ms | 不将取消固定等待吹成全链路等同原程序 |

## 6. 复验和交付

实际解压程序保持：

`C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`

本次改动交付在同目录的 `collection/collection_live_session.py`；native EXE 无需改变字节。CLI 已实际运行，不宣称前端“运行”按钮新增了尚未接入的调用。

完整命令、原始输出、退出码、基线哈希、发布副本、差异与隔离副本回滚证明记录在 `../artifacts/collection_click_wait/VERIFICATION.txt`。基线第一次 timing 测试缺少冻结目录的资源根标记，明确记录失败；随后设置真实 `RELINK_PROJECT_ROOT` 后仅运行 FakeBackend 单测，通过，未改冻结源码。

复验入口：

```powershell
.tools/ocr-runtime/Scripts/python.exe -B -X utf8 tests/release/verify_collection_click_wait_package.py verify
```

回滚只作用于显式指定的程序副本，不清除游戏关注，也不回写用户配置。详细实机结果在 `../artifacts/collection_click_wait/run01/result.json`。
