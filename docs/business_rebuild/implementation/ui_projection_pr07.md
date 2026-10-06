# PR07：13 列导入预览的只读 UI projection

> 文档状态：implementation-ready（2026-10-06，Asia/Shanghai）  
> 适用范围：M1 / PR07。本文定义 13 列文本导入预览如何投影到 Win11 浅色界面；不把预览结果升级为可执行配置，也不定义真实窗口捕获、OCR、鼠标键盘输入或购买动作。

## 1. 目标与非目标

### 目标

PR07 把 `preview13Columns()` 的结构化只读结果接入应用层 projection，使用户能够：

1. 选择文本/INI 输入并看到源文件摘要（编码、源 SHA-256、行数）；
2. 按行查看 13 个原始列、候选映射和逐行诊断；
3. 清楚区分“候选值”“未知值”“需要人工复核”和“阻止提交”；
4. 在预览页切换筛选（全部、错误、需要复核、重复行）而不改变输入；
5. 在 UI 中看到导入预览永远不可运行的状态。

### 非目标

- 不写入 `AppState`、活动 run、profile 仓库或 SQLite；
- 不生成 v2 可执行配置，不启用任务，不推断第 12/13 列含义；
- 不把第 7/8 列候选价格直接当作已确认边界；
- 不执行 INI 中的路径、命令、插件或载荷；
- 不调用 BBZPS，不捕获真实窗口，不做 OCR，不发送输入；
- 不保存 PNG/JPEG 或逐帧临时文件。

## 2. 输入与输出契约

| 层 | 输入 | 输出 | 约束 |
|---|---|---|---|
| Adapter | `QByteArray` 原始字节、可选 `ImportOptions` | `LegacyImportPreview` JSON 对象 | 输入只读；记录编码、源哈希和原始 token |
| Projection | `LegacyImportPreview` | `ImportPreviewView`（内存 DTO） | 不改变 preview；诊断顺序稳定 |
| Qt UI | `ImportPreviewView` | 表格、状态徽标、摘要卡片 | 只渲染和筛选；没有隐式提交副作用 |
| Command | `OpenImportPreview` / `FilterDiagnostics` / `ClosePreview` | UI 状态变化 | `ApplyImportPreview` 属于 PR08，PR07 只显示禁用态 |

projection 至少保留以下顶层字段：

```text
kind=LegacyImportPreview
source_format=text_13_columns | ini_13_columns
source_encoding=utf8 | utf8_bom | utf16le_bom | gb18030_explicit | unknown
source_sha256=<64 hex>
source_line_count=<integer>
committable=false
preview_only=true
```

每一行必须保留：

```text
row_number
raw_line
raw_columns[13]
candidate_mapping
diagnostics[]
enabled=false
review_required=true
```

`raw_line`、`raw_columns` 只用于审计和人工判断；UI 可以脱敏显示未知字段，但 projection 不得丢失原始值。

## 3. 13 列显示与语义边界

| 列 | UI 标签 | 展示方式 | 业务边界 |
|---:|---|---|---|
| 1 | 商品/任务标识 | 原值 + 稳定 ID（若可映射） | 仅候选标识，不自动启用 |
| 2 | 品类/市场 | 原值 | 未知 taxonomy 标记 review |
| 3 | 品级 | 原值 | 不替换为新 taxonomy |
| 4 | 关键词/名称 | 原值 | 保留原始 token |
| 5 | 成色区间 | 原值 | 不改变精度或区间 |
| 6 | 规则附加字段 | 原值 | 未知字段进入诊断 |
| 7 | 价格下限候选 | 十进制规范化 + 原 token | 候选；未确认前不可执行 |
| 8 | 价格上限候选 | 十进制规范化 + 原 token | 候选；反转时生成错误 |
| 9 | 成色/浮点参数 | 规范化值 + 原 token | 精度错误需要复核 |
| 10 | 规则标记 | 原值 | 不执行动态延迟或命令 |
| 11 | 来源/备注 | 脱敏文本 | 不显示敏感原文之外的推断 |
| 12 | 未知尾列 A | 原位 raw | 禁止猜作 quantity |
| 13 | 未知尾列 B | 原位 raw | 禁止猜作 quantity |

第 7/8 列支持千分位十进制（如 `1,000`），内部显示使用稳定十进制格式；第 12/13 列必须原位保留，即使为空。缺列、超列、坏分组、价格反转、重复行和未知 taxonomy 均作为逐行诊断，不得静默修正。

## 4. Projection 状态机

```text
Closed
  --OpenImportPreview--> Loading
Loading
  --preview_ready--> PreviewReady
  --preview_error--> PreviewError
PreviewReady
  --FilterDiagnostics--> PreviewReady
  --ClosePreview--> Closed
  --ApplyImportPreview--> RejectedByPR07 (command disabled)
PreviewError
  --ClosePreview--> Closed
```

`PreviewReady` 允许复制诊断和导出人工审阅文本（如产品最终需要），但 PR07 不允许保存为活动配置。任何 `committable=true`、`enabled=true` 或 `review_required=false` 的投影都属于契约错误，应在 projection 单元测试中拒绝。

## 5. UI 组件与交互

### 摘要区

- Mica 浅灰背景 `#F3F3F3`，内容层 `#F9F9F9`，白色卡片；
- 显示“只读导入预览”标题和 `预览模式 · 不可运行` 中性徽标；
- 显示编码、源哈希（可复制）、行数和诊断计数；
- 不使用蓝色；状态仅使用黑灰文字、细灰边框和中性强调色。

### 行表格

- 默认列顺序为原始 13 列，左侧增加行号和状态；
- 状态分为 `通过候选`、`需要复核`、`错误`、`重复`；
- 单击行打开右侧详情，详情同时展示 raw line、候选映射和 diagnostics；
- 长文本使用省略显示，悬停/详情中提供完整值；
- 第 12/13 列不折叠成数量、队列或延迟字段。

### 禁用操作

`应用到方案`、`启用任务`、`开始运行`、`购买` 等操作在 PR07 均不存在或处于禁用态。若复用现有按钮容器，必须显示禁用原因：`PR07 仅提供只读预览，需 PR08 审定提交`。

## 6. 测试与验收矩阵

| 编号 | Given | Then | 不证明 |
|---|---|---|---|
| PR07-A | 15 条正常 13 列输入 | 行号、原值、13 列、源 hash 全部保留；`committable=false` | 不证明列 12/13 的业务含义 |
| PR07-B | 12 列和 14 列输入 | 每行错误诊断；不可提交；不丢原始行 | 不证明可自动补列 |
| PR07-C | UTF-8/UTF-8 BOM/UTF-16LE/显式 GB18030 | 编码识别稳定；坏字节逐行诊断 | 不证明系统默认编码 |
| PR07-D | 千分位、价格反转、浮点噪声 | 合法值规范化；反转/精度错误进入 review | 不证明旧边界是闭区间 |
| PR07-E | 重复行和未知 taxonomy | 显示重复/未知诊断；候选保持禁用 | 不证明重复行可合并 |
| PR07-F | 任意 preview projection | UI 不能改变 `AppState` 或活动 run | 不证明 PR08 的审定提交 |

建议测试入口：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test
ctest --test-dir build --output-on-failure -R "business_migration_tests|ui_offscreen"
```

离屏自测必须保持：

```text
game_connected=false
system_input_sent=false
imageFileWriteCount=0
committable=false
```

## 7. 实现切片与交付清单

建议实现文件（可按当前工程命名调整，但职责不可合并到 adapter）：

```text
src/application/ui/import_preview_projection.h/.cpp
src/application/ui/import_preview_controller.h/.cpp
tests/business/import_preview_projection_tests.cpp
tests/ui/import_preview_offscreen_tests.cpp
```

实现顺序：

1. 从 `preview13Columns()` 构造不可变 projection DTO；
2. 先完成状态徽标、摘要和 13 列表格的离屏渲染；
3. 加入筛选与详情面板，不接入提交命令；
4. 增加 PR07-A 至 PR07-F 负例和 AppState 不变断言；
5. 运行 CTest、离屏自测和文档校验；
6. 将实际新增文件写入新的 `m1_pr07_*_baseline.json`，不得覆盖 PR01–PR06/PR09 历史基线。

## 8. Evidence → Finding → Path

| Evidence | Finding | Path |
|---|---|---|
| E03 15 条 13 列任务快照 | 尾列语义未知，必须原位保留 | `src/config/v1_adapter.cpp` / projection raw columns |
| E04 历史价格与规则片段 | 第 7/8 列只能作为候选 | `docs/business_rebuild/implementation/domain/IMPORT.md` |
| B05/T05-A、T05-B | 预览必须可审计且不可提交 | `tests/business/migration_tests.cpp` + PR07 UI tests |
| G04 迁移门槛 | 预览与 runnable 配置必须分离 | `implementation/readiness/readiness_gates.md` |
| D01/D02/D12 | 不猜尾列、价格边界和历史枚举 | 本文第 3 节与第 6 节 |

## 9. PR07 完成定义

PR07 只有在以下条件全部满足后才能从“计划中”改为“完成”：

- projection 不修改 `AppState`、profile 或活动 run；
- 13 列、raw line、行号、编码、源 hash 在 UI 和测试中可见；
- 候选、未知、重复和错误状态可筛选，且诊断顺序稳定；
- `committable=false`、`enabled=false`、`review_required=true` 在所有行上成立；
- `business_migration_tests`、PR07 projection tests、`ui_offscreen` 全部通过；
- 不运行 BBZPS、不写临时图片、不发送系统输入；
- 新建 PR07 基线和事务验证 artifacts，旧基线保持不变。

