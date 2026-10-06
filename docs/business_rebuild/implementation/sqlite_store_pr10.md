# PR10 · SQLite 事件仓库、事务与恢复

更新时间：2026-10-06（Asia/Shanghai）。
状态：PR10 后端实现与验收完成。最终实测为 12/12 CTest、316 条 SQLite 断言、22 条发布存储自检；发布副本回退与恢复程序离屏复验均通过。当前 UI 接入进度及完整证据索引见 [实施进度](../09_m1_progress.md)。

**本票只增加后端结构化账本。** 正常 UI 未接 SQLite，ConfigV2 仍由 PR08 ProfileStore/QSaveFile 保存；没有接入真实捕获、OCR、键鼠或交易。Win11 浅色白灰界面与既有功能入口保持不变。图像不落盘不等于账本不落盘，必要业务事实仍按事务持久化。

交付复核额外修复了历史 UI 中文乱码（330 处字符串），不改布局与标识；新增三项可见文案离屏断言。最终完整构建记录为 `artifacts/m1_pr10_transaction/logs/final_build.log`，发布复验记录与最终 EXE 一致。

## 1. 从哪些文件继续开发

| 证据入口 | 当前实现 | 后续用途 |
|---|---|---|
| [sqlite_event_store.h](../../../src/ledger/sqlite_event_store.h) | SqliteEventStore、OpenOptions 与存储 API | PR11 controller 按实际签名接入 |
| [sqlite_event_store.cpp](../../../src/ledger/sqlite_event_store.cpp) | 关系表、迁移、事务、回执、恢复与备份 | 修改行为时同步契约与回归 |
| [store_types.h](../../../src/ledger/store_types.h) | IEventStore、draft、snapshot 和 StoreError | 与内存仓库共享类型 |
| [sqlite_ledger_tests.cpp](../../../tests/ledger/sqlite_ledger_tests.cpp) | 真实临时数据库、锁/满盘故障与退出窗口 | 验证持久性，而非仅 mock 返回码 |
| [storage_self_test.cpp](../../../src/ledger/storage_self_test.cpp) | 程序内的临时数据库自测 | 从发布目录验证 QSQLITE |
| [storage_contract.md](integration/storage_contract.md) | 初始事务与集成目标 | 对照本票实现差异 |
| [storage_schema.sql](integration/storage_schema.sql) | 冻结的完整目标 DDL | 规格参考，本票未直接执行该文件 |

证据→结论→开发落点：现有 IEventStore/PR09 测试确定业务身份与预留语义；SqliteEventStore 增加明确提交和重开恢复；PR11 应当在 controller 成功提交后投影快照，不直接执行 SQL，也不把临时测试数据库当用户数据路径。

## 2. 打开仓库与调用接口

命名空间为 relink::ledger。仓库不复制；创建、调用和销毁在同一线程。每个实例拥有命名写连接与独立只读查询连接；写实例另持有进程感知的 .writer.lock，避免另一活跃实例被误判为“退出后待恢复”。现存路径或父目录规范化用于锁名；硬链接别名、直接绕开该锁协议的外部写连接，不属于此单写者协议的保证。

| API | 返回值 | 语义 |
|---|---|---|
| open(path)、open(path, options) | VoidResult | 初始化/检查版本；默认执行重开恢复 |
| close() | VoidResult | 回滚未提交工作，释放连接和锁；不会隐式 commit |
| isOpen()、schemaVersion() | bool、int | 查询打开状态和当前版本，当前版本为 2 |
| appendEvent(EventDraft) | StoreResult<AppendReceipt> | 暂存结构化事件，身份/序号冲突明确报错 |
| reserveAttempt(AttemptDraft) | StoreResult<AttemptRecord> | 校验 quota 并暂存 attempt + reservation |
| recordDispatch(DispatchDraft) | StoreResult<AttemptRecord> | 保存派发事实，不执行外部动作 |
| applyReceipt(ReceiptDraft) | StoreResult<AttemptRecord> | 保存回执与待入账状态，不等于终态入账 |
| commitLedger(LedgerDraft) | StoreResult<LedgerTransactionRecord> | 校验已确认回执，暂存终态与预留转移 |
| commit()、rollback() | VoidResult | 提交整笔事务；或回滚并清除 aborted 状态 |
| snapshot(runId) | StoreResult<LedgerSnapshot> | 返回该 run 的已提交一致快照 |
| recoverUnresolved(sessionId) | StoreResult<RecoverySet> | 只读扫描已提交的占用预留，空 session 表示全部 |
| eventCount() | StoreResult<int> | 返回已提交事件数 |
| backupTo(destinationPath) | VoidResult | 生成一致副本并独立只读重开验证，不覆盖已有目标 |

OpenOptions 默认值与影响：

| 字段 | 默认值 | 影响 |
|---|---|---|
| readOnly | false | true 只读打开当前版本，不迁移、不恢复、不创建缺失 DB |
| busyTimeoutMs | 1000 | 传给 QSQLITE_BUSY_TIMEOUT；负值拒绝 |
| recoverOnOpen | true | 可写重开时执行恢复事务；false 仅用于明确控制恢复的调用方/测试 |

路径必须指向磁盘文件；拒绝空路径、目录、:memory: 与不存在的父目录。本票不选择最终 app data 位置，不在普通 UI 启动时自动新建用户数据库。

## 3. 实际 schema 与迁移

PRAGMA user_version 是唯一版本号来源；schema_meta 仅记录 schema_name=delta_market_assistant_ledger。两个值分别表达版本与库身份，不维护第二套相互竞争的版本计数。

| 关系表/索引 | 保存内容 | 约束 |
|---|---|---|
| schema_meta | 数据库身份 | 已识别 ledger 名称 |
| runs | run_id 与 session_id 映射 | 不补造 source_revision、模式或启动事件 |
| events | 事件身份、时钟/步骤/代次和 JSON payload | event_id 唯一；非空 run 下的 seq 包括 0 也唯一 |
| attempts | 意图、规则版本、状态、数量、派发证据与关联 | attempt/intent/reservation 身份和枚举约束 |
| quota_reservations | scope、数量、held/released/consumed/unresolved | attempt 与 reservation 一一对应 |
| receipts | outcome、association 与首次关联 event_id | receipt_identity + attempt_id 复合身份 |
| ledger_transactions | 确认的 Success/Failed 与回执 | 每 attempt 至多一条终态交易 |
| recovery_audit（v2） | 恢复前后状态、证据和原因 | 关联 attempt，并建立查询索引 |

版本路径：

1. 空库（user_version=0 且无用户对象）在同一迁移事务中建立 v1 关系表，再建立 v2 recovery_audit 与索引。
2. 已识别 v1 库先验证结构和数据一致性，再事务迁移到 v2。
3. v2 库核验预期 schema 对象、库身份、quick_check、foreign_key_check 及 attempt/reservation/receipt/ledger 的关系语义。
4. 新于 v2、非空未版本化、结构不符、关系损坏或非 SQLite 文件返回诊断，保留原文件，不删除重建。
5. 只读打开 v1 返回 SCHEMA_MIGRATION_REQUIRED，调用者不得静默切成可写模式。

与冻结 DDL 的区别：本票服务现有 draft 字段，仅保存可验证的 run/session 关系；dispatch、receipt、ledger 的 event_id 保留关联，但不会为当前接口缺失的创建事件等字段补造记录或强加错误外键。完整目标 DDL 仍保留原样，不宣称“全部目标表已照规格执行”。

结构检查按源码中的预期 SQL 对象进行规范化比对；人工改表即使大致相似，也可能返回 SCHEMA_INVALID。后续变更必须新建显式版本迁移，不修改 user_version 来绕过校验。

## 4. 暂存、提交与错误后的继续

首次写操作延迟开启 BEGIN IMMEDIATE；后续写操作加入同一事务。每个 API 返回业务结果，只表明当前操作已暂存成功；唯一持久化提交点是 commit() 成功。

~~~text
append / reserve / dispatch / receipt / commitLedger
  → 当前写事务中的暂存事实
  → commit 成功
  → 独立只读连接读取已提交 snapshot
  → 后续 PR11 才刷新 UI
~~~

- 任一写操作出现业务冲突或 SQL 失败，回滚整笔事务并锁存 aborted；commit() 返回 TRANSACTION_ABORTED，不保留前半笔操作。
- 调用方记录原始错误，显式 rollback() 清除 aborted 后，再决定是否以原幂等键重试存储；不会重新发送外部动作。
- COMMIT 失败保留 SQLite 原生错误映射，例如 DB_BUSY；不会把尚未提交的 Success 显示给 UI。
- SQLITE_FULL/IOERR 可能已由 SQLite 回滚整笔事务。实现识别“已无活动事务”的回滚结果；其它回滚失败关闭写连接，不继续提交。
- close()/析构回滚未提交工作。重开只看到已提交状态，不恢复暂存内存状态。
- 默认启用外键与 synchronous=FULL；未主动切换 WAL 模式。本票不把 WAL、同步选项或一次进程退出测试等同于断电永不丢失保证。[P10-R3、P10-R5]

PR09 内存仓库用于工作状态和回滚检查点；PR10 SQLite 查询采用 **committed-only**。相同 IEventStore 接口不表示每种实现的暂存可见性一致，UI 必须依赖明确提交边界。

## 5. 幂等、额度与回执

- event_id 相同且全部业务字段/规范化 JSON 一致时返回 duplicate；同键不同内容返回 EVENT_ID_CONFLICT。JSON 对象键顺序不制造冲突。
- run + seq 独立约束顺序，不按墙钟重新排序。run/session 不同绑定返回 RUN_SESSION_CONFLICT。
- attempt/intent/reservation 的身份及业务字段必须一致；quotaTarget 必须显式且 scope 已解析。Unknown 保留占用；成功消耗预留，确认失败释放预留。
- 同状态/同 proof 的重复 dispatch、同身份/同 outcome/association 的 receipt、同业务字段的 ledger 重放，允许再次观测带来不同 event_id，但保留首次事件关联，不为重复观测创建新交易。
- Unknown 或 ambiguous 回执只能保留待核验状态；直接提交 Success/Failed 返回 RECEIPT_UNCONFIRMED。
- 已确认 success 回执与 Failed 终态、或 failed 回执与 Success 终态，返回 RECEIPT_OUTCOME_MISMATCH。
- 后两项门槛同步到 PR09 内存实现；本轮不保留“Unknown 可直接记成功”的旧行为。

## 6. 重开恢复与受控退出

默认可写 open 完成 schema 检查/迁移后，在独立恢复事务中处理已提交、仍占用预留的未决事实：

| 已提交状态 | 恢复结果 | 预留和审计 |
|---|---|---|
| Prepared + not_dispatched | Cancelled | 释放预留；记录 prepared_not_dispatched |
| Dispatching / Sent / AwaitingReceipt 等可能已派发状态 | Unknown | 保留预留为 unresolved；记录 possible_external_dispatch |
| 已经 Unknown | 保持 Unknown | 不重复追加恢复审计 |
| 已经 Success / Failed / Cancelled | 保持终态 | 不重复入账或重启运行 |

recoverUnresolved() 是只读查询；open 的恢复事务才改变恢复状态。两者不自动采集、重发或继续 run。

当异常退出留下热 rollback journal，单纯只读连接可能因需要恢复而报告 DB_READ_ONLY。实现只在可写打开路线中核对 journal，复制 DB+journal 到 QTemporaryDir，让 SQLite 先恢复副本并验证 schema/关系语义；副本验证通过后，才允许 SQLite 在原 DB 上执行原生恢复。失败返回 DB_RECOVERY_REQUIRED、DB_IO_ERROR 或原始诊断；不手工删除 journal，不以空库覆盖原库。

这条路线解决实际进程中断后的恢复，并不宣称覆盖真实断电、驱动/硬件故障、网络文件系统或所有 SQLite journal_mode 组合。默认模型是本机文件与单写者协议。

## 7. 备份和回退

backupTo() 通过 VACUUM INTO 建立已提交的一致副本，不直接复制可能依赖 journal 的主文件；完成后用独立只读连接重开，验证当前版本、schema 与数据一致性。[P10-R4]

- 活动事务返回 TRANSACTION_ACTIVE；aborted 返回 TRANSACTION_ABORTED，先 commit 或 rollback 再备份。
- 目标已存在返回 BACKUP_EXISTS；父目录必须存在；不覆盖已有备份。
- 备份中断可能留下不完整输出；只有 backupTo() 返回成功并通过独立重开核验的文件才作为恢复源。
- 恢复副本应在关闭仓库后使用另一路径重开验证，保留原文件与证据，不在运行中覆盖 DB。
- 回退程序与回退数据库分开处理：旧程序沿用独立 v1 演示配置，v2 ledger 原文件保留；旧程序不解释新 schema。
- 本票没有自动云备份、备份轮换、修复任意损坏数据库或 schema 降级功能。

## 8. 调用方需要处理的错误

| 类别 | 主要错误码 | 处理方式 |
|---|---|---|
| 生命周期/线程 | STORE_CLOSED、STORE_ALREADY_OPEN、WRONG_THREAD、STORE_IN_USE | 在同线程管理生命周期；另一写实例仍活跃时不执行恢复 |
| 路径/驱动 | INVALID_PATH、DB_NOT_FOUND、DRIVER_UNAVAILABLE、DB_OPEN_FAILED | 保留错误，修正路径或发布依赖后重开 |
| SQLite 故障 | DB_BUSY、DB_FULL、DB_READ_ONLY、DB_IO_ERROR、DB_CONSTRAINT、DB_ERROR | 不显示成功；先核实 rollback，再限次重试或呈现诊断 |
| 版本/损坏 | SCHEMA_TOO_NEW、SCHEMA_INVALID、SCHEMA_UNRECOGNIZED、SCHEMA_MIGRATION_REQUIRED、DB_CORRUPT、DB_RECOVERY_REQUIRED | 保留原文件，选择明确迁移或已验证副本 |
| 事务/备份 | TRANSACTION_ABORTED、TRANSACTION_ACTIVE、BACKUP_EXISTS | 清除失败事务或结束事务，使用新备份目标 |
| 业务身份 | EVENT_ID_CONFLICT、EVENT_SEQ_CONFLICT、ATTEMPT_ID_CONFLICT、INTENT_ID_CONFLICT、RESERVATION_CONFLICT、TRANSACTION_ID_CONFLICT | 不使用“最后写入覆盖”修复冲突 |
| 额度/回执 | QUOTA_SCOPE_UNRESOLVED、QUOTA_FULL、RECEIPT_CONFLICT、RECEIPT_UNCONFIRMED、RECEIPT_OUTCOME_MISMATCH | 保留原事实与未决预留，不提升为确认成交 |
| 投影范围 | SNAPSHOT_OVERFLOW | 不截断计数；报告结构化诊断 |

完整校验错误见源码；StoreError 同时包含 code、message 与 entityId。错误码是调用契约，不要求 UI 显示数据库内部 SQL 文本。

## 9. 本轮验证矩阵

以下结果来自本轮真实构建和测试日志，不以规格示例或 mock 错误代替 SQLite 结果。主证据目录为 artifacts/m1_pr10_transaction；最终全量日志为 logs/modified_test.log，SQLite 明细为 logs/sqlite_ledger_tests.log，发布与回退命令原文为 package_commands.json。

| 场景 | 验证方法 | 当前结果 |
|---|---|---|
| 身份/seq=0、规范化 JSON、重复业务键 | 同临时文件上的真实 SQL/唯一约束与 API 调用 | 通过 |
| 提交前后可见性、成功/失败/Unknown/ambiguous | 独立查询连接对照快照，检查回执门槛 | 通过 |
| rollback、close/reopen、Prepared/Sent 恢复 | 关闭真实连接后重开，检查预留与审计 | 通过 |
| readonly / busy / COMMIT 失败 | 只读连接、外部 BEGIN IMMEDIATE 写锁、外部读锁 | 通过 |
| 单写者/只读并存 | 第二写实例返回 STORE_IN_USE；只读访问不取消活跃 Prepared | 通过 |
| SQLITE_FULL | 限制测试库 max_page_count 后写入大 payload，不填满系统磁盘 | 通过 |
| v1→v2、future/corrupt/schema/关系损坏 | 包括只读 v1 拒绝迁移、缺唯一索引、伪 Success 无账本的拒绝与哈希保留 | 通过 |
| 一致备份与未决预留 | VACUUM INTO 后独立重开 | 通过 |
| 四个进程退出窗口 | QProcess 运行同测试 EXE 的 crash child，以 std::_Exit 退出 | 通过 |
| 发布目录 QSQLITE | 已解压 EXE 的 --storage-self-test，包内 DLL/插件路径 | 22 条断言通过，exit=0 |
| 全量应用与离屏 UI | build.ps1 -Test、-Package 与 --self-test | 12/12 CTest；打包/UI 均 exit=0 |
| 发布副本回退 | 真实旧发布依赖恢复、哈希复核、再次离屏启动 | 通过；新数据库与新版发布保留 |

受控退出窗口为 before_transaction、before_commit、after_commit、before_receipt；子进程分别使用预定非零退出码，父测试以重开结果对照 oracle。before_commit 使用 cache_size=4、cache_spill=ON 与 4 MB payload，让 rollback journal 真正落盘；父进程观察到 62,976 字节与非零 journal header。四个退出窗口属于真实子进程退出测试，**不等同于真实断电试验**。

关键原始输出：

~~~text
100% tests passed out of 12
SQLITE_LEDGER_TESTS=PASS; assertions=316; failures=0; real_database=true; crash_processes=4; external_actions=0
CRASH_HOT_JOURNAL_BYTES=62976; header=d9d505f920a163d7
STORAGE_SELF_TEST=PASS; driver=QSQLITE; temporary_data=true; system_input_sent=false; assertions=22
UI_SELF_TEST=PASS; offscreen=true; game_connected=false; system_input_sent=false
ROLLBACK_RESTORED=PASS; new_databases_preserved=true
RESTORED_HASH=PASS; baseline_unchanged=true; modified_file_retained=true; new_database_preserved=true
PR10_PACKAGE_VERIFICATION=PASS
~~~

复现命令（项目根目录；直接 CTest 需使用构建脚本设置的工具链/PATH）：

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test
& .\.tools\aqt-env\Scripts\ctest.exe --test-dir build_relocated -R sqlite_ledger_tests --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Package
~~~

包内存储自测独立入口：

~~~powershell
$env:QT_QPA_PLATFORM = 'offscreen'
& 'C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe' --storage-self-test
~~~

上述存储入口本轮实际输出 STORAGE_SELF_TEST=PASS，assertions=22，退出码 0。完整 stdout、输入、命令与退出状态分别见 package_commands.json 的 BASELINE、MODIFIED、MODIFIED_UI、ROLLBACK、RESTORED 记录；这些阶段均 exit=0。测试输入均为人工构造或临时数据库，不使用 BBZPS 可执行组件。

## 10. 发布依赖与 PR11

CMake 新增 relink_sqlite 静态目标并链接 Qt6::Sql；程序只通过独立 --storage-self-test 调用持久化自测。正常主窗口尚未选择持久化目录或持有 SqliteEventStore。

build.ps1 -Package 显式部署 Qt6Sql.dll 与 sqldrivers/qsqlite.dll；qt.conf 将 Prefix/Plugins 设为发布目录。打包自测收紧 PATH 并指定包内插件路径，避免由开发 SDK 的插件搜索路径掩盖缺失依赖。最终交付仍是：

~~~text
C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe
~~~

PR11 继续完成 profile/revision/review 状态、已提交快照、恢复记录、错误提示与 CSV 导出投影；PR12 再做完整端到端回归。可复用任务文本见 [NEXT_IMPLEMENTATION.md](../NEXT_IMPLEMENTATION.md)。每票例行构建和更新发布目录，不等于 PR12 已完成。

## 11. 官方依据

以下官方页面于 2026-10-06 通过 HTTP 200 核查标题与相关条目；它们解释技术行为，不替代本项目测试。Qt 6.8 页面当前标题为 6.8.9；本机 SDK 仍为 6.8.3，本轮未升级 Qt。

| 编号 | 官方资料 | 本票使用点 |
|---|---|---|
| P10-R1 | Qt 6.8 QSqlDatabase | 命名连接、线程生命周期、commit/rollback |
| P10-R2 | Qt 6.8 SQL Database Drivers | QSQLITE、BUSY_TIMEOUT 与 OPEN_READONLY |
| P10-R3 | SQLite Transaction | BEGIN IMMEDIATE、SQLITE_BUSY、错误后的事务处理 |
| P10-R4 | SQLite VACUUM | VACUUM INTO 一致副本及中断不完整风险 |
| P10-R5 | SQLite PRAGMA | user_version、quick_check、foreign_key_check、synchronous |

~~~text
P10-R1 https://doc.qt.io/qt-6.8/qsqldatabase.html
P10-R2 https://doc.qt.io/qt-6.8/sql-driver.html
P10-R3 https://www.sqlite.org/lang_transaction.html
P10-R4 https://www.sqlite.org/lang_vacuum.html
P10-R5 https://www.sqlite.org/pragma.html
~~~
