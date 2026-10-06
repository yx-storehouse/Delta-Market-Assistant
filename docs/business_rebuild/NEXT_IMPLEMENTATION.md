# 下一轮开发交接任务：M1 业务模型与离线回放

下面的提示词可直接交给后续开发会话；只执行M1，不把计划误认作已实现。

```text
工作目录：C:\Users\Administrator\Desktop\price。
先读取AGENTS.md、SESSION_START.md、docs/business_rebuild/README.md、08_implementation_ready.md和implementation/README.md。
复用现有Relink Studio 0.6.0的C++17/Qt Widgets工程。

本轮已完成 PR01→PR06 与 PR09；下一阶段只推进 PR07、PR08、PR10、PR11、PR12，已完成的 PR06/PR09 以实现与测试证据为准，不重复规划。
1. 分离商品、卖单观测、任务规则、运行实例、动作尝试和事件类型。
2. 实现确定性的纯规则判断，输出Match/NoMatch/NeedsReview及逐字段原因。
3. 实现金额/磨损精确十进制比较；未知单位、歧义关联、过期观测不自动匹配。
4. 实现schema v1兼容及BBZPS 13列文本的只读导入预览。
   第7/8列价格为候选，第12/13列保留raw；时间参数不自动换算；不导入凭据。
5. 实现基于人工结构化JSON/历史事件片段的回放、可控时钟和状态记录。
6. 先实现回执账本、未决数量预留和重复事件去重；不把模拟成功混为实际成交。
7. 在现有界面接入模式、来源、观测时间、匹配原因与回放控制。
8. 用implementation/domain、implementation/runtime的固定schema/fixture，以及business_catalog.json/acceptance_cases.json登记实际实现和用例结果。

执行约束：
- 不运行、加载、调用或打包BBZPS目录中的EXE、DLL、脚本、插件和恢复载荷。
- 不接入真实点击/购买；UI继续只保存配置并展示后端回放状态。
- 不改系统时间；使用假时钟/单调时钟测试。
- 遵守ADR10/E12：步骤需要观察时才申请新帧，空闲/暂停/长等待不持续采集或OCR。
- 实时像素走内存缓冲区/共享内存，禁止PNG/JPEG临时文件、逐帧图片日志及磁盘文件IPC。
- 正常/成功/失败默认都不保存截图，只有用户主动导出或显式启用有界调试才允许。
- 配置和必要账本正常保存；日志限速汇总。共享内存失败不静默改走临时文件。
- Win11浅色白灰，不使用蓝色，保留商店布局和所有原功能入口。
- 优化入口保留未定义状态，不替用户发明内容。
- 不自动启动前台窗口。

验收和交付：
新增领域和迁移测试，执行现有build.ps1 -Test和-Package，离屏截图验收。
备份旧程序与旧配置后更新解压发布目录，保留DLL/platforms。
最终给出C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe，
列出实际通过的测试、未完成项和回退路径；不宣称M2/M3/M4完成。
```
