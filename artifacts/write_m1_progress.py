from pathlib import Path

path = Path("docs/business_rebuild/09_m1_progress.md")
path.write_text(r"""# M1 实施进度与双基线

更新时间：2026-10-06（Asia/Shanghai）

## 当前结论

M1 首条业务纵切片已经完成：领域规则、Replay/Fake runtime、Replay UI 投影和离屏交互验收均已落地。当前分支仍严格保持“观察、回放、解释优先”的边界，不启动 BBZPS，不连接真实窗口，不执行 OCR、鼠标键盘输入、市场动作或购买。

## 已完成 PR

| PR | 状态 | 实际结果 |
|---|---|---|
| PR01 | 完成 | QtCore 领域库、测试目标与 CMake 集成 |
| PR02 | 完成 | 精确十进制值、商品观察、任务规则与结构校验 |
| PR03 | 完成 | Matcher 决策、原因码、优先级与 `eligible_for_action=false` |
| PR04 | 完成 | ReplayReducer、RunCoordinator、取消/暂停/停止、Unknown 与回执语义 |
| PR05 | 完成 | Overview 中接入 Replay 来源/状态/匹配说明/计数，以及开始、暂停、继续、停止控件 |

## PR05 验收范围

- `ReplayController` 只驱动 `RunCoordinator/FakeClock`。
- `UiProjection` 显示 `Replay` 来源、运行状态、匹配说明、观察请求、内存帧、确认成功和未决预留。
- Replay 分支与旧 `AppState` demo 分支隔离，不覆盖演示统计，不把运行快照写入现有配置。
- 图像只表达内存观察请求；`imageFileWriteCount` 保持为 `0`，没有临时图片文件 IPC。
- 现有 8 页顺序、页面 objectName、Win11 浅色白灰风格和收藏/任务/运行设置入口保持不变。

## 验证结果

```text
domain_tests          PASS
business_tests        PASS
business_value_tests  PASS
business_rule_tests   PASS
runtime_tests          PASS（15/15 Replay fixtures）
ui_offscreen           PASS（包含 REPLAY_UI_INITIAL/START/PAUSE/RESUME/STOP）
CTest                  6/6 PASS
```

固定发布目录已更新并用隐藏窗口离屏执行自测：

```text
C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe
PACKAGED_EXIT=0
offscreen=true; game_connected=false; system_input_sent=false
```

## 双基线

- `artifacts/business_rebuild_docs/project_baseline.json`：文档阶段原始基线，保留不覆盖。
- `artifacts/business_rebuild_docs/m1_pr01_pr04_baseline.json`：PR01–PR04 历史基线，保留用于审计。
- `artifacts/business_rebuild_docs/m1_pr05_baseline.json`：当前 M1 代码与发布产物基线，验证脚本优先读取该版本。
- `docs/business_rebuild/scripts/validate_docs.py`：同时验证原始基线差异是否被最新 M1 基线覆盖，以及最新 M1 基线自身是否发生未记录修改。

最近一次文档验证：

```text
DOCUMENTATION_CHECKS=PASS
BUSINESS_ITEMS=44
ACCEPTANCE_SPECIFICATIONS=88
OFFICIAL_REFERENCES_VERIFIED=16
EVIDENCE_FILES_HASH_VERIFIED=23
M1_BASELINE_ID=m1_pr05
M1_BASELINE_FILES=25
M1_BASELINE_FILES_UNCHANGED=25
M1_BASELINE_DIFFS=0
```

## 下一步

PR06–PR12 仍处于计划状态：事件存储、v1/13 列导入、SQLite 持久化、真实窗口捕获适配、OCR provider、共享内存租约和发布回归。进入这些阶段前，仍需保持默认不落盘、不执行外部动作，并为每个 provider 增加可回放 fixture 和离屏验收。
""", encoding="utf-8")
print(path)
