# 收藏流水线提速（2026-10-08）

事务目录：`artifacts/collection_pipeline_speed`。用户已同意 `docs/COLLECTION_SPEED_PLAN.md` 的 6 项，并明确“有未决就禁止任何输入”不是他的规则。价格、磨损、成色条件，收藏后的成功回读，不购买，保留原关注，这几条都没有变。

**状态：S11 三条已全部实机跑完。单把收藏周期中位从 1000 ms 降到约 470–480 ms，不含翻页约每秒 2 把；含翻页和行内导航约 0.69 s/把。** run01–run06 中途停下的原因都已逐个修复；run07 一次跑完 AS Val 和 P90，没有出现异常停止。

## 实机记录（2026-10-08，S11 黑银先锋，用户同意占前台）

| 段 | 新收藏 | 保留金星 | 耗时 | 停止原因 → 修复 |
|---|---:|---:|---:|---|
| run01 | 1 | 0 | 17 s | 列表一致性把底部半截卡的边缘抖动算进去 → 只比较完整卡 |
| run02 | 9 | 1 | 23 s | 翻页稳定判定 16 帧上限在快速截帧下先用完；滑块因下方加载变短被判为列表变化 → 改按时间限、允许滑块变短 |
| run03 | 17 | 9 | 32 s | 第 6 次翻页后列表重绘超过 900 ms → 翻页等待上限 1.6 s，未稳定时只重读不重滚 |
| run04 | 1 | 12 | 22 s | 滚动条 1 px 中心线测量 1877/1878 → 允许 ±1 px |
| run05 | 23 | 0 | 33 s | 收藏后底部尾行间距 11/14 vs 15（老问题）→ 成对包络加同样的 3 px 边缘容差 |
| run06 | 15 | 21 | 45 s | 行 12（AUG）已到价格边界 320；行 14 点星前光标被外部移动（CURSOR_INTERFERENCE），未发送点击 |
| run07 | 48 | 0 | 59 s | **passed**：行 14（AS Val）到价格边界 308，行 16（P90）到 310；10 次翻页，无未决，IDE 已恢复 |

- run07（行 14+16）：48 把新收藏，选卡到下一张选卡中位 484 ms（p10 453 / p90 594），含翻页的周期中位 1242 ms。AS Val 20 把用 13.1 s，P90 28 把用 18.6 s，都约 0.69 s/把（含翻页）。每行从开始到列表顶部的导航约 8–9.5 s。2 次回执在指针静止后重读列表通过，每次多花约 0.4–0.8 s。
- run01–run06 共新收藏 66 把，回执全部为像素金星确认。有 2 次移动中帧不足以证明列表未变，都在指针静止后重读通过。没有一次退回完整 OCR 回执，也没有重复点星。
- 每次结束都已切回 IDE（Mirasim 窗口）。原 journal 记录和正式配置全部未改。
- run06 最后一把 AS Val 只做了预留，点击没有发出（`input_dispatch=null`），已按证据移到 `artifacts/m2_savedvalue_collection/released_unsent/`。代码也补上了：今后点星前被打断时自动这样处理，不会挡住下一次运行。

| 指标（run02–run06 中位） | 新 | 旧（tail_latency/run06） |
|---|---:|---:|
| 收藏一把（选卡到下一张选卡） | 469 ms（n=49，p10 453 / p90 578） | 1000 ms |
| 已是金星、跳过一把 | 375 ms（n=32） | — |
| 含一次翻页的周期 | 1188 ms（n=20） | 约 2600 ms |
| 选中后识别 | 61–72 ms | 157 ms |
| 点星前记账 | 18–20 ms | 79 ms |
| 回执等待（并入下一张移动） | 0 ms（往返 31–47 ms） | 265 ms |
| 抬起到看到选中 | 208–221 ms | 243 ms（主要是游戏本身） |

按每页 4 把算，含翻页约 0.65 s/把（约 1.5 把/秒）。

## 改了什么

| # | 改动 | 位置 | 离线证据 |
|---|---|---|---|
| 1 | journal 热路径：一次目录枚举拿到每个文件的 ID、大小、写入和变更时间，只重读新增或改动的文件 | `collection_journal.py` `_directory_signatures` | 453 条记录：`decision_records` 20.4 → 2.8 ms，`prepare` 扫描 18.2 → 1.4 ms。原地改写后即使恢复大小和 mtime 也会重读（ChangeTime 会变），有测试覆盖 |
| 1 | 步骤证据改后台写盘：主线程序列化，后台线程 fsync，同一路径只保留最新一份；journal 仍同步落盘 | `collection_live_session.py` `AsyncJsonWriter` | 以前每把要同步写 4 次约 400 KB 的 JSON 加 8 次小文件 |
| 1 | 翻页规划读 journal 改用决策投影 | `run_collection_trial.py` `scroll_list` | 以前每次翻页整读 453 个 JSON，约 62 ms |
| 2 | 像素回执：点星后原生端只抓新帧，确认同一张选中卡、几何不变、详情星由白变金，不做 OCR。识别失败时退回原来的完整 OCR 回执 | `live_capture_check.cpp` `--collection-pixel-receipt`、`collection_candidate.py` `validate_pixel_receipt` | 收藏记录写 `receipt_kind=pixel_same_card_white_to_gold`、`text_reread=false`，商品、价格、磨损沿用该卡自己选中帧的读数 |
| 3 | 原生 OCR 线程池：3 个后台助手加请求线程，同一帧的 4 个列表 ROI 并行识别，弹窗检测区拆成重叠 60 px 的两半；精确磨损 ROI 和选中卡成色 ROI 在同一帧预取 | `windows_ocr.cpp` `WindowsOcrPool`、`live_capture_check.cpp` | 合成画面上串行 118–121 ms，池化 83–90 ms（拆半前）；结果顺序、语言、缩放和原帧不变，有测试 |
| 3 | Python 标题和价格用两个模型实例并行识别 | `run_collection_observed.py` `parallel_local_price` | 合成样本 40.3 → 21.3 ms |
| 4 | DXGI 取帧改为阻塞 Map，并把进程计时精度设为 1 ms | `dxgi_observation_source.cpp`、`timeBeginPeriod(1)` | 以前 `DO_NOT_WAIT + Sleep(1)` 每帧多睡约 15.6 ms，实测每帧拷贝 21–28 ms |
| 5 | 翻页：滚轮前移动改用收藏的 continuous 轨迹和详情面板绕行；去掉 180 ms 固定等待，仍靠原生“3 帧、100 ms 稳定”判定；翻页后的识别走列表 ROI 热路径 | `collection_live_session.py`、`run_collection_observed.py` | 旧版：移动 264 ms、固定等待 187 ms、整屏 OCR 166 ms |
| 6 | 流水线：点星后立即发像素回执请求，并开始移向下一张卡；按下前必须拿到金星回执，并且列表未变。若移动中的帧证明不了列表未变，就在指针静止后再读一次；仍不一致则不按下并停止 | `_start_pixel_receipt` / `_settle_receipt` / `dispatch_guard` | 下一张卡的按下事件排在回执之后，有测试覆盖 |

## 安全边界

- 每次点星前仍要求：同帧读到的标题、价格、成色、磨损都符合规则，当前星为白色，journal 无未决记录，并且先把预留记录落盘。
- 流水线只放行“下一次选卡”的指针移动。翻页、导航、本行结束和会话结束都会先结清回执。翻页的租约要读 journal，所以翻页前一定先结清。
- 回执超时或像素不符时走原完整 OCR 回执；再不通过就停止，journal 保持 `dispatched`，下次启动会要求先对账，不会重复点星。
- 新选项默认全部关闭（`async_reports`、`pixel_receipts`、`pipeline_receipts`、`parallel_local_price`），旧调用方式和旧测试行为不变。实机入口 `artifacts/collection_pipeline_speed/run_s11.py` 会全部打开。`--no-pipeline` 只用像素回执，`--legacy-receipts` 回到原完整回执，方便对照。

## 验证

- CTest 44/44 通过，其中新增像素回执门控 9 条断言、拆半合并 2 条、尾行静态锚点重读 2 条、OCR 线程池 8 条（含真实 Windows OCR），尾行间距用 run05 实测数字补 2 条。
- Python：收藏 757 项、前台批次和指针测试全部通过。新增 `test_collection_pipeline_receipts.py` 共 28 项（含 run01 真实布局数字回归、翻页稳定重读、未发送预留释放），覆盖流水线顺序、白星回退、列表变化、静止重读、同步模式、异步写盘、调度方计数和后端协议；另有 journal 目录签名 2 项、滚轮轨迹 1 项。
- 发布：`build.ps1 -Test -Package` 输出 PACKAGE=PASS，17 个收藏模块与源码逐字节一致。运行时自检、离屏 UI 自检、SQLite 自检都通过。
- 发布 EXE SHA256：`7e331c5d1f185e5814a7895edd21bd9a0fe471ce49eb1eb9c266447cc28f3e9d`。
- 命令和输出见 `artifacts/collection_pipeline_speed/VERIFICATION.txt`。

## 后续可做

- 现在一把里最大的一段是游戏自己显示选中的约 210 ms，加上两段鼠标轨迹约 110 ms。
- 换商品、换成色时，导航点击仍然每次固定等 0.6 s，每行约 10 s，是下一步最值得改的地方。
- 每段的时序可以用 `cycle_breakdown.py <run目录> --json` 拆出来。

回滚：把 `baseline/` 下的源文件拷回原路径，再运行 `build.ps1 -Test -Package`。
