# S11 连续收藏、滚动与读取延迟修正

日期：2026-10-08。事务：`artifacts/collection_tail_latency`。

## 当前结论

- S11 黑银先锋 AUG / AS Val / P90 三条已启用规则，已分别跑到原配置价格上限的停止边界：AUG 在 run04 完成，AS Val / P90 在 run06 完成。后段没有重跑已完成的 AUG。
- run04 与 run06 共完成 21 次滚动；45 + 51 次选卡均经后续新帧确认，没有再次因漏选卡停机。run04 的真实画面读数触发过新尾行间距分支，得到 943 / 943 的实际尾行顶部；不只是正常分支的测试。
- 本事务六段累计新增 104 条收藏记录，正式 journal 从 349 增至 453；原 349 条字节哈希和正式配置均保持。历史记录仅统计，当前符合条件的白星仍执行收藏。
- **速度目标尚未达到。** run06 的 34 个纯连续点星间隔中位数是 1000 ms，范围 890–1234 ms；没有达到每秒收藏多把，也没有两倍提速。run04 中位 938 ms，前两段短样本约 859 / 875 ms，样本商品、长度不同，不是受控 A/B。
- 最新 run06 已恢复 IDE，点星未决和几何未决均为零；结束时鼠标位置恢复另记 `CURSOR_INTERFERENCE`，保留这个结果。采集和 OCR 子进程已退出。
- 发布目录已更新，包含 `RelinkStudio.exe`、17 个收藏运行模块、所需 DLL 与 `platforms`。普通 UI“运行”尚未在本轮接入真实收藏 CLI。

## 实测记录

| 段 | 结果 | 扫描 | 新收藏 | 保留黄星 | 滚动 | 完成规则 | 耗时 |
|---|---|---:|---:|---:|---:|---|---:|
| run01 | 第六张选卡未响应 | 5 | 5 | 0 | 0 | 无 | 22.360 s |
| run02 | 同位置选卡未响应 | 5 | 5 | 0 | 0 | 无 | 24.781 s |
| run03 | 改落点后另一张仍未响应 | 4 | 1 | 3 | 0 | 无 | 20.688 s |
| run04 | 已确认收藏后的布局重读价格置信度不足 | 47 | 42 | 4 | 10 | AUG，行 12 | 86.484 s |
| run05 | 接管阶段鼠标位置变化，0 次业务输入 | 0 | 0 | 0 | 0 | 无 | 1.328 s |
| run06 | passed | 53 | 51 | 0 | 11 | AS Val / P90，行 14 / 16 | 97.172 s |

run04 / run06 三条规则的停止候选价格均为 310，超过该规则原上限 300。价格、磨损、成色条件未放宽。这是 S11 子集分段完成，不替换此前 21/21 完整业务循环的原始来源记录。

## 证据 → 修正 → 验证

### 1. 尾行间距误拒绝

原证据：`artifacts/collection_ready_stream/run06/blocked_layout_review.json`。

当前帧两列完整行间距分别测得 11 / 15 px，下一尾行两列间距为 15 / 15 px。旧逐列判断让左列的 `15 > 11 + 3`，拒绝实际尾行。

`src/application/vision/collection_layout.cpp` 保留原逐列判断；只有两列成对上下边仍符合原 3 px 对齐、至少有两个完整行且原行距结构一致时，追加同帧两列实际间距包络。此例使用实测 `[11,15]`，不加额外余量，仍拒绝缺边、未对齐和包络外候选。

- `tests/runtime/collection_tail_gap_tests.cpp`：44 条原生断言。
- `tests/runtime/fixtures/collection_tail_gap_run06.json`：原失败记录的数值输入，不含或重建原像素。
- `run04/tail_gap_branch_evidence.json`：真实新帧 `dxgi:8887dd45-deef-4e4f-9dae-734a3154ca20` 中，新分支实际接受 15 / 15 的尾部间距，测得顶部 943 / 943。

### 2. 点击位置与按下/抬起时序

run01 / run02 中，鼠标到达卡片中心、两事件 API 调用返回 2，但多帧仍保持旧卡选中。run03 改为当前完整卡片内部左上留白位置，仍在另一张卡片漏选；画面中的“C 查看详情”提示会跟随悬停位置。**落点调整本身没有解决漏响应，也没有证据把提示判定为拦截原因。**

`WindowsBackend.fast_selection_click()` 仅对选卡使用一次按下、请求保持 24 ms、一次抬起。没有重发按下；前台变化、位置变化或等待中断时执行抬起清理。原始抬起失败会保留不完整返回值，清理成功不伪装为动作成功。普通导航和点星维持原两事件路径，点星后固定等待仍为 0。

run04 与 run06 共 96 次选卡均完成新帧关联。24 ms 是本实现的请求值，时间记录含系统时钟量化，不声称是硬件送达时刻或 BBZPS 的原参数。

测试：`test_collection_selection_gutter.py`、`test_collection_selection_press.py`、原选中关联与时序测试。

### 3. 标题与历史读取减负

- `collection_local_title.py`：仅原生已定位的列表单行标题 ROI 使用已有识别器直接识别，保留 0.97 置信度、空/多行/低置信度回到原检测器。没有把期望商品名送给识别器，也没有改变共享 OCR 引擎的检测开关。
- `collection_journal.py`：`decision_records()` 在原锁与文件身份校验内返回决策所需的独立字段副本，避免深拷贝全部旧 OCR 证据。完整 `records()` 接口和未决检查继续保留。
- 历史副本基准中，完整读取中位约 52 ms，字段投影约 20.8 ms；这不是游戏端到端吞吐。中文合成标题模型测试另存 `title_model_chinese_benchmark.json`，不使用早先管道编码损坏的英文问号样本。

### 4. 收藏已确认后的价格重读

run04 先完成同一商品的黄星回执，随后补全整页布局时，价格 `300` 的置信度仅 0.88659；额外独立新帧读得 `300 / 0.99998`。旧重读条件仅覆盖“选卡未决”，没有覆盖“已确认回执后补全布局”。

新增 `local_price_refresh_waiting()`，仅允许已确认的局部回执之后，对同标题、同帧绑定、同区域黄星的低置信度结果再取新帧。它不接受旧价格，不放宽 0.99 门槛，不清除未决，也不发送输入。最多三次读取，重复帧立即停止。

真实数值回归：`tests/fixtures/collection_price_refresh_run04.json` 和 `test_collection_price_refresh.py`。run06 使用了新发布版本并完成 AS Val / P90；**该新增重读分支在 run06 没有触发**，其验证依据仍是原始失败/新帧数值的离线回放，而不是一次未发生的实机重试。

## 当前延迟分解与后续方向

run06 中位：选卡释放到点星开始 609 ms，点星释放到回执完成 266 ms，点星释放到下一选卡开始 359 ms。三项存在包含关系，不相加作为单件总时间。完整连续点星周期采用独立记录的 1000 ms。

下一轮提速重点仍是选中就绪与识别、完整回执到下一卡的串行衔接；继续缩短已经为零的点击后等待没有收益。保留同帧商品/价格/磨损关联，后续用新 S11 实测判断收益，不以局部模型耗时推导每秒多把。

## 复核入口

以下命令均为离线验证，不接管前台：

```powershell
.\.tools\ocr-runtime\Scripts\python.exe -B -X utf8 -m unittest discover -s tests\manual -p test_collection*.py
.\.tools\ocr-runtime\Scripts\python.exe -B -X utf8 artifacts\collection_tail_latency\audit_live.py --name run06
.\.tools\ocr-runtime\Scripts\python.exe -B -X utf8 tests\release\verify_collection_tail_latency_package.py verify
```

最新结果：726 项收藏 Python 测试、44 项 CTest、离屏 UI / SQLite / 依赖闭包与独立副本回滚通过。冻结基线为 643 项，19 skipped；不以当前测试树冒充基线。精确命令、输入、原始输出、退出状态和程序恢复结果在本事务 `VERIFICATION.txt`。

下一次新实机批次先协调前台；本轮已完成的批次不自动重跑。图像仅在内存中，失败预览不落盘；未购买、未清理关注、未移动库存、未运行 BBZPS 原二进制。详细数值日志与正式记录保留。

已解压程序：`C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`。
