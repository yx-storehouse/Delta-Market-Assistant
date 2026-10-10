# 全配置收藏：实机完成结果与识别合同

> 最新后续已完成价格修复和提速，见 [收藏提速验收](COLLECTION_SPEED.md)。下文 run_12 / 98次新增 / journal144 均为原开发周期历史，不是现在的数据。最新状态以 current_checkpoint 和 collection_speed 为准。

**显式 CLI 已完成本次冻结配置的 21 条启用规则、9 个商品；最后 `run_12` 退出 0，IDE 已恢复。普通 UI 的“运行”按钮仍未接通这条完整实机链路。**

这里的完成，是原业务默认排序下每条规则均观察到高于价格上限的停止边界；**不是把未查看尾部算成已扫描，也不是全市场永远扫描完毕**。最终同时保留 `task_file_fully_completed=true` 与 `exhaustive_market_scan=false`。

## 最终真实结果

正式配置50行任务、21启用、29禁用、9个启用商品；禁用项没有开启。12个连续段沿同一冻结快照续跑，先前故障保留，最后一段完成剩余6条规则。

| 指标 | 结果 | 口径 |
| --- | ---: | --- |
| 完成规则 | 21/21 | 全部启用规则有实际原价边界证据 |
| 启用商品 | 9/9 | 不代表所有其他商品及分支均已测试 |
| 候选检查 | 293 | 实际检查累计，非全市场唯一商品数 |
| 新增收藏 | 98 | 98个唯一星标动作，98条新帧机器回执 |
| 保留已有金星 | 174 | 检查事件数，含重复读取，不是174个不同旧收藏 |
| 不符合当前规则 | 21 | 实际候选检查结果 |
| 历史journal | 144 | 138 confirmed、6历史confirmed_reconciliation；不是当前关注页数量 |
| 待收藏回执 / 待几何关联 | 0 / 0 | 最终两个pending均为空 |
| 购买 / 库存移动 | 未执行 | 本轮仅收藏阶段 |
| 游戏图像写盘 | 0 | 显式离屏UI测试图另行记录 |

本事务新增98条全部为confirmed。独立复核逐条检查sent=2、商品/成色/价格/磨损/源行相同、点星前后帧不同、实际白星到金星及机器回执通过。旧历史46条加本轮98条等于144条历史动作记录，不用于推断当前关注列表大小。

完成源行为 `0..12、14、16、18、19、20、21、22、23`：AUG天命、P90天命、SR-25天命各S/A/B/C，共12条；AUG黑银先锋、ASVal黑银先锋、P90黑银先锋各S，共3条；K437私人定制、勇士私人定制、AWM私人定制各S/A，共6条。

## 原始故障与最后成功分别保留

| 连续段 | 本段新增 | 累计完成规则 | 原停止原因 |
| --- | ---: | ---: | --- |
| run_01 | 0 | 0 | CURSOR_INTERFERENCE |
| run_02 | 7 | 4 | P90大标题不完整 |
| run_03 | 0 | 4 | 窄Latin区仍误读P90 |
| run_04 | 0 | 6 | COLLECTION_WEAR_ASSOCIATION |
| run_05 | 26 | 12 | 滑块高度62→61，旧等长比较拒绝 |
| run_06 | 0 | 12 | 筛选框锚点遗漏，CONDITION_MENU_REQUIRED |
| run_07 | 9 | 12 | 选中框上边变化超出旧容差 |
| run_08 | 19 | 13 | 目录滚动后缺局部页面识别，ASVal定位未完成 |
| run_09 | 14 | 13 | E_COLLECTION_PARTIAL_SPANS_ROWS |
| run_10 | 17 | 14 | E_COLLECTION_ROW_EDGES |
| run_11 | 3 | 15 | COLLECTION_SEASON_READBACK_MISMATCH |
| run_12 | 3 | 21 | 无错误，completed，exit=0 |

更早price_probe_01的E_WINDOW_OCCLUDED是真实失败，但之后已采集、导航并连续收藏，**遮挡不是最终状态**。历史错误未改写成通过；修正后的新观察才支持继续。每个结束段enter_calls=leave_calls=1，均恢复IDE。

开发迭代间隔不算单次成品运行耗时；run12的100.094秒、3条新增只是末段，不能当作全部98条的速度。174次保留包含重复看到同一金星；详细分段指标及样本边界见本事务performance_review.json / .md。

## 标题、目录：运行前固定本地provider

Windows原反例保留：run02/session.steps/000076.json中文只读出P及中文后缀，Latin为空；run03/session.steps/000011.json窄区英文2x读PSB、1x读p"。没有补90，也没有宣称窄区已修好P90。后续Windows目录ASVa[也没有被字符替换成配置中的ASVal。

当前显式协调器通过 `--local-title-ocr` 在运行开始前固定 `RapidOCR3.9.2/PP-OCRv6-local-CPU`，同一引擎处理product_title与catalog_names实际原始ROI。接口不接收期待商品，不按是否匹配配置动态换provider。原生 `--collection-title-image` / `--collection-catalog-image` 提供同帧ROI，核验帧ID/SHA、PNG SHA及尺寸后，模型真实词框映射回客户区。

接受阈值固定为真实词score≥0.97；低分词保留为rejected，Windows原词保留为windows_observation。run04实际完整P90标题score=0.99687。run09的4次目录观察保留全部目标和非目标词：真实“AS Val突击步枪-黑银先锋”分数0.99882、0.99232，同屏也有“AS Val突击步枪-管弦乐队”。模型文本没有按配置改写，既有严格标签匹配再比较实际读数。

| 固定模型 | 本机校验SHA256 |
| --- | --- |
| PP-OCRv6_det_small.onnx | 090f04abcd9d9a7498bc4ebf677e4cb9bdce1fe4197ddb7e529f1ef44e1ff94f |
| PP-OCRv6_rec_small.onnx | 6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884 |
| ch_ppocr_mobile_v2.0_cls_mobile.onnx | e47acedf663230f8863ff1ab0e64dd2d82b838fceb5957146dab185a89d6215c |

模型在独立.tools/ocr-runtime本机运行，不上传游戏图像。**本地模型负责标题与目录，不是价格、磨损、赛季、复选框全被PP-OCR接管，也不是普通前端已内置整套执行入口。**

## 价格和磨损保持实际来源

价格仍来自Windows同帧数字ROI。numeric-price仅做真实数字字块、去框线、留白及坐标逆映射；不接收期待价格、不把字母改数字。历史300→30漏位仍保留，后续成功不保证未来画面永不误读。

磨损来源保留为direct（实际完整直读）或chinese_prefix_english_decimal（同帧中文真实成色/括号前缀与英文实际带点小数尾严格关联）。run04/session.steps/000033.json中文读B〔1 463027〕，英文读BC及1.463027)。后续使用英文实际存在的小数点，不给中文补点、不把C翻译成括号。可忽略的非数字英文前缀必须落在中文前缀框±2px内；含数字前缀不能丢弃，数字/成色冲突仍停止。wear_evidence保存ROI、原词、框与帧ID/SHA。

run05/events.jsonl的230 / B / 1.463027新收藏提供了真实验证。磨损上限5仅是配置条件，不是实际磨损来源。

## 筛选、赛季和几何均需当前帧

- 目录滚动后的预期skin_home采集也进行ui_regions补读，不把unknown强改为目标页。列表筛选框缺词时读取同帧窄ROI再走原锚点分类；恢复已识别筛选框先关闭并重新观察，不借恢复流程点击收藏。
- run11季名为空的失败保留。run12/session.steps/000013.json在同帧[770,426,270,42]区以Windows中文3x实际读到“气”“象”“感”“应”，expected_text_supplied=false。仅替换季名文字区再跑原CatalogFilterReader，replacement_applied=true、reader_complete_after=true，六个checkbox实测unchecked。7ms只指该ROI补读，不是整帧识别速度。
- 选中框边缘、滑块端点和尾行分隔的实测变化均有独立回放及原失败记录；不把滑块变化归因为已证明的量化误差。几何仍来自当前原像素，不用旧坐标补边，不把缺边视口标完整。
- 已派发星标动作才可用receipt_only局部新帧证明。商品/成色/价格/磨损、新帧及金星要求不变；不证明全视口完整或全局唯一选中，不授权下一次选择/滚动，下一动作先重新得到完整视口。

最终无待回执、待选卡或待滚动关联。日志中的历史卡片框不是可直接操作的当前位置，新操作始终重新观察。

## CLI入口、UI边界与交付

最后成功段请求命令如下，仅作复现记录，不向已有目录重放，已确认的星标不能再次切换：

```powershell
& "C:\Users\Administrator\Desktop\price\.tools\ocr-runtime\Scripts\python.exe" -X utf8 tests/manual/run_collection_observed.py `
  --snapshot artifacts/collection_full_cycle/input/task_snapshot.json `
  --output artifacts/collection_full_cycle/run_12 `
  --seconds 900 --numeric-price --local-title-ocr `
  --resume artifacts/collection_full_cycle/run_11/summary.json
```

wrapper stdout可能含内存预览，不能重定向到文件。会话写日志前移除preview、价格、标题与目录图像envelope，日志仅存词框/文字/数值/耗时/哈希。游戏截图零落盘与显式离屏UI验收图分开统计。

本次成功仅覆盖冻结配置收藏，不表示UI Run已接通、购买已实现、其他分辨率/缩放或非零限量/仅磨损/满额等未出现分支已验收。固定解压程序仍为 `C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`；业务成功与最终构建、离屏测试、隔离回滚独立，发布结果以最终VERIFICATION.txt及其程序哈希为准，不能用早期包结果覆盖新代码。

## 证据与续接

- 正式config SHA：7c317fc1850e2ca85eeba394f73f9c231499f7a781dad570f95f0e4287b8f5b9。
- snapshot SHA：f6f2e94eb9fd477129404696d896a2edd5d644f90d71eb6e19a3dbb9bc5651e8。
- artifacts/collection_full_cycle/independent_final_review.json：12段resume哈希链、配置未变、各段累加、98唯一动作及逐条白星到金星新帧回执、最终21条边界与pending清空。
- local_catalog_provider_review.json：真实目录全量输出及非匹配项、Windows原词、同帧绑定与模型SHA。
- season_readback_provider_review.json：真实季名窄区词框、原reader复读和六checkbox。
- `artifacts/m2_savedvalue_collection/current_checkpoint.json`：已发布的最终v2状态；`doc_drafts/checkpoint.final.draft.json`保留原草稿，不合并旧错误、旧卡坐标和旧帧。

只读独立复核命令，不操作游戏：

```powershell
python -X utf8 artifacts/collection_full_cycle/doc_drafts/review_final_cycle.py
```

```text
继续Delta Market Assistant：本次显式CLI已完成run12，21启用规则/9商品、新增98条机器新帧确认；历史144条不是当前关注数量。
先读最终summary、independent_final_review和正式checkpoint，不使用旧run04断点、旧遮挡结论或历史卡片坐标。
UI运行按钮尚未接此CLI。UI、购买及其他分支须分别开发验证，不把收藏成功扩大为所有业务完成。
保留关注和journal、不重复切星，新动作重新观察。游戏图像仅内存，每连续段前置一次、结束恢复IDE一次。
```
