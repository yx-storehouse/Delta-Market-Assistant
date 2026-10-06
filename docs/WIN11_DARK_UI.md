# 打开石墨深色工作区

当前版本为 **0.5.0**。已解压的 Windows x64 桌面程序：

```text
C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe
```

直接打开该 EXE，同目录保留 DLL 和 `platforms` 文件夹。开发与验收使用 Qt offscreen；用户自行打开正式窗口。

## 视觉与布局

本次延续用户指定的微软应用商店 / Windows 11 方向，并将浅色改为石墨深色，而不是早期黑灰琥珀终端。

| 元素 | 当前实现 |
| --- | --- |
| 背景 | `#202124` 石墨深灰，避免纯黑大底 |
| 内容面 | `#2B2D31`，12 px 圆角，细边界与低强度阴影 |
| 强调色 | `#8CBFF1`，用于主按钮、选中标记及价格曲线 |
| 正文 | `#EDF0F5` / `#D5DAE2`；辅助信息使用 `#A0A8B5` |
| 控件 | 6–8 px 圆角，输入聚焦底线，独立悬停和禁用状态 |
| 导航 | 原位置与顺序保留，增加线性图标 |
| 关注行 | 连续圆角选择背景、轻量缩略图底板和浅蓝短标记 |
| 价格图 | 深色底、柔和蓝线、水平辅助线，去掉密集纵向网格 |
| 小窗口 | 右侧条件区允许独立纵向滚动，避免预览与价格文字重叠 |

关注目录、列表列、右侧最高价格/最大磨损/数量上限、创建任务、底部趋势和七个页面继续保留。数据层、模拟任务逻辑、原演示图集不变。

界面使用 C++17 + Qt 6.8.3 Widgets 和自绘图表。背景是应用绘制的静态颜色，不是 Mica / Acrylic；没有替换为 WinUI。普通 Windows 窗口和任务弹窗会请求系统深色标题栏及圆角，继续使用系统标题栏按钮与窗口行为。标题栏请求已编译；离屏验收不覆盖可见桌面标题栏的最终外观。

## 构建与复验

在项目根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test -Package
python artifacts/win11_dark_ui/verify_ui.py baseline
python artifacts/win11_dark_ui/verify_ui.py modified
python artifacts/win11_dark_ui/verify_ui.py rollback
```

验收包含七页的 1560×980 与 1180×820 逻辑尺寸、100% 与 150% 缩放、深色像素采样、正文/选中行/主按钮对比度、导航文字适配、输入焦点、下拉列表、任务弹窗与禁用状态。既有搜索、筛选、创建/编辑/删除、配置保存、CSV 和模拟运行检查继续执行。

`before/`、`after/`、`after_150/`、`restored/` 保存实际程序离屏截图；`VERIFICATION.txt` 保存确切计数、命令、输入、标准输出、退出状态和哈希。

## 查看修改与恢复上一版

本轮证据目录为 `artifacts/win11_dark_ui/`：

- `MODIFIED_FILE.zip`：当前源码与完整已解压发布目录的快照。
- `DIFF_FILE.diff`：0.4 → 0.5 的实际源码差异。
- `VERIFICATION.txt`：基线、修改、回滚和交付复核。
- `ROLLBACK.sh`：调用同目录 `restore_ui.py`，按保留哈希恢复 0.4。

默认回滚验收只修改 `rollback_test/` 副本，正式目录保留 0.5。需要实际恢复时，退出 Relink，在项目根目录执行：

```powershell
& 'C:\Program Files\Git\bin\bash.exe' artifacts/win11_dark_ui/ROLLBACK.sh .
```

恢复脚本先验证备份，再恢复源码和发布文件，只清理明确列出的新增主题文件与说明；用户应用配置不在恢复范围内。

## 后续继续设计时使用

```text
继续基于 Relink Studio 0.5 的石墨深色 UI 修改。
以 Win11 / 微软商店为参照：中性深灰层次、柔和圆角、细边界、克制的浅蓝强调。
保留关注列表、右侧条件编辑、底部趋势和七个页面；除非明确要求，不改数据逻辑。
不要重做 A/B/C 概念选择，也不要回到黑灰琥珀终端风格。
用实际程序的离屏截图检查 1560×980、1180×820 及 100%/150% 缩放。
修改后构建和测试，更新 dist/RelinkStudio，并给出已解压 EXE 的绝对路径。
不要自动启动可见窗口。
```
