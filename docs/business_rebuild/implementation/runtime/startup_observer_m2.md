# M2 原启动页面识别与只读观察器

日期：2026-10-07。对应 [原流程 S01–S39](../../10_bbzps_first_startup_reconstruction.md) 的启动部分，实施 S04–S10 的页面/关注预检与 S32–S33 的已有列表接续检查点，不生成导航点击或购买。

## 1. 本轮完成了什么

- `SkinPageClassifier`：以多个文本锚点及区域判断大厅、仓库、曼德尔页面、典藏首页、空关注、总筛选、商品卖单、带关注计数的卖单、游戏设置与特勤处；缺证据或页类冲突返回Unknown。
- `StartupObserver`：保留原“首页→我的关注预检→确认空关注→回首页→筛选”的观察顺序；已有列表进入排序/身份核对检查点，不强制重跑收藏。
- 有模态筛选或结果提示时，只返回重新观察检查点，不把遮挡后的底页判作可继续执行。
- CLI `--startup-observer-self-test` 使用包内冻结的历史OCR投影，QCoreApplication运行，无游戏帧采集或可见窗口。
- 现有 `--live-capture-check --ocr` 新增 `startup_page` 结构化诊断字段；**本轮没有运行该实机命令**，等待用户准备后再校准。

普通UI、旧合成回放与配置保持原行为。旧回放器不是完整BBZPS原流程的实机执行器；新观察器也没有与输入/交易适配器相连。

## 2. 为什么不是直接使用旧“当前页面”标签

27条页面fixture来自原日志的全页OCR及紧邻的归一化记录，只导出业务锚点文本、坐标、原OCR分数及文件/原行哈希，不含截图或完整OCR全文。构建时嵌入资源，发布程序无需读取BBZPS目录。

这次发现两个具体反例：

| 历史原行 | 旧标签 | 实际识别文字 | 当前处理 |
|---|---|---|---|
| `BBZ_20260930_113211.log`:11–13 | 大战场 | 分辨率、显示模式、局内帧数上限、视频 | `game_settings`，不是凭标签判为战斗模式 |
| `BBZ_20260928_104709.log`:56–58 | 应用外观 | 同类显示/视频设置字段 | `game_settings`，不把“应用”按钮读成枪皮应用外观 |

因此前一份39步文档里引用的“大战场/应用外观”等名称，继续作为**原分类器标签**保留，不应理解为已证明发生了相应实际页面转换。当前fixture同时保存 `legacy_page_label` 与人工核对锚点后的 `expected_page`，分类器只读取OCR内容，忽略旧标签。

归一化记录声明的区域包括1920×1080和2880×1620；它与原打印ROI存在尺度差异。本轮原样保留其声明，不猜测第二次DPI缩放。比例变换测试只证明新分类器在同一坐标约定下的数学一致性，不替代真实屏幕尺度校准。

## 3. 页面判断契约

输入：`{width,height,coverage:"full_client",words:[{text,x,y,width,height,score?}]}`。

| 约束 | 行为 |
|---|---|
| 宽高必须为1–8192整数，字框须有限且落在帧内 | 无效输入被拒绝，不猜裁剪外内容 |
| 必须明确full_client | 局部ROI和缺失覆盖范围不当作完整页面 |
| 最多2000词、文本合计64KiB | 超限被拒绝 |
| 有原生score时范围0–1 | 当前诊断忽略低于0.70的词；这是本项目门槛，不是恢复出的原算法阈值 |
| Windows OCR没有score | 标注score不可用，不伪造数值；仍要求多锚点 |
| 相邻同一行的中文分词 | 有界合并最多8个近邻token；不拼接整个屏幕 |
| 导航“交易行”“典藏外观”单独出现 | 不构成业务页证明 |
| 不相容页面同时满足 | `E_PAGE_AMBIGUOUS`，不择一推进 |

页面结果有 `page/overlay/reason/anchors/candidate_pages`。所有输出保持 `live_calibrated=false` 与 `actions_enabled=false`，直到另有真实版本校准记录。数字、商品名、价格和磨损尚不从这个页面分类器写入业务账本。

空关注需要“在售”与“暂未添加任何关注…”两项独立特征；“返回”是可选佐证，因为历史2880记录中的“返回”分数仅约0.50。不会为通过这个例子而把低置信词升级为可靠锚点。

## 4. 启动观察器的顺序

| 当前观察 | 已有证明 | 输出检查点/状态 |
|---|---|---|
| 典藏首页 | 尚未证实空关注 | `watchlist_precheck` / AwaitWatchlist |
| 第一张空关注 | 新鲜且与当前上下文匹配 | `confirm_empty_watchlist` |
| 第二张独立新鲜空关注 | 非同一frame ID、非同一呈现时间 | `skin_home_after_empty` / AwaitHomeAfterEmpty |
| 返回典藏首页 | 已确认空关注 | `catalog_filter` / AwaitCatalogFilter |
| 出现总筛选 | 空关注及返回首页均已证明 | FilterObserved，接下来读取赛季/拥有/品阶 |
| 一开始就看到筛选 | 尚无预检证明 | `watchlist_precheck_unproven`，不伪称完整启动顺序已通过 |
| 已有商品/关注卖单列表 | 无遮挡且页面锚点充分 | ExistingListObserved，接续S32/S33，不生成重新收藏命令 |
| 列表筛选遮挡、结果提示 | 任意 | `recheck_after_overlay`，清除可能失效的预检证明 |
| Unknown | 任意 | 有界重观测，预算/截止耗尽则Paused |

两次空关注为本项目可配置的观察确认策略（1–5次，默认2），来源中有两次回读记录，但不宣称原受保护程序所有分支都固定两次。

上下文包含run/session/clock/step/cancel epoch/viewport generation；旧上下文、重复帧、重复呈现时间、过期/未来时间都不推进。默认10秒、32个观察预算，硬上限60秒/128个观察；暂停/停止清除预检证明，重启须使用新上下文。观察器不创建线程、定时器、采集请求或输入命令。

## 5. 离线验收与实机验收的区别

本轮CTest共23组。新增 `startup_observer_tests` 检查27条历史投影、不同尺寸比例、缺字段/低置信/冲突、中文分词、重复/过期帧、暂停/停止/预算以及原顺序。包内自检检查27页与5条分支。**这不是对当前游戏截图识别准确率的测量。**

```powershell
# 不启动游戏，不采集屏幕，直接验证当前包内原流程投影。
& .\dist\RelinkStudio\RelinkStudio.exe --startup-observer-self-test

# 本轮构建、包内验收、副本回退的可复用入口。
python -X utf8 tests\release\verify_startup_observer_package.py build
python -X utf8 tests\release\verify_startup_observer_package.py verify
python -X utf8 tests\release\verify_startup_observer_package.py finalize
```

OCR辅助进程验证另做了修正：运行期间按helper PID采样可见窗口和前台归属，不再把用户自行切换应用造成的全局HWND变化误报成OCR抢占前台。只有自己的helper实际被观测为可见/占前台才记 `E_OCR_HELPER_VISIBLE`。该检查是周期采样，不声称覆盖两次采样之间所有瞬时窗口行为。

## 6. 下一步实机准备

用户准备后，先保持游戏在大厅，保留现有分辨率/DPI，不改配置、不清空关注、不新增购买。随后逐页人工展示典藏外观、我的关注、总筛选和已有枪皮列表；每一步只在当前业务检查点提出一次或有界画面需求。

核对：完整客户区及字框尺度、当前版本的多锚点、空/非空关注区分、筛选遮挡、旧帧拒绝，以及恢复IDE前台。关注非空时直接使用现有条目观察，不通过删除关注来制造空列表。

每轮短暂前置游戏后恢复并校验Mirasim前台。像素留在内存；不运行BBZPS；不把只读观察接成点击或购买。实机校准结果将单独记录，不能把本轮27条历史fixture标成当前游戏实测成功。
