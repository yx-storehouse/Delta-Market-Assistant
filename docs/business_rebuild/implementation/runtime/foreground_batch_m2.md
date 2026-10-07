# M2 实机联调：整段操作只切换一次前台

> 后续市场页识别和有界Unknown重读已经更新，见 [市场实机校准](market_page_calibration_m2.md)。下文“曼德尔页仍Unknown”是本模块初次验证时的状态，现已解决；批次前台原则不变。

日期：2026-10-07。用户指出原测试脚本每执行一个动作都切回IDE，造成连续闪屏。本轮已修改执行方式，不只是更改口头约定。

## 当前结果

- 发布程序 `dist/RelinkStudio/RelinkStudio.exe` 新增 `--focus-policy caller-owned`：该模式只使用已经由外层批次前置的游戏，**不自行激活游戏，也不自行恢复IDE**。
- 外层 `tests/manual/run_foreground_batch.py` 负责整段任务：开始前置一次，连续执行观察/导航/回读，结束或出错时统一恢复IDE一次。每个点击后必须有新页面观察，避免旧画面连续驱动点击。
- 原 `desktop_navigation_probe.py`、`navigate_lobby_to_warehouse.py` 已转接批次入口，不再使用“预检回IDE→点击→回读回IDE”的旧调用链。
- 独立单次诊断默认 `standalone` 也改为采集、OCR、预览处理全部完成后再恢复IDE，不在OCR前后重复管理焦点。
- 真实两次连续采集已经验证：外层 `enter_calls=1`、`leave_calls=1`；两个子诊断的激活/恢复请求均为0，期间游戏保持前台，末尾恢复Mirasim。

## 实机证据不是推测

文件：`artifacts/m2_lobby_calibration/foreground_batch_live.json`。

两帧分别具有不同SHA-256；采集和Windows OCR成功，游戏图像不写文件，批次没有按钮点击。10ms周期的前台采样记录在首次观测游戏之后只有“游戏→IDE”，没有中间回IDE；这不是声称采样覆盖了每个微秒。子诊断代码同时报告零切换请求。

本次验证的是**批次焦点控制与连续采集**，不是当前曼德尔页识别成功。该页视觉上已确认进入曼德尔砖界面，但分类器仍返回Unknown，需要后续锚点校准。当前没有自动收藏或购买。

```powershell
# 离线核验已有实测记录，不切屏。
python -X utf8 tests\manual\verify_foreground_batch_evidence.py

# 只有执行整段实机检查时使用；记录路径必须是新文件。
python -X utf8 tests\manual\run_foreground_batch.py `
  --plan tests/manual/plans/focus_two_observations.json `
  --record artifacts/m2_lobby_calibration/foreground_batch_next.json
```

## 组成与异常处理

| 模块 | 职责 |
|---|---|
| `diagnostics/foreground_policy.h` | 单次/调用方所有权；结束操作幂等；析构不重复恢复 |
| `manual/foreground_batch_core.py` | 有界步骤队列；步骤失败、截止耗尽、前台被用户切走时终止剩余步骤；统一finally收尾 |
| `manual/run_foreground_batch.py` | 重新解析当前PID/HWND；调用零切换子诊断；校验点击前页类与客户区；归还指针和IDE |
| `manual/desktop_navigation_probe.py` | 兼容旧参数，将预检、单动作、回读放进同一批次；优先直接使用完整计划 |

一个计划最多12个观察/操作步骤、30秒；这一限制用于单个连贯的测试阶段，不是每一步都切屏的理由。下一段开始前先在后台完成必要的分析和修改，再一次性执行整段。

`caller-owned`不是普通双击启动模式。单独调用时若游戏不在前台，诊断报错而不抢回前台；此模式的调用者必须持有批次并负责最终恢复。普通UI依旧只管理配置/回放，测试脚本的导航点击没有接入前端自动业务。

## 本轮前序导航的准确记录

在用户明确要求由智能体点页面后，人工联调脚本执行过3次导航点击：仓库标签、返回大厅标签、大厅底部经悬停确认的“F4 市场”图标。没有点击购买、移动库存或更改关注。

- 仓库页已有真实多锚点匹配；部分早期预览曾带桌面缩略图浮层，不将这些帧称为完整无遮挡业务验收。
- 进入市场使用的是大厅底部“F4 市场”，不是顶部普通物资“交易行”。最新内存预览显示曼德尔砖/典藏外观/典藏挂饰三个页签。
- 这是为确认入口做的人工测试导航，不能写成S01–S39自动执行器已完成。
- 用户指出闪屏后，旧逐动作执行已停止；随后仅用新批次入口做了一段连续两帧只读验证。

## 回归

应用CTest **25/25**；新增前台策略测试验证独立/嵌套、成功/失败、作用域异常恢复、用户切前台后不重抢。Python批次协调器6项测试通过。原大厅37条和启动观察器250条断言、包内历史27页/5路线保持。

发布程序、所需DLL及plugins已更新；六张主要UI离屏图与基线一致。基线、修改后程序以及隔离副本回滚均执行了真实检查。精确命令/输入/输出/退出码与所有实机失败保留在 `artifacts/m2_lobby_calibration/VERIFICATION.txt`。
