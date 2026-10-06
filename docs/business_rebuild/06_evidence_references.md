# 06 · 证据与参考资料

## 1. 证据来源与复验

本轮复用上一轮已完成的静态资料，没有重新扫描约1.77GB日志、没有启动样本或加载其组件。证据索引保存的是实际文件SHA-256；本轮工作为开发设计，旧报告里的静态载荷分析不作为新程序代码来源。

| E编号 | 来源 | 可支持的业务发现 |
|---|---|---|
| E01 | source_manifest.json | 初始文件身份、cv2依赖线索；不证明所有依赖均执行 |
| E02 | pe_summary.json | TomatoOCR导出/资源、授权层接口、Python/Qt生态 |
| E03 | task_schema.json / configs_redacted.json | 15条13列任务、可见运行参数、脱敏配置 |
| E04 | log_index.json / business_evidence.json / business_passages.json | 页面/筛选/收藏/关注/倒计时/确认/异常与原文件行号、字节偏移 |
| E05 | embedded_payload.json / nested_pe.json | 旧外层与内层及额外载荷的关系；仅用于解释不搬二进制的决定 |
| E06 | decoded_pe.json / decoded_api_calls.json | 上一轮静态恢复的证据摘要，不纳入实现 |
| E07 | ida_survey.json / 原反编译证据 | 旧载荷代码能力已静态确认；开发沿用结论，不延伸载荷分析 |
| E08 | domain.h/.cpp / mainwindow.cpp / CMakeLists.txt / STORE_UI.md / build.ps1 | 当前前端/模拟实现、字段、构建工具链 |
| E09 | source_audit_verification.json | 上一轮目录结束复核情况；34张历史PNG缺失，原因未核实 |
| E10 | gdi32.dll.interesting.json | OpenCV、ppocrv5、TomatoOCR字符串偏移 |
| E11 | AGENTS.md | 最新Win11浅色白灰、全部功能入口和交付路径约定 |
| E12 | 用户补充的采集要求记录 | 按步骤触发，图像默认只走内存；取代临时帧文件IPC与常驻全速采集方案 |

原文件完整路径与哈希见 [evidence/index.json](evidence/index.json)；业务消息及原日志定位见 [business_excerpts.json](evidence/business_excerpts.json)；动态参数区间等补充片段定位见 [passage_locators.json](evidence/passage_locators.json)。[13列配置快照](evidence/task_schema.json)、[OCR身份摘录](evidence/ocr_identity.json) 和 [上一轮完整静态报告](evidence/static-analysis-report.md) 随文档附带，均为文本资料。

v0.2新增来源：[用户的步骤采集与不落盘要求](evidence/2026-10-06_capture_requirement.md)。该项是产品要求，不是样本已具备低IO实现的证明。

复验一组证据（只读哈希，不执行被检查文件）：

```powershell
python -I -X utf8 docs\business_rebuild\scripts\validate_docs.py --evidence E04
```

索引的source_ref以项目根目录为基准；上述复验需在本项目中进行。压缩包中的离线文档与摘录可直接阅读，但不包含原始日志或任何样本EXE/DLL。原始scope/timeline可在项目 `artifacts/bbzps_static/` 查阅。本轮入口与文档检验记录在 `artifacts/business_rebuild_docs/`。

## 2. Evidence → Finding → Path

| 发现 | 证据 | 结论与强度 | 开发影响 |
|---|---|---|---|
| BF01 | E03+E04 | 高：存在筛选收藏/关注调度两阶段，不只是价格展示 | WI01/03/09/10 |
| BF02 | E01+E02+E10 | 高：包含OpenCV及TomatoOCR接口；具体启用模型仍未知 | WI06/07；不复制OCR DLL |
| BF03 | E08+E11 | 高：现版本为配置与合成数据模拟，原入口需要保留 | WI01/05；不以演示成功充真实业务 |
| BF04 | E03+E04 | 中：价格列、延迟区间存在语义候选；边界/单位/公式未确定 | WI02；D01–D04 |
| BF05 | E04 | 高：结果超时、下架、识别失败、市场暂停是主流程分支 | WI04/09/10/11 |
| BF06 | E05+E07 | 高：旧发行包有独立远控代码静态证据 | WI12使用独立依赖，不打包原二进制 |
| BF07 | E12 | 用户确认：按步骤采集、实时图片默认不落盘 | ADR10、WI06–WI08/WI10；协议、统计与验收同步更新 |

业务路径 BP01（历史行为还原）：E03任务 → E04页面/筛选 → E04价格和磨损匹配 → E04收藏回执 → E04关注/倒计时 → E04确认/结果；支撑BF01/BF05。新实现的账本、取消代次、进程隔离等是设计加固，不冒充上述历史代码路径。

识别路径 BP02（接口/日志综合判断）：E02 OCR接口 + E10 OpenCV线索 → E04文字/框/分数 → E04页面分类与字段判断；支撑BF02。此路径是组件/数据关系，不代表已经恢复每一个函数调用。

## 3. 官方技术参考资料

以下是原始维护者的开发文档/标准，不是搜索摘要或二手博客。访问日期：2026-10-06。16项均实际取得HTTP 200并检查主题关键字；URL、最终地址、页面标题、获取时间、响应长度和SHA-256见 [references.json](references.json)。Qt链接属于6.8文档分支，当前页面可能显示6.8.x较新补丁号，**不代表本机Qt已升级**。

资料解释重构技术，不证明样本实际调用该API，也不是OCR性能达标证据。页面响应哈希是当次取证标识，不是将来网页永不变化的承诺。

| 编号 | 维护者 / 文献标题 | 对应业务与用途 | 核查 |
|---|---|---|---|
| **R01** | OpenCV — Template Matching | 固定图标/锚点定位的候选方法；不证明样本实际调用了模板匹配。 | HTTP 200 / verified |
| **R02** | OpenCV — Image Thresholding | 对文字 ROI 的阈值处理做离线对照，原图仍需保留。 | HTTP 200 / verified |
| **R03** | Microsoft — Screen capture | Windows Graphics Capture 捕获能力和约束；作为新实现候选。 | HTTP 200 / verified |
| **R04** | Microsoft — Desktop Duplication API | 桌面帧捕获候选、更新与移动区域；与窗口捕获方案对照。 | HTTP 200 / verified |
| **R05** | Microsoft — High-DPI desktop application development on Windows | 区分逻辑/物理像素，处理窗口和显示器 DPI 变化。 | HTTP 200 / verified |
| **R06** | PaddleOCR — OCR pipeline | 检测框、文字和置信度输出，作为独立 OCR 候选评测依据。 | HTTP 200 / verified |
| **R07** | PaddleOCR — Text recognition module | 按中文/数字场景评估识别模型；不是样本模型启用证明。 | HTTP 200 / verified |
| **R08** | Qt 6.8 — QElapsedTimer | 使用单调经过时间而不是可回拨的墙上时钟计算超时。 | HTTP 200 / verified |
| **R09** | Qt 6.8 — QTimer | 定时器可能延迟；事件触发后重核截止时间，不承诺亚毫秒精度。 | HTTP 200 / verified |
| **R10** | Qt 6.8 — QThread | worker 与 GUI 的线程分工和 queued connections。 | HTTP 200 / verified |
| **R11** | Qt 6.8 — QProcess | 可信独立视觉 worker 的进程通信、退出及错误处理。 | HTTP 200 / verified |
| **R12** | Qt 6.8 — QSaveFile | 配置先写临时内容，成功后提交；保留原配置。 | HTTP 200 / verified |
| **R13** | SQLite — Write-Ahead Logging | 账本持久化候选；事务、检查点和备份需单独设计。 | HTTP 200 / verified |
| **R14** | Qt 6.8 — Supported Platforms | 记录 Windows 编译器组合；第三方库也需匹配工具链。 | HTTP 200 / verified |
| **R15** | Qt 6.8 — Qt for Windows Deployment | 打包 Qt DLL 和 platform plugins，检验干净环境运行。 | HTTP 200 / verified |
| **R16** | IETF RFC 5905 — Network Time Protocol Version 4 | 网络时间偏移与测量不确定度参考；新程序不修改系统时间。 | HTTP 200 / verified |

## 4. 资料地址

- **R01**：`https://docs.opencv.org/4.x/d4/dc6/tutorial_py_template_matching.html`
- **R02**：`https://docs.opencv.org/4.x/d7/d4d/tutorial_py_thresholding.html`
- **R03**：`https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture`
- **R04**：`https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api`
- **R05**：`https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows`
- **R06**：`https://www.paddleocr.ai/main/en/version3.x/pipeline_usage/OCR.html`
- **R07**：`https://www.paddleocr.ai/main/en/version3.x/module_usage/text_recognition.html`
- **R08**：`https://doc.qt.io/qt-6.8/qelapsedtimer.html`
- **R09**：`https://doc.qt.io/qt-6.8/qtimer.html`
- **R10**：`https://doc.qt.io/qt-6.8/qthread.html`
- **R11**：`https://doc.qt.io/qt-6.8/qprocess.html`
- **R12**：`https://doc.qt.io/qt-6.8/qsavefile.html`
- **R13**：`https://sqlite.org/wal.html`
- **R14**：`https://doc.qt.io/qt-6.8/supported-platforms.html`
- **R15**：`https://doc.qt.io/qt-6.8/windows-deployment.html`
- **R16**：`https://www.rfc-editor.org/rfc/rfc5905.html`

## 5. 对应关系怎样使用

- 业务矩阵每行都有E/R/T：E说明为什么列此业务，R说明实现时参考什么，T说明做完怎样验收。
- 同一R可能为多项业务提供工程背景；它不会替代未恢复的业务公式。例如R08/R09只能解释时钟/定时器，不能证明clicktime单位。
- 参考实现由本项目重写；本轮没有咨询样本授权网站、连接控制端或下载样本依赖。
- 主要未确定项见 [决策表](07_decisions.md)，按文档明确缺口继续开发，不把检索资料填成样本事实。
