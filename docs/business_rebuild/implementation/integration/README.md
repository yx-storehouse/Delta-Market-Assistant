# 集成契约入口

本目录回答“领域、运行时、存储和现有 UI 怎么接起来”，是进入源码实现前的最后一层边界。它不包含生产 C++；本轮只有接口、DDL、示例和验收规则。

| 文件 | 责任 |
|---|---|
| [ui_integration.md](ui_integration.md) | 现有 `MainWindow/AppState` 接入新 controller/projection 的位置、UI命令、模式、金额编辑和首纵切片 |
| [storage_contract.md](storage_contract.md) | M1 `IEventStore/InMemory`、M10 QtSql/SQLite、事件/attempt/回执/预留事务、故障恢复 |
| [storage_schema.sql](storage_schema.sql) | SQLite目标DDL；不在本轮执行 |
| [storage_contract.sample.json](storage_contract.sample.json) | 结构化事务与幂等示例；非实测结果 |

## 交叉边界

```text
domain Decision (是否匹配)
  → runtime Run/Attempt reducer (是否允许创建意图、状态如何变)
  → IEventStore (事实/回执/预留如何提交)
  → UI Projection (如何展示)
```

- `Match`不等于`eligible_for_action=true`；运行层还要检查模式能力、取消代次、观察新鲜度、额度和关联。
- `Pause/Stop`不抹除可能已发出的事实；迟到`LedgerCommitted`可以入账，但不恢复运行。
- 图像帧、OCR全文和逐帧日志不进入账本；默认`image_ref=null`，用户明确导出才有图片引用。
- M1只有Replay/Fake适配器；Observe配置可以保存但不能启动捕获/动作。

## 集成前检查

1. 先跑根 `validate_contracts.py`、readiness `validate_readiness.py` 和现有 `validate_docs.py`。
2. 再按 PR01→PR05 实现最小纵切片；PR09内存账本在PR04后可并行。
3. QtSql/SQLite只能在PR10引入；检查当前 `CMakeLists.txt`、实际 `build_relocated`、发布DLL/driver差距，不把SDK文件视为可交付依赖。
4. 每个实现票保存源码revision、实际buildDir、命令、stdout/stderr、退出码、fixture hash和断言数；不要用本目录的预期字段伪造“通过”。
