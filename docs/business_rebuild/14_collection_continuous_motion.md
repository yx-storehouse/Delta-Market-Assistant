# 连续收藏轨迹时序

日期：2026-10-08。

## 新增 profile

`tests/manual/cursor_motion.py` 增加显式 `collection_continuous`，没有改变原默认项。

| profile | 最小计划时长 | 最大计划时长 | 距离除数 |
|---|---:|---:|---:|
| standard（原默认） | 120 ms | 280 ms | 10 |
| collection_fast（原收藏） | 60 ms | 120 ms | 25 |
| collection_continuous（新增） | 30 ms | 60 ms | 50 |

非零距离的默认计划公式为 `min(maximum, minimum + distance / distance_divisor)`。相同起终点下，新增 profile 的默认计划时长恰为 collection_fast 的一半。零距离仍为一个点、0 ms，并检查前台和输入状态。

```python
from cursor_motion import plan_motion

previous = plan_motion((10, 20), (2000, 1000), profile='collection_fast')
continuous = plan_motion((10, 20), (2000, 1000), profile='collection_continuous')
assert continuous.duration_ms == previous.duration_ms / 2
assert continuous.control_points == previous.control_points
assert continuous.points[-1].position == (2000, 1000)
```

曲线仍为同一组三次 Bézier 控制点和 smoothstep 缓动。缩短时长后采样点数减少，不表示逐个采样点都相同；最大**计划**检查间隔仍为 12 ms。真实 OS 调度可能晚于计划，所以不声称硬实时或实际用时恒定减半。

## 执行约束不变

- 起点/边界/精确终点校验保留。
- 前台、用户按键/遮挡、外部鼠标移动检查保留，每次等待和更新前后都检查。
- 干扰、失焦、终点偏移或执行超时立即停止，不点击、不重试，不覆盖用户的新位置。
- 原标准及 collection_fast 默认/显式计划与冻结基线逐字段一致。
- planner/executor 模块只移动，不自行点击；Session 仍先验证当前候选和 frame/lease，再决定是否派发点击，成功回读保留。

## 主流程集成只读核对

`MemoryReviewBackend` 仅在 `fast_capture and fast_actions` 时设 `fast_collection_motion=True` 和 `collection_motion_profile='collection_continuous'`。

`ForegroundSession` 仍需要 `fast_settle=True`、动作属于 `select_visible_card/collect_selected` 且 backend fast gate 才调用 `fast_collection_click`。

`WindowsBackend._move_screen` 只在 `purpose='collection_click'` 且 fast gate 为真时选择该字段；没有该字段时回退原 collection_fast。普通 click、scroll、hover、neutral_pointer 和 restore 保持 standard。

只读假后端集成探针覆盖 24 个 purpose/gate/profile-field 组合及 4 个 backend 参数组合，28 个全通过；不调用真实鼠标或窗口 API。记录：`artifacts/collection_local_hotpath/continuous_motion_integration_review.json`。

## 离线验证

新增 `tests/manual/test_collection_continuous_motion.py` 11 项，覆盖长短/反向/负坐标、准确半时长、相同控制点、12 ms 计划间隔、精确终点、干扰/前台/输入检查、零距离、非法参数及旧 profile 基线一致。

```powershell
# cwd: C:\Users\Administrator\Desktop\price\tests\manual
C:\Users\Administrator\Desktop\price\.tools\ocr-runtime\Scripts\python.exe -m unittest test_collection_continuous_motion test_cursor_motion test_collection_action_speed -v
```

实际字面结果：

```text
Ran 52 tests in 0.531s

OK
```

退出状态 `0`。所有光标/时钟/前台均为注入假的回调，没有实机测试。完整记录在 `artifacts/collection_local_hotpath/continuous_motion_tests.json` 和 `.txt`。
