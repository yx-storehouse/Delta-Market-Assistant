# M2 总筛选同帧状态读取与原流程差异计划

2026-10-07。沿用原启动 S10–S14，不改变关注预检顺序；本模块不执行输入。

## 已实现

- `CatalogFilterReader` 绑定 `frame_id`、像素 SHA-256 和客户区坐标，读取实际赛季文字及已拥有、未拥有、四个品阶的三态值。缺字、缺框、模糊、不同帧、非当前校准尺寸均保留 Unknown。
- 当前校准仅覆盖 2560×1440、144 DPI；不把其他分辨率按比例猜成已校准。
- 空框要求四边包围与低方差暗色内部；选中框是实测的**白色内嵌方块**，不是预设绿色对勾。分类要求白色平坦中心、暗边、过渡区域比例同时成立。
- 全帧 OCR 后，同一帧的筛选区做 2 倍反色对比度预处理。普通品阶被选中后，明亮方块干扰宽区 OCR，必要时独立裁出文字、3 倍识别；不把错字替换成预期文字。
- `CatalogFilterPlan` 只接受明确的 UI 状态目标快照：先比较赛季，改变赛季后必须重读；再比较拥有状态，改变后必须重读；品阶是多选集合，匹配则保留，不匹配按 S13 清残留、S14 选目标。每一步有读取回执要求，不包含点击坐标或执行许可。
- 不从演示任务推断当前真实任务，不把单条 grade 条件擅自合并成原程序的任务分组；品阶和稀有度仍独立。

## 实测与恢复

实机校准仅临时勾选筛选复选框，未按“确认”，未购买、移动物品或修改关注。阶段结束时赛季仍为“全部赛季”，六个复选框全部恢复未选；每个批次前置游戏一次、结束恢复 IDE 一次。

证据分层：

| 证据 | 发现 | 后续实现位置 |
|---|---|---|
| `live_style_calibration.json` | 已拥有、史诗选中为白色内嵌方块；旧算法正确保留 Unknown | `classifyFilterCheckboxMeasurement` |
| `live_verified_states_v2.json` | 已拥有与史诗选中/取消、其他框保持值均通过 | 三态规则及数值信号 fixture |
| `live_remaining_states.json` | 普通品阶选中导致邻近文字读取失败；停止后有两项临时勾选未恢复 | 不把像素选中绕过标签校验 |
| `live_restored_interruption.json` | 整行 3 倍识别仍不能重建普通品阶文字 | 改为独立裁出文字区域 |
| `live_restored_interruption_v2.json` | 初版误用不支持的 4 倍参数，返回 `E_OCR_REGION`；零点击停止 | 改为已支持的 3 倍，不放宽 provider 参数上限 |
| `live_restored_interruption_v3.json` | 独立文字区识别成功，恢复两项临时勾选 | 同帧按需字段重读 |
| `live_final_filter_states.json` | 当前发布包验证传说、稀有、未拥有、普通及两品阶同时勾选；最终全部还原 | 最终二进制哈希绑定记录 |

`live_verified_states.json` 的期望赛季因 PowerShell 管道编码变为问号，首帧即拒绝、零点击。修正计划 UTF-8 内容后另存 v2，旧失败不覆盖。

`fixtures/catalog_filter_signals_20261007.json` 保存 84 组数值摘要，预期来自事先声明的联调计划，来源记录/帧/二进制有哈希。它不是游戏图片，不是独立标注的准确率留出集，不代表所有亮度、动画或窗口布局已验证。合成栅格测试另外覆盖白块、无边框、缺字段、非法数值、错帧和错误标签。

## 有界批次

为把四项状态的校准和恢复放在同一段前台，联调计划上限扩展为 20 步，总时限仍为 30 秒。不是每个动作切回 IDE。每次点击后须重新识别页面；期望字段断言绑定对应帧哈希。Unknown 页可有界重读，但字段失败不重放点击。

中途异常可能留下临时筛选勾选：先重新读取当前状态，再恢复有证据的已选框，记录恢复结果。用户切走前台时不强抢回来继续点击。

## 复验

```powershell
python -X utf8 docs\business_rebuild\scripts\export_catalog_filter_signals.py
python -X utf8 tests\release\verify_catalog_filter_package.py build
python -X utf8 tests\release\verify_catalog_filter_package.py verify
python -X utf8 tests\release\verify_catalog_filter_package.py finalize
```

固定解压交付为 `C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`。四个事务产物、完整命令与 literal 输出在 `artifacts/m2_catalog_filter/`。原包保留，回退只在隔离副本验收，正式交付保持新版。

## 剩余接口边界

- 只读筛选差异计划尚未连接正式游戏执行器；未来执行器必须保留品阶计划游标，不能每次勾选后重新生成“先清空”的计划，造成循环。
- 赛季字符串读取已实现；赛季下拉展开/菜单选项识别、枪种状态仍未实测。
- 原 `StartupObserver` 默认 1 秒时效没有修改。诊断的同帧多次 OCR 可能超过这一门槛，不能直接冒充异步运行时已经接通，更不能放宽价格、成交类证据时效。
- 当前本机保存的配置仍是 schema 1、`demo=true`，五条任务禁用。只读页面校准不激活这些演示任务。

继续开发提示词：

```text
读取 AGENTS.md 与 SESSION_START.md，保持静默连续开发。复用 CatalogFilterReader/Plan；不要重做页面、焦点和 OCR 坐标修复。
只在真实阻塞时用短句询问。用原 BBZPS 流程证据推进 S15 商品定位及 S17 标题复核，不能把演示任务当成真实执行目标。
同帧像素只走内存；整段联调结束才恢复 IDE。新批次重解析窗口身份，旧证据不可覆盖。
```
