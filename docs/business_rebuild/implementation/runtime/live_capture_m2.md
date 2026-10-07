# M2：按步骤采集、隐藏OCR诊断与原业务流程对齐

日期：2026-10-07。当前业务实施依据为 [BBZPS首次启动逐步复刻](../../10_bbzps_first_startup_reconstruction.md)，不是普通物资交易行。这里仅描述已完成的底层模块，不把采集/OCR测试写成39步业务完成。

## 已实现与已实测分开看

| 项目 | 实现/验证状态 |
|---|---|
| DXGI采集 | 实现IObservationSource；真实游戏3帧通过，2560×1440、144DPI |
| 像素路径 | GPU侧裁剪目标客户区→CPU BGRA8→FrameEnvelope；没有图片文件路径 |
| 需求约束 | 单次或有界多帧；不可变demand；单请求在途；sticky取消；QPC域/截止时间/帧预算 |
| 帧检查 | PID/HWND/进程创建时间/窗口类；客户区/DPI/显示器；前台、遮挡、黑屏采样、格式/容量、源呈现时间 |
| IDE恢复 | QCoreApplication诊断显式创建Win32消息队列；临时关联前台线程；RAII恢复后实际读取前台HWND |
| Windows OCR | 通过IObservationRecognizer；隐藏PowerShell宿主+系统Windows.Media.Ocr；共享内存只读，有限超时 |
| OCR实际测试 | 合成内存画面“MARKET 123456”被真实系统OCR识别；中文/英文语言包在当前系统可用 |
| 真实枪皮识别 | 尚未完成。当前游戏画面曾观测为特勤处；该画面不证明已进入典藏业务 |
| 原启动流程 | 已重新扫描历史日志并细化S01–S39及入口分支；当前EXE还没有按这些步骤导航游戏 |

## 模块边界

- `target_window.*`：窗口身份、物理客户区、DPI、前台恢复、QPC时钟转换。
- `dxgi_observation_source.*`：一个源绑定一个demand和一个视口；每次capture建/释放D3D资源，不开常驻采集线程。
- `windows_ocr.*`：单次OCR诊断provider、输出结构校验、非个人化页面摘要；独立于业务规则。
- `src/vision/windows_ocr_worker.ps1`：只读paging-file mapping，构造SoftwareBitmap，调用系统OCR，返回文字框；不执行游戏输入。
- `diagnostics/live_capture_check.*`：显式CLI入口，短暂前置目标，恢复IDE，再处理所保留的内存帧。
- 正常主窗口、回放、配置与SQLite投影没有接入自动采集/自动交易；`vision_worker_fixture.exe`仍不打包。

当前OCR helper使用独立的一请求协议 `windows-ocr-once-v1`，**不是宣称已实现PR12B的持久worker hello/ready/release完整会话**。它复用SharedFrameMemory，等待进程退出后关闭mapping。持久worker服务未来可对接既有WorkerProtocol，但不在这次测试中冒充已完成。

## 诊断调用

先由操作者解析本次真实游戏/IDE窗口句柄和PID。诊断要求明确四个身份参数，不枚举并选择任意桌面；诊断入口限定游戏EXE与Mirasim名称。正常双击程序不会进入此分支。

```powershell
# 实测时身份参数来自Get-Process，不把下方变量当永久句柄。
$game = Get-Process DeltaForceClient-Win64-Shipping | Where-Object { $_.MainWindowHandle -ne 0 }
$ide = Get-Process Mirasim | Where-Object { $_.MainWindowHandle -ne 0 }
& .\dist\RelinkStudio\RelinkStudio.exe --live-capture-check `
  --target-hwnd $game.MainWindowHandle --target-pid $game.Id `
  --return-hwnd $ide.MainWindowHandle --return-pid $ide.Id --frames 3
```

`--frames`范围1–5。`--ocr`显式请求对最后一帧调用Windows OCR；默认stdout仅含帧元数据及OCR锚点摘要，不输出账户文字。`--preview-stdout`是显式调试预览，PNG仅写入内存QBuffer并经stdout传回；不是运行时像素IPC，也不写磁盘。实际OCR的像素仍只走共享内存。

退出码：0表示所请求的诊断成功且前台恢复；1表示采集或OCR失败；2表示参数或身份绑定失败（未改变前台）；3表示恢复IDE失败。JSON把 `capture_passed`、`ocr_passed`、`ide_foreground_restored`分开报告，避免将其中一个成功冒充全成功。

## 资源、超时与限制

1. 不支持旋转显示器、跨屏客户区、被遮挡或最小化窗口的本次DXGI采集，返回对应错误；没有PrintWindow/整桌面图片落盘兜底。
2. DXGI `LastPresentTime` 与需求使用QPC毫秒域；过滤请求前的呈现和仅鼠标更新，保留1ms保守转换不确定度。源时间是桌面呈现时间，不能独立证明游戏内订单刚发生变化。
3. AcquireNextFrame每段最多20ms；单次请求采集上限1500ms且不超过demand截止时间；最多600次内部取帧尝试；GPU Map使用非阻塞轮询。
4. Direct3D驱动创建/COM调用的底层阻塞不由应用精确控制，1500ms不是驱动挂死时的硬实时保证。UI没有同步调用此诊断，联调外层进程另有时间预算。
5. 每帧最多128MiB、边长8192；OCR输出最多1MiB/2000词，文本合计64KiB，边框必须落在帧内。OCR可用语言明确指定，不静默更换语言；系统OCR没有置信分数，不伪造置信度。
6. OCR默认8000ms，配置限制100–15000ms；超时终止并等待helper退出。OCR服务不是连续高频引擎，冷启开销未作吞吐优化。识别后的旧帧不得直接变成交易依据。
7. 采集前后做窗口/前台/遮挡检查，不宣称能证明两次检查间所有瞬时遮挡不存在；真正业务接入前仍需真实页面正反例和关联校验。

## 实际失败与修复

- 第二次临时预览出现 `E_FOREGROUND_ACTIVATION`，目标和返回IDE均未获得前台。原失败元数据保留，未改为PASS。
- 原因定位到无窗口QCoreApplication线程可能没有Win32消息队列；显式 `PeekMessageW`创建队列后，关联前台线程及目标线程，再切换并校验。
- 重试实际取到游戏帧，并在诊断结束时校验Mirasim HWND为前台。用户之后主动切换其它窗口不会被当成这次恢复失败，也不持续抢占前台。
- Windows PowerShell 5的DataWriter.DetachBuffer返回未类型化COM对象，首次OCR合成测试失败；改用WindowsRuntimeBufferExtensions.AsBuffer后真实OCR文字测试通过。

## 回归与证据

应用CTest现有21组，包括新增窗口/采集负向测试和真实系统OCR合成文字测试。原UI206项、工作区UI58项和SQLite22项继续验收。历史真实采集记录位于 `artifacts/m2_live_capture_transaction/live_capture_1.json`；预览仅保留元数据，未提交游戏画面或全文OCR。

底层模块更新没有改写原业务入口。下一步以S01–S39为准落实原页面分派和关注预检，避免继续由通用OCR样例反推业务路线。
