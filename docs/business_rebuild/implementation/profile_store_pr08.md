# PR08 profile store and reviewed ConfigV2 commit

更新时间：2026-10-06（Asia/Shanghai）

## 目标

PR08 把 PR06/PR07 的 LegacyImportPreview 接入一个独立的 profile 仓库边界。预览仍然是只读数据；只有在用户明确提供完整 ReviewChoices 后，系统才会在内存中 materialize 一个 ConfigV2 文档，并通过 QSaveFile 原子提交到新路径。

当前实现不启动真实运行、不连接 BBZPS、不执行 OCR、鼠标键盘输入或购买动作。提交的规则默认保留 enabled=false，并写入 x-activation_required=true，因此 profile 保存成功不等于业务执行已启用。

## 输入与输出

输入：

- LegacyImportPreview；
- profile 名称；
- 单位 quantum；
- 商品、价格区间、taxonomy、未知列保留和数量不绑定确认；
- 可选的现有 ConfigV2 文档，用于同一 profile revision 递增。

输出：

~~~text
schema_version = 2
kind = ConfigV2
profile.id
profile.name
profile.revision
source.format
source.encoding
source.sha256
run_settings
rules
review_decisions
extensions.x-target_mode = demo
extensions.x-live_market_data = false
extensions.x-activation_required = true
~~~

## 提交语义

~~~text
READ_ONLY_PREVIEW
    ↓
CHECK_PREVIEW_ERRORS
    ↓
CHECK_REVIEW_CHOICES
    ↓
MATERIALIZE_CONFIG_V2
    ↓
QSaveFile::open
    ↓
write complete JSON
    ↓
QSaveFile::commit
    ↓
publish committed result
~~~

任一阶段失败时：

- 当前内存配置不变；
- 已存在目标文件不变；
- 不切换 profile；
- 返回稳定错误信息；
- 不生成半提交配置。

## API

实现文件：

~~~text
src/config/profile_store.h
src/config/profile_store.cpp
tests/business/profile_store_tests.cpp
~~~

核心接口：

~~~cpp
ProfileStoreResult materializePreview(
    const QJsonObject& preview,
    const ReviewChoices& choices,
    const QJsonObject& existing = {});

bool load(
    const QString& path,
    QJsonObject& document,
    QString* error = nullptr);

bool commitPreview(
    const QJsonObject& preview,
    const ReviewChoices& choices,
    const QString& path,
    QString* error = nullptr,
    ProfileCommitResult* committed = nullptr);
~~~

## 校验边界

拒绝以下情况：

~~~text
PREVIEW_INVALID
PREVIEW_NOT_READ_ONLY
PREVIEW_HAS_ERRORS
REVIEW_INCOMPLETE
SOURCE_HASH_INVALID
REVISION_INVALID
invalid ConfigV2 target
QSaveFile open/write/commit failure
~~~

source_sha256 必须是 64 位小写十六进制字符串；同一 profile ID 再次提交时 revision 递增，revision 小于 1 或达到 INT_MAX 时返回 REVISION_INVALID；首次提交从 revision 1 开始。规则保存为 reviewed snapshot，但仍保持 enabled=false 和 activation_required=true，由后续 PR11/PR12 决定 UI 投影和运行许可。

## 验收矩阵

| 用例 | 结果 |
|---|---|
| 合法 preview materialize | 通过 |
| schema v2 / kind=ConfigV2 | 通过 |
| 首次 profile revision=1 | 通过 |
| 同 profile revision 递增 | 通过 |
| source_sha256 原样保留 | 通过 |
| 非十六进制或大写 SHA-256 | 拒绝 |
| revision 溢出/非法 | 拒绝并返回 REVISION_INVALID |
| 未完成 ReviewChoices | 拒绝 |
| preview 携带 committable=true | 拒绝 |
| invalid existing target | 拒绝且目标文件保持原样 |
| QSaveFile 原子写入 | 通过 |
| 规则不会自动 enabled | 通过 |
| activation_required 保留 | 通过 |

## 下一步

PR10 将复用同一 profile/ledger 边界接入 QtSql/SQLite；PR11 再把 profile revision、review 状态和 ledger 投影到 UI。当前 PR08 不把 ConfigV2 提交结果直接切换为运行中的 AppState。
