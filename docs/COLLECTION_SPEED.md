# 收藏提速：当前实机结果与维护入口

本轮已完成价格识别修复、跨采集常驻和动态动作等待缩短。最新 `run_04` 完成 **9/21 条启用规则**，在第 9 行因 `COLLECTION_CONFIRMED_STATE_CONFLICT` 停止，退出 1。已完成规则以原业务实际超价边界结束，仍为 `exhaustive_market_scan=false`。**新一轮完整业务尚未全部通过；不把性能改善写成21规则已全通。**普通 UI 的“运行”按钮尚未接入该实机 CLI。

## 当前结果

- 新整份周期：检查 60 次候选，新加关注 24 次，保留已有金星 27 次，不符合规则 9 次。保留数是读取事件，不是不同收藏数量。
- 本提速事务还先恢复上轮剩余 3 条规则；该恢复段新增 2 次，且在新帧真实读到 `480`、磨损 `0.406920`，原最高价 `260` 因此正确结束，不点那颗星。
- 整个提速事务新增 26 条 confirmed，原 163 条 journal 字节哈希保持不变。历史 journal 189 条不是当前关注页数量。
- 未进入后续我的关注购买阶段；未清空原关注、未移动库存。pending 收藏、pending 几何及 journal pending 均为 0。
- 本轮停在真实业务冲突：SR-25 天命、成色 A、磨损 0.739214；历史已确认关注时价格 230，当前新帧价格 320 且白星。价格不是物品唯一身份，既有 key 不因价格变化自动重建。因此本次没有重按这颗星，也没有改掉历史 confirmed；需用户确定“当前白星允许重新收藏”或“历史已确认的一律跳过”的规则。
- 最后整段前置/恢复各一次；IDE 已恢复。常驻 capture worker 与 OCR helper 已由进程退出检查确认结束，游戏图片写盘 0。
- 已解压程序：`C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`。

## 实际速度，而不是预测倍数

基线是 `collection_acceptance/run_01`；修改后代表性段是 `artifacts/collection_speed/run_03`（含20次新收藏）。最新 `run_04` 没有新增收藏，不用它的空样本替代真实动作计时。使用同一冻结配置，但市场卖单、初始状态及新增条数不同。以下仅比较同类动作观测中位数，不据总周期耗时推算倍数，不宣称与 BBZPS 做过同环境跑分。

| 观测指标 | 原版本 | 修改后 |
| --- | ---: | ---: |
| 整步采集和识别 | 1500 ms（n=137） | 906 ms（n=90） |
| 收藏动作开始至成功回读 | 2391 ms（n=19） | 1953 ms（n=20） |
| 显式选卡开始至收藏成功回读 | 4851.5 ms（n=10） | 3219 ms（n=15） |

最终段 native capture 请求 32 次，协调端只启动 1 个服务进程。完整分位数、冷/热引擎次数、轨迹与等待分解见 [after_timing.json](../artifacts/collection_speed/after_timing.json)。轨迹仍保留原多点 Bézier 120–280 ms；未删商品、成色、价格、磨损、前台或回执校验来换速度。

## 实现与保持的合同

1. `--live-capture-server` 用严格 JSONL 串行接收只读请求。跨请求保留 OCR owner/helper 与语言引擎；每次独立新 DXGI 采集、窗口身份/遮挡检查、原帧龄门槛。EOF 退出；超时/断管不自动重放。
2. 价格使用运行前固定的 RapidOCR 3.9.2 / PP-OCRv6 CPU provider，与标题共享已载入的 hash 固定模型。C++ 只测墨迹、边框、字块下界并传同帧原始 ROI；Python 读取完整数字，实际分数门为 0.99，真实 detector 词框逆映射、墨迹覆盖和位数下界全部保留。无期待价格输入，不做 O/0 或 囗/0 替换。
3. 动态选卡/收藏/滚动先等待 120/80/180 ms，再执行原新帧成功屏障；仍未就绪且证明确为原状态时，只读复采最多三次，不重复点击。普通导航和筛选仍保留 600 ms，因为这些步骤尚无完整的状态就绪合同。
4. 本轮全量重测发现 Windows 把成色 S 输出成小写 s。只对原串严格 `成色([sSaAbBcC])` 后的单个 ASCII 等级统一大小写；同帧独立详情等级仍须吻合。原始词框和大小写证据保留，不补中文前缀、不替数字/同形字。
5. 原 journal prepared/dispatched/confirmed、已有金星保护、同商品同成色同价格同磨损且白转金回读不变。协调端总 roundtrip 继续计入 5 秒帧龄；每个 capture 的 metrics 单独计数，累计窗口不会重复加总。

## 实机运行入口

以下命令会执行正式配置对应的收藏，不是模拟。输出目录每次新建，正式截图不写盘。已确认关注自动保留；不接后续购买。

```powershell
Set-Location 'C:\Users\Administrator\Desktop\price'
$Output = 'artifacts/collection_trial_' + (Get-Date -Format 'yyyyMMdd_HHmmss')
.tools/ocr-runtime/Scripts/python.exe -X utf8 tests/manual/run_collection_observed.py `
  --snapshot artifacts/collection_speed/input/task_snapshot.json `
  --output $Output --seconds 900 --numeric-price `
  --local-title-ocr --local-price-ocr --fast-capture
```

上述 snapshot 冻结于本次 config；配置已改时需走现有 `collection_run_config.py` 冻结当前数据，不能继续拿旧快照覆盖新编辑。wrapper stdout 含内存图像，不能重定向保存。恢复未完成段加 `--resume <实际前段summary.json>`，并保持 snapshot 字节一致。

## 验证与回退

- 真正完成及当前 checkpoint：[live_result.json](../artifacts/collection_speed/live_result.json)。失败的 `run_02` 原样保留；它不是通过记录。
- 480 旧内存像素重读与后续真实新帧分别存证；旧图片没有落盘，也未当作新的动作凭证。
- 发布包、离屏 UI、SQLite、native/Python 回归与隔离包回退：[VERIFICATION.txt](../artifacts/collection_speed/VERIFICATION.txt)。详细命令、字面输出、状态及发布 SHA 均在文件内。
- 四项证据位于 `artifacts/collection_speed/`：`MODIFIED_FILE.exe`、`DIFF_FILE`、`VERIFICATION.txt`、`ROLLBACK.sh`。回退脚本只恢复发布程序文件，不撤游戏关注、不改用户配置；在另一个包副本实测，固定发布目录保留新版本。
- BBZPS 对照依据为原日志局部 ROI/联合匹配/后续成功提示；未运行原样本。参见 [设计和原日志定位](business_rebuild/13_collection_speed_design.md)。
