# 跨契约审查记录

> 2026-10-06，开发/测试视角。审查对象是本轮文件，不是运行中的后端。结论分“已在规格解决”“仍是后续代码验收”“真实产品语义未决”；不存在全系统Ready结论。

## 本轮已解决的实质冲突

| 编号 | 原缺口/冲突 | 本轮核对后的处理 | 对开工的影响 |
|---|---|---|---|
| CR01 | v0.2没有多失败优先级 | domain/RULE_ENGINE.md明确V0/P1–P7，缺磨损+可靠超价给NeedsReview；同层原因顺序固定 | PR03有可断言语义，不再由实现自行选顺序 |
| CR02 | Match容易被理解成可点击 | Domain `eligible_for_action=false`；quantity只是候选；Runtime需独立模式/额度/关联/代次门 | PR03独立开工，不把未知D05隐藏成无限 |
| CR03 | disabled预览与可执行配置混用 | 独立import-preview.schema与config-v2.schema；committable=false；审定/materialize/全量校验后新路径提交 | PR06/07可做保真预览；PR08不自动启用 |
| CR04 | v1迁移反例只给patch，基线留待未来寻找 | 增加v1.synthetic.json、origin哈希；I07–I09明确base_ref/base_sha256及operations顺序 | 迁移输入不再依赖运行时随机/当前默认值 |
| CR05 | Observe+autoCollect=true会命中收藏guard | Runtime增加只读优先及阶段/adapter能力门；M1 Observe不可启动；即便有配置开关也不授权动作 | 防止在接UI时越过“只读”边界 |
| CR06 | 取消后立刻回收可能覆盖worker仍在读的内存 | run Paused/Stopped与资源draining分开；lease需release或确认进程退出，超时仅隔离 | M2能按独立资源状态实现，不靠一个布尔running |
| CR07 | 子序列oracle允许意外额外事件 | 至少首RT-01增加exact_events；其余required子序列配精确计数/forbidden；建议PR04逐步扩大完整oracle | 最小静态切片有严格事件比较；仍须代码增加组合测试 |
| CR08 | Pause后一律丢事件可能丢已提交账本事实 | Runtime新增RT-15：Receipt提交→Pause→LedgerCommitted；接受真实事务事实，保持Paused、不新采集 | PR09/10不把视觉迟到和持久化提交回执混为一类 |
| CR09 | build/发布验收可能错用旧构建 | 当前build缓存指旧工作区，build_relocated指当前；PR01/12绑定buildDir+hash；verify_delivery需参数化 | 不阻塞纯域开发；阻塞未经修正的发布证明 |
| CR10 | SQLite方案没有落到CMake/部署 | 首纵切片InMemory；PR10为QtSql/SQLite适配器，Sql/qsqlite部署单列 | 持久化为独立票，不借SDK已有DLL宣称完成 |

## 固定的接口对应关系

| 边界 | 唯一语义权威 | 调用者必须做的事 |
|---|---|---|
| 配置/规则/观测/Decimal | `../domain/core.schema.json`、`README.md`、`RULE_ENGINE.md` | C++解析和语义校验后才进入matcher；不能只过JSON Schema |
| preview→新配置 | `../domain/IMPORT.md`及两个配置schema | 检查源hash/revision、保留raw、全量提交；旧schema1不改义 |
| run/step/epoch | `../runtime/state_machine.md`与`state.schema.json` | 单线程仲裁；模式/能力门先于业务动作；账本提交回执单独路径 |
| 观察需求→帧→worker | `../runtime/observation_contract.md`与`worker_protocol.md` | 按步骤、deadline和frame预算；共享内存租约；默认图像写0 |
| 事件/attempt→持久化 | `../integration/storage_contract.md` | 内存与SQLite共用语义；只有commit成功才更新成功投影 |
| 状态→原界面 | `../integration/ui_integration.md` | 保留商店布局、白灰无蓝；模式/来源明确；不把slot函数变业务循环 |

老版 `examples/vision-result.sample.json` 继续作为概念说明；实施使用runtime v1，不能混合 `generation` 等旧草案名字。命名的代码目录只是建议落点，不是第二套协议；PR01由工程负责人统一后记录，不同时建立两套等价控制器。

## 仍需诚实保留的缺口

1. **字段schema检查不等于语义通过。** 引用存在、单位quantum、min≤max、observed/evidence一致性、UTF-16长度等仍需C++/契约语义测试。Domain golden声明expected不是把业务引擎提前做完。
2. **完整转换表不代表所有路径已经有可执行oracle。** 15个回放场景已能开始实现关键路径，PR04/09/10需补组合、重复、乱序、边界及真实提交故障。不能用只包含Stop的exact case宣称完整两阶段状态机已完全覆盖。
3. **v1纯JSON反例已闭合，不代表真实GB18030/坏字节测试已完成。** PR07仍须按固定字节构建编码、长度、BOM/续行/注释负例并记录hash。
4. **M1是假源，E12真实IO结论待M2。** 跟踪整个应用进程树的图像/临时帧创建及字节，不只搜PNG文件名；配置/账本的必要写入单计。
5. **D03/D04/D05/D07/D09/D10/D11/D13仍有产品/环境未知。** 合成fixture的单位、quota bucket、时间和参数不是实际市场规则。
6. **代码基线没有变。** 新目录schema、票据、DDL和例子不会自行给0.6.0添加截图/OCR/账本能力；下轮仍需构建、测试和解压目录发布。

## 最终开工建议

- **可以开工：** PR01→PR02→PR03→PR04→PR05，按[门槛](readiness_gates.md)逐票验证；新契约冻结为实现输入，合成数据可用。
- **可并行但分票：** PR02后的PR06/07迁移；PR04后的PR09内存账本；不要把尚未提交的两套模型同时接UI。
- **不能直接宣称可投产：** M2视觉provider/质量与真实捕获IO，M3真实动态和限额，M4真实动作。其对应决定和实测未完成；这不阻碍第一条纵切片。

用这套文档开始开发是“首阶段条件已具体到可检验”，不是“后续所有问题已预先解决”。
