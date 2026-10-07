# M2 大厅实机校准：2026-10-07

> 后续用户纠正与实现见 [整段前台批次](foreground_batch_m2.md)：不再逐动作切屏，不再让用户逐页代点。下文为最初大厅只读阶段；之后用户授权了3次人工导航点击，已进入曼德尔砖市场但该页识别仍Unknown。当前应用测试为25组；固定交付是后续焦点修复版，原大厅实测EXE单独保留用于哈希对照。

## 结论与下一步

用户准备游戏主界面后，本轮在 **2560×1440、144 DPI** 的真实游戏客户区完成两次独立的大厅识别，均返回 `lobby`，并验证回到 Mirasim 前台。运行截图不写磁盘，没有游戏点击、收藏变更或购买。

下一页按 [原流程 S04–S06](../../10_bbzps_first_startup_reconstruction.md) 由用户展示仓库，再核对曼德尔/典藏入口。**本轮没有把后续页面、商品字段或完整39步业务标为通过。**

交付程序仍为 `C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`。主界面的 Win11 白灰样式和配置行为保持不变。

## 证据 → 发现 → 修改

本轮证据目录：`artifacts/m2_lobby_calibration/`。JSON 保存确切命令、EXE哈希、帧哈希、字框/白名单锚点、输出及退出码，不保存游戏图片或全文OCR。

| 证据文件 | 实际结果 | 处理 |
|---|---|---|
| `live_baseline.json` | 申请3帧，取到2帧后出现 `E_WINDOW_NOT_FOREGROUND`；退出1，恢复IDE | 保留失败；前台变化立即停止，不继续读取其它窗口 |
| `live_single_frame.json` | 单帧采集/OCR成功，但页类为Unknown | 采集成功与页类成功分开，不假报大厅通过 |
| `live_preview_metadata.json` | 明确请求的内存预览显示大厅；页类仍Unknown | 只保存元数据；预览图片没有写文件 |
| `live_anchor_diagnostics.json` | OCR实际给出“开/始/游/观”“仓/库”“行/前/备/战” | 确定是按钮的局部OCR混淆，不是业务页跳转 |
| `live_modified_lobby_1.json`、`live_modified_lobby_2.json` | 两个不同帧哈希，均识别Lobby、无覆盖弹窗、退出0 | 验证发布EXE修复，不重用旧帧或只跑fixture |
| `live_expected_mismatch.json` | 真实仍在大厅，故意要求Warehouse；返回 `E_DIAGNOSTIC_PAGE_MISMATCH`、退出1 | 证明采集/OCR成功不自动满足页面验收 |

内存预览的视觉文字是“开始游戏”；系统Windows OCR将末字“戏”识别为“观”。这是一条当前设备样例，不代表该OCR引擎总有这种错误。

### 局部兼容，而不是全局模糊替换

原来的精确三锚点规则继续保留。仅当以下条件全部成立时，增加大厅候选：

1. 输入协议是 `windows-ocr-once-v1`，语言为 `zh-Hans-CN`。
2. 左上开始按钮区域内识别到“开始游观”。
3. 相邻仓库导航区域内独立识别到“仓库”。
4. 右下准备按钮区域内独立识别到“行前备战”。
5. 原有输入校验、低分过滤、同行近邻拼接、页类冲突和弹窗检查继续生效。

没有全局将“观”改成“戏”，没有接受任意编辑距离为1的文字，也没有修改仓库、典藏、价格或交易规则。输出 `calibration=lobby_live_sample_20261007` 标记此证据来源；`live_calibrated=false` 保留，表示尚未完成多页面/多显示配置的整体校准，`actions_enabled=false` 保持。

## 新增诊断字段

- `--ocr-lobby-anchors` 必须与 `--ocr` 同用；仅导出三个固定按钮区域中允许字符组成的词框。每区最多32词，截断会显式标记；不导出额外字段、未知文字、账号数值或整页OCR。
- 投影明确标记 `coverage=anchor_projection`，不能直接作为完整画面送入分类器。
- `--expected-page PAGE` 必须与 `--ocr` 同用。只有页类匹配且没有阻挡弹窗时，`page_match_passed=true`；否则退出1。错误页名/缺OCR退出2，发生在窗口绑定前。
- `startup_page` 是当前页类判断；旧 `ocr.page_hint` 只检测特勤处设施，因此大厅时它仍可能为Unknown，不应替代 `startup_page`。

## 离线回归与发布验收

本轮应用CTest **24/24**；新增大厅专项 **37条断言**，覆盖实际分词、缩放、逐字缺失、错误位置、错误provider、其它相似字、低分、跨区域拼接及诊断投影的信息最小化。原启动观察器250条断言和27条历史OCR投影保持通过。

包内 `--startup-observer-self-test` 保留27页/5路径，另新增 `live_projection_checks=1`。它回放的是本轮冻结的文字/坐标fixture，**不是重新采集游戏，也不是实时识别准确率测试**。

发布包及独立副本的UI/工作区/SQLite自检通过；六张主要UI离屏图片与基线逐像素一致。回滚恢复基线文件哈希并保留用户数据哨兵；固定交付目录和 `MODIFIED_FILE.exe` 保留修复版。四份正式交付证据是本目录事务中的 `MODIFIED_FILE.exe`、`DIFF_FILE`、`VERIFICATION.txt`、`ROLLBACK.sh`。

```powershell
# 只离线验收，不前置游戏。
python -X utf8 tests\release\verify_lobby_calibration_package.py build
python -X utf8 tests\release\verify_lobby_calibration_package.py verify
python -X utf8 tests\manual\verify_lobby_evidence.py
python -X utf8 tests\release\verify_lobby_calibration_package.py finalize

# 只有用户已展示对应页面时才手动执行。每次重新解析当前窗口身份。
# record必须是尚不存在的文件，避免覆盖既有实测证据。
python -X utf8 tests\manual\live_startup_probe.py --expected-page lobby `
  --lobby-anchors --record artifacts/m2_lobby_calibration/live_manual_next.json
```

一次大厅样例只能验证当前窗口配置。其它DPI、画幅、弹窗遮挡、仓库/曼德尔/典藏页，以及商品名、价格、成色的绑定仍须逐页实机校准；不会为测试清空原有关注。
