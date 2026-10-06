# UI 接入契约：保留现有界面，替换数据与命令来源

> M1/M2开发规格，不是本轮已修改的界面。事实基线：`src/mainwindow.h/.cpp`、`src/domain.h/.cpp`、CMakeLists.txt；证据E08/E11/E12。新设计由控制器隔离识别与运行，不在窗口槽函数中增加业务循环。

## 1. 现状与接入点

现有`MainWindow`持有`AppState*`，构造时把`AppState::changed`连接到`refreshAll()`；首页和任务页的开始按钮都调用`startSimulation()`。商品、日志、任务当前来自公开的演示集合。`MainWindow`已经有独立的build/refresh方法，适合逐页接入投影视图，不需要改Win11白灰布局。

| 现有位置 | 保留内容 | 新接入职责 | 验收关联 |
|---|---|---|---|
| `buildOverview/refreshOverview` | 统计卡、开始/暂停入口 | 显示当前模式/run_id/状态；按钮发控制器命令，不直接调用OCR/输入 | B02、B37、B43 |
| `buildFavorites/refreshFavorites` | 关注列表、搜索/过滤、右侧详情 | 演示模式仍可展示Skin；回放/观察模式绑定卖单投影，标识数据来源和观测时间 | B20、B38 |
| `refreshFavoriteDetail/createInspectorTask` | 商品详情、价格/磨损/数量/成色编辑 | 投影保留商品ID与卖单关联；创建规则不把屏幕位置写成订单ID | B04、B16、B20 |
| `buildTasks/editTask` | 任务表格及编辑对话框 | 规则修订号、启用与Review状态分开；保存不修改既有run快照 | B04、B39 |
| `buildRunSettings/refreshRunSettings` | 全部原运行参数入口 | 保存配置和显示是否已接入；未定义策略只保存，不因勾选而激活后端 | B23–B28、B32–B36、B40 |
| `buildPrices/refreshPrices` | 图表及导出 | 标明合成/回放/观察数据，观测价不是成交价；缺数据保留空档 | B38、B43 |
| `buildStats/refreshStats` | 统计面板 | 使用run聚合的匹配/收藏/发起/成功/失败/未知，禁止沿用模拟成功总数 | B30、B37 |
| `buildLogs/refreshLogs` | 等级、检索和列表 | 订阅结构化关键事件；高频帧统计在内存汇总后低频刷新 | B35、B37 |
| `importConfiguration/exportConfiguration` | 右上角入口和文件选择 | v1旧解析器与v2新导入服务分开；先预览再应用，原配置备份 | B05、B39 |
| `showHelp` / 优化入口 | 现有入口 | 帮助解释模式/单位/未决字段；优化保留待定义说明 | B40、B41 |

## 2. 控制器和投影的最小边界

建议新增文件路径属于计划：`src/application/application_controller.h/.cpp`、`src/application/workspace_projection.h/.cpp`、`src/ui/business_bindings.h/.cpp`。首个纵切片继续保留旧AppState以保证demo回归；新controller不反向调用`simulateTick()`来假装业务完成。

| UI命令 | 必备输入 | 结果 | 拒绝/冲突处理 |
|---|---|---|---|
| StartReplay | profile_id、revision、fixture_set_id | 接受后返回run_id与规则快照 | 已有活动run不创建第二个；未知fixture/未审规则给稳定错误码 |
| PauseRun | run_id、expected_cancel_epoch | 暂停投影；撤销观察/新意图 | 已暂停幂等；已可能发出的操作结果保持待核验 |
| ResumeRun | run_id、当前模式 | 新代次进入重新观察 | 不恢复旧定时器、帧位置或共享内存租约 |
| StopRun | run_id | 停止接收新工作，显示未决attempt数 | 停止不是外部回执取消；晚到回执只核验账本 |
| ApplyImportPreview | preview_id、已确认的映射、源文件哈希 | 原子写新配置成功后切换视图 | 预览后源文件改变，重新导入；验证失败保留当前状态 |
| SaveRuleRevision | task_id、expected_revision、完整草案 | 新revision与字段校验结果 | 过期revision冲突，不静默覆盖 |
| RequestImageExport | 仍有效的内存帧句柄、目标路径、明确用户动作 | 单次导出反馈 | 帧已释放则提示已过期，不伪造截图、不常驻自动保存 |

命令名是应用层接口建议；运行状态/事件词汇以`../runtime`规范为准，不在UI另造一套同义枚举。控制器返回结构化错误和字段定位，UI负责中文映射，日志保留稳定代码。

投影至少包含`mode/run_id/state/cancel_epoch`、任务规则快照版本、卖单行、匹配主原因/次原因、观测时间与来源、成功/失败/未知计数及`image_saved=false`标识。UI只渲染不可变投影；显示刷新可合并，但账本关键事件不可丢弃。

## 3. 金额与磨损编辑

现有界面使用`QDoubleSpinBox`。新的精确十进制领域值不应通过double往返后覆盖原值。首阶段提供字符串格式化/解析适配层：读取时保留原十进制字符串，用户未编辑时原值往返；超出当前控件可表达精度的字段使用文本编辑或只读精确值，不悄悄四舍五入。

业务比较使用领域`DecimalValue`，UI显示精度仅决定显示，不改变阈值。价格下限大于上限、单位未确定、磨损超精度等在提交前显示字段级错误。现有v1 double产生的兼容问题由导入规范处理，UI不自行推断转换策略。

## 4. 模式与控件状态

| 状态 | 用户可做 | 控件应说明什么 |
|---|---|---|
| demo | 使用当前演示、编辑演示配置 | 价格/统计是合成数据，不能成为真实运行历史 |
| replay | 选择结构化fixture，开始/暂停/单步/停止 | 当前fixture与逻辑时间；不连接目标窗口，不执行输入 |
| observe（后续M2） | 在步骤需求下只读观察 | 当前目标/采集状态/字段来源；默认图像不落盘 |
| 配置待审 | 继续编辑、预览差异、确认映射 | 哪些字段尚未确认；开始按钮不绕过Review |
| 已暂停/长等待 | 编辑下一版规则、停止、查看记录 | 没有持续截图/OCR；计时等待与观察步骤分开 |
| 结果未知 | 看已知字段/原因/未决预留 | 不显示成功图标，不自动释放额度；按恢复策略核验 |

观察/动作尚未实现的功能，入口保留但反馈“参数已保存，当前模式未接入”；不删除原UI入口，也不让其静默执行。系统使用浅色白灰与中性近黑强调，不借状态提示引入蓝色主题或终端外观。

## 5. 首个可交付纵切片

用户选择一份人工回放fixture → 运行控制器创建run → 规则引擎给出Match/NoMatch/NeedsReview与原因 → 关注行和右侧详情显示该次观测 → 关键事件列表显示规则版本和原因 → 暂停/恢复/停止可用 → 导出记录标明Replay。此阶段不需要截图/OCR，不需要未确认的旧延迟公式。

对应开发票见`../readiness`。此切片之后再接导入确认、持久化账本和视觉worker；UI所依赖的DTO/命令边界应保持一致。

## 6. UI与发布验收

- 用现有domain_tests保护v1读取和demo；新增纯规则/控制器测试不能改名冒充旧测试通过。
- 离屏检查首页、运行、任务、关注、记录页；同款两个卖单不会被合并为同一行，长名称/未知值/Review原因不遮挡操作。
- 状态变化可能来自worker线程，但窗口控件只在GUI线程更新；关闭窗口后控制器取消订阅，已结束run的更新不覆盖新run。[R10]
- 现有构建脚本可能选`build_relocated`，验收从实际build目录取测试与截图，不能固定读取旧`build`结果。
- 代码交付仍更新解压路径`C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe`，保留DLL/platforms，由用户自行打开。当前本轮只更新文档。
