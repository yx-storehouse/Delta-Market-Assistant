# 存储契约：内存事件仓库与 PR10 QtSql 适配器

> 这份文档保留最初的**接口目标、事务边界和故障语义**。历史 E08/E11 记录的“未链接 QtSql”不再代表本轮源码：PR09 内存仓库与 PR10 `SqliteEventStore` 均已实现并验证，工程已链接 `Qt6::Sql`。实际 API、迁移、恢复、测试和发布结果看 [PR10 实现记录](../sqlite_store_pr10.md) 与 [当前进度](../../09_m1_progress.md)。文中旧称 M10 对应 PR10 存储票，不是另一个已完成的里程碑；常规 UI 的持久化接入仍在 PR11。

## 当前实现与初始目标的映射

| 初始目标 | PR10 实际落点 | 接入时注意 |
|---|---|---|
| `QtSqlEventStore` 命名示意 | `src/ledger/sqlite_event_store.h` 的 `SqliteEventStore` | 以实际头文件为准 |
| 本文接口伪代码 | `src/ledger/store_types.h` 的 `IEventStore` | 另有 `commitLedger()`；空返回使用 `VoidResult`，不是 `variant<void, ...>` |
| 完整 `storage_schema.sql` | 源码内的关系表 v1 与迁移到 v2 的恢复审计表 | 不直接执行冻结目标 DDL，不补造当前 draft 没有的字段 |
| 内存工作状态快照 | SQLite 通过独立连接查询已提交状态 | 暂存成功不能提前刷新 UI；内存仓库的工作快照不等于持久化确认 |
| 一般回滚目标 | SQLite 写失败整笔事务回滚并锁存 aborted | 调用方须显式 `rollback()` 后继续，不提交前半段操作 |
| 未决恢复扫描 | 默认重开恢复并记录审计；只读打开不执行恢复写入 | 明确未派发者取消；可能已派发者 Unknown，不重发 |
| UI 与配置集成 | PR08 QSaveFile 继续独立，正常 UI 暂未接 SQLite | PR11 再处理方案、记录和错误投影 |

本轮两种仓库都补强回执门槛：Unknown/ambiguous 不能直接 `commitLedger()` 为成功/失败；终态必须与已确认回执一致。以下章节保留设计目标；若伪代码与已实现类型不同，使用上表及 PR10 记录，不把规格示例直接当编译通过的代码。

## 1. 分层决定

M1 先使用 `InMemoryEventStore`，让纯状态机、回放和 UI 投影可以开发、测试和回退；M10 复用完全相同的接口接 QtSql/SQLite。**不能因为数据库尚未可用而把事件写进 UI 文本或逐帧日志。**

```text
RuntimeReducer / RunCoordinator
        ↓ append transaction
IEventStore  ← InMemoryEventStore (M1)
        ↑ same contract
QtSqlEventStore / SQLite (PR10)
```

存储层不负责 OCR、捕获、匹配、调度或输入动作；它只保存结构化事实和投影所需索引。UI 成功数必须来自已提交账本，不能来自“按钮点击”或 reducer 尚未提交的内存计数。

## 2. 事件和记录的身份

| 记录 | 唯一键 | 重复相同 payload | 相同键不同 payload |
|---|---|---|---|
| `business_event` | `event_id` 全局字符串 | 幂等返回原提交结果 | `EVENT_ID_CONFLICT`，事务回滚 |
| `attempt` | `attempt_id` | 返回当前记录，不重复预留/发起 | `ATTEMPT_ID_CONFLICT` |
| `intent` | `intent_id` | 不重复进入派发队列 | `INTENT_ID_CONFLICT` |
| `receipt` | `(receipt_identity, attempt_id)` | 幂等入账 | `RECEIPT_CONFLICT` |
| `ledger_transaction` | `transaction_id` | 返回原账本结果 | `TRANSACTION_ID_CONFLICT` |
| `quota_reservation` | `reservation_id` | 不重复占用 | `RESERVATION_CONFLICT` |

`seq`由协调器在本次事件流中分配、单调递增；数据库不按墙钟重排事件。`event_id`去重和`seq`顺序是两个约束。不同`session_id`/`clock_domain_id`可以写入同一账本，但必须保留来源，重启不复用旧运行/帧/租约ID。

## 3. M1 应实现的 C++ 接口

建议文件：`src/ledger/event_store.h`、`src/ledger/in_memory_event_store.cpp`、`src/ledger/store_types.h`。以下为接口契约，类型名可等价实现但语义不可弱化。

```cpp
namespace relink::ledger {

struct StoreError { QString code; QString message; QString entityId; };
template<class T> using StoreResult = std::variant<T, StoreError>;

struct AppendReceipt {
    QString eventId;
    QString transactionId;
    bool duplicate = false;
};

struct EventDraft {
    QString eventId, runId, sessionId, clockDomainId, stepId;
    qint64 seq = 0, atMonoMs = 0, cancelEpoch = 0, viewportGeneration = 0;
    QString type;
    QJsonObject payload;
};

class IEventStore {
public:
    virtual ~IEventStore() = default;
    virtual StoreResult<AppendReceipt> appendEvent(const EventDraft&) = 0;
    virtual StoreResult<AttemptRecord> reserveAttempt(const AttemptDraft&) = 0;
    virtual StoreResult<AttemptRecord> recordDispatch(const DispatchDraft&) = 0;
    virtual StoreResult<AttemptRecord> applyReceipt(const ReceiptDraft&) = 0;
    virtual StoreResult<LedgerSnapshot> snapshot(const QString& runId) const = 0;
    virtual StoreResult<RecoverySet> recoverUnresolved(const QString& sessionId) const = 0;
    virtual StoreResult<void> commit() = 0;
    virtual StoreResult<void> rollback() = 0;
};
}
```

`reserveAttempt → recordDispatch → applyReceipt`不能拆成三个对外不可见的 UI 更新。实现可在一个事务中完成一次阶段转换；任何失败必须返回稳定错误码，不能只在日志打印后继续。`AttemptRecord.state`与runtime schema一致：`Prepared/Dispatching/Sent/AwaitingReceipt/Unknown/Success/Failed/Cancelled`。

## 4. 事务边界和额度语义

### 4.1 预留

预留只在 runtime 已允许的意图进入 `Prepared` 时建立：

```text
confirmed_success + unresolved_reservations < explicit_quota_target
```

目标、周期、品级映射或单位未确认时，不把“没有配置”解释成无限；返回 `QUOTA_SCOPE_UNRESOLVED`。`Unknown`继续占用预留；明确`Failed`且已确认没有外部成功时才可释放；`Success`转为成功计数。`Cancelled`只有在**明确未派发**时释放，可能已派发的取消仍进入`Unknown`核验。

### 4.2 停止与迟到回执

- `Stop/Pause`只增加`cancel_epoch`并阻止新意图，不删除已准备或已可能发出的事实。
- `Prepared`且`dispatch_proof=not_dispatched`可转`Cancelled`并释放预留。
- `Dispatching/Sent/AwaitingReceipt`转`Unknown`，保留预留；迟到的`Receipt`仍可产生`LedgerCommitted`。
- `LedgerCommitted`在暂停/停止后仍更新账本，但不恢复run、不重开观察、不重新采集。
- 已提交的成功/失败投影只在事务提交后刷新UI；事务失败时UI保留旧快照并显示存储错误。

### 4.3 幂等与冲突

相同ID且规范化payload相同是幂等；相同ID而payload不同是数据冲突，不选择“最后写入”。收到重复Receipt时返回原`transaction_id`和`duplicate=true`。数据库/内存实现都必须具有相同的行为，不能只靠SQLite唯一索引而缺少内存测试。

## 5. 事务故障矩阵

| 故障点 | 允许持久化的结果 | 恢复后行为 | UI投影 |
|---|---|---|---|
| `reserve` 前崩溃 | 无预留 | 不产生attempt | 不变 |
| 预留写入后、事件提交前崩溃 | 事务必须全回滚；若无法确认则扫描未决事务 | 进入诊断，不重复预留 | 显示恢复中 |
| `recordDispatch` 已提交、进程退出 | Prepared/Sent事实可读 | 不自动重发；Unknown核验 | 未知保留 |
| Receipt写入前退出 | 已发起事实保留 | 重新观察/人工核验 | 不加成功 |
| Receipt与Ledger同事务提交后退出 | 两者均可读 | 重放幂等 | 成功/失败可投影 |
| Ledger提交失败 | Receipt可留作未决但不能伪造账本 | 重试存储或人工核验，不重发外部动作 | 显示保存失败 |
| 数据库损坏/新schema | 只读诊断、保留原文件 | 走备份恢复/回退 | 不清零历史 |

故障注入点由PR10测试创建；M1内存实现必须先有同样的状态oracle。SQLite WAL/检查点是实现选项，不代表应用自动获得跨进程不可丢失保证；关键事件仍需事务与一致性备份。[R13]

## 6. SQLite 目标表和部署边界

`storage_schema.sql` 给出M10目标DDL。它不是当前数据库文件，也不要求M1安装QtSql。建议开启外键、busy timeout和显式事务；连接创建后执行schema版本检查，禁止新程序静默降级旧结构。

M10发布前必须额外验证：

1. CMake实际链接`Qt6::Sql`，不是只在SDK发现`Qt6Sql.dll`。
2. 发布目录带匹配架构的 `Qt6Sql.dll` 和 `sqldrivers/qsqlite.dll`；`sqldrivers` 与 `platforms` 同级，干净 PATH 可加载。
3. `QSQLITE`打开、建表、事务、唯一约束、恢复扫描全部在发布目录运行；不能用开发机SDK路径代替。
4. PR12的package检查排除BBZPS原始EXE/DLL/插件，验证固定解压路径和回退副本。

## 7. M1/M10代码和测试落点

| 阶段 | 实现 | 必测 |
|---|---|---|
| M1 / PR09 | `InMemoryEventStore`、去重、预留、Unknown恢复集 | 28/30条runtime/domain黄金中涉及账本的用例；重复ID、失败/未知、Stop迟到回执 |
| M10 / PR10 | `QtSqlEventStore`、迁移、故障注入、备份 | 事务四个退出窗口、唯一键冲突、busy/full/readonly、重启核验、发布driver |
| PR11 | UI只读投影和错误显示 | UI数值与提交账本一致；存储失败不显示成功 |

配置仓库的QSaveFile路径与事件账本不同：配置提交失败保留旧配置，账本恢复不覆盖配置。截图、OCR像素和高频识别全文不进入事件表；只保存必要字段、版本、原因和可空的`image_ref`（默认null）。
