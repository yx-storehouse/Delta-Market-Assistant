"""Documentation-only intake: hash local evidence and fetch official reference metadata."""
from __future__ import annotations
import concurrent.futures
import datetime as dt
import hashlib
import html
import json
from pathlib import Path
import re
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
DOC = ROOT / 'docs/business_rebuild'
ART = ROOT / 'artifacts/business_rebuild_docs'
ART.mkdir(parents=True, exist_ok=True)
(DOC / 'evidence').mkdir(parents=True, exist_ok=True)

REFERENCES = [
 ('R01', 'OpenCV — Template Matching', 'https://docs.opencv.org/4.x/d4/dc6/tutorial_py_template_matching.html', 'matchTemplate', '固定图标/锚点定位的候选方法；不证明样本实际调用了模板匹配。'),
 ('R02', 'OpenCV — Image Thresholding', 'https://docs.opencv.org/4.x/d7/d4d/tutorial_py_thresholding.html', 'adaptiveThreshold', '对文字 ROI 的阈值处理做离线对照，原图仍需保留。'),
 ('R03', 'Microsoft — Screen capture', 'https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture', 'GraphicsCapture', 'Windows Graphics Capture 捕获能力和约束；作为新实现候选。'),
 ('R04', 'Microsoft — Desktop Duplication API', 'https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api', 'DXGI', '桌面帧捕获候选、更新与移动区域；与窗口捕获方案对照。'),
 ('R05', 'Microsoft — High-DPI desktop application development on Windows', 'https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows', 'DPI', '区分逻辑/物理像素，处理窗口和显示器 DPI 变化。'),
 ('R06', 'PaddleOCR — OCR pipeline', 'https://www.paddleocr.ai/main/en/version3.x/pipeline_usage/OCR.html', 'rec_scores', '检测框、文字和置信度输出，作为独立 OCR 候选评测依据。'),
 ('R07', 'PaddleOCR — Text recognition module', 'https://www.paddleocr.ai/main/en/version3.x/module_usage/text_recognition.html', 'PP-OCRv5', '按中文/数字场景评估识别模型；不是样本模型启用证明。'),
 ('R08', 'Qt 6.8 — QElapsedTimer', 'https://doc.qt.io/qt-6.8/qelapsedtimer.html', 'steady_clock', '使用单调经过时间而不是可回拨的墙上时钟计算超时。'),
 ('R09', 'Qt 6.8 — QTimer', 'https://doc.qt.io/qt-6.8/qtimer.html', 'accuracy', '定时器可能延迟；事件触发后重核截止时间，不承诺亚毫秒精度。'),
 ('R10', 'Qt 6.8 — QThread', 'https://doc.qt.io/qt-6.8/qthread.html', 'moveToThread', 'worker 与 GUI 的线程分工和 queued connections。'),
 ('R11', 'Qt 6.8 — QProcess', 'https://doc.qt.io/qt-6.8/qprocess.html', 'readAllStandardOutput', '可信独立视觉 worker 的进程通信、退出及错误处理。'),
 ('R12', 'Qt 6.8 — QSaveFile', 'https://doc.qt.io/qt-6.8/qsavefile.html', 'commit', '配置先写临时内容，成功后提交；保留原配置。'),
 ('R13', 'SQLite — Write-Ahead Logging', 'https://sqlite.org/wal.html', 'checkpoint', '账本持久化候选；事务、检查点和备份需单独设计。'),
 ('R14', 'Qt 6.8 — Supported Platforms', 'https://doc.qt.io/qt-6.8/supported-platforms.html', 'MinGW', '记录 Windows 编译器组合；第三方库也需匹配工具链。'),
 ('R15', 'Qt 6.8 — Qt for Windows Deployment', 'https://doc.qt.io/qt-6.8/windows-deployment.html', 'windeployqt', '打包 Qt DLL 和 platform plugins，检验干净环境运行。'),
 ('R16', 'IETF RFC 5905 — Network Time Protocol Version 4', 'https://www.rfc-editor.org/rfc/rfc5905.html', 'offset', '网络时间偏移与测量不确定度参考；新程序不修改系统时间。'),
]

def sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()

def fetch(row):
    rid, title, url, marker, use = row
    out = dict(id=rid, title=title, url=url, publisher=url.split('/')[2], use=use,
               checked_at=dt.datetime.now(dt.timezone.utc).isoformat(), required_marker=marker)
    try:
        req = urllib.request.Request(url, headers={'User-Agent': 'RelinkStudio-DocumentationReview/1.0'})
        with urllib.request.urlopen(req, timeout=35) as r:
            body = r.read(8 * 1024 * 1024)
            text = body.decode('utf-8', errors='replace')
            title_match = re.search(r'<title[^>]*>(.*?)</title>', text, flags=re.I | re.S)
            out.update(http_status=r.status, final_url=r.url, bytes=len(body), sha256=hashlib.sha256(body).hexdigest(),
                       page_title=html.unescape(re.sub('<[^>]+>', '', title_match.group(1))).strip() if title_match else '',
                       marker_found=marker.lower() in text.lower(),
                       status='verified' if r.status == 200 and marker.lower() in text.lower() else 'review')
            # Save metadata/keyword hit only, not a full-page republication.
    except Exception as e:
        out.update(status='fetch_failed', error=str(e))
    return out

def main():
    baseline = ART / 'project_baseline.json'
    if not baseline.exists():
        targets = [p for d in ['src', 'tests', 'dist/RelinkStudio'] for p in (ROOT/d).rglob('*') if p.is_file()]
        targets += [ROOT/n for n in ['AGENTS.md','CMakeLists.txt','build.ps1','resources.qrc','SESSION_START.md'] if (ROOT/n).is_file()]
        baseline.write_text(json.dumps({p.relative_to(ROOT).as_posix(): sha(p) for p in sorted(targets)}, ensure_ascii=False, indent=2), encoding='utf-8')
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        results = sorted(pool.map(fetch, REFERENCES), key=lambda x:x['id'])
    (DOC/'references.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
    print('PROJECT_BASELINE_FILES=' + str(len(json.loads(baseline.read_text(encoding='utf-8')))))
    for item in results:
        print(f"{item['id']} {item['status']} HTTP={item.get('http_status','-')} {item.get('page_title',item.get('error',''))}")
    return 0 if all(x['status']=='verified' for x in results) else 1

if __name__ == '__main__':
    raise SystemExit(main())
