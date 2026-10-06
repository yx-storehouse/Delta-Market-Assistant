# 打开 0.4 工作区

这是 0.4 浅色版的历史记录。当前 0.5 按用户追加要求改为石墨深色，说明与验收入口见 `WIN11_DARK_UI.md`。

本机已解压程序：

```text
C:\Users\Administrator\Desktop\price\dist\RelinkStudio\RelinkStudio.exe
```

用户自行打开。开发和验收使用 Qt offscreen，未自动弹出前台窗口。

## 这次改变了什么

0.4 按用户追加的微软应用商店 / Windows 11 和 IDE 截图调整视觉，而不是继续沿用黑灰色交易终端外观。

- 浅灰背景与两块白色工作面分层；关注列表和条件区保留原有功能位置。
- 白色工作面使用 10 像素圆角和很轻的阴影，控件采用 5–7 像素圆角。
- 列表减少横线；选中行由自定义 delegate 绘制完整的浅蓝圆角底和短蓝色标记，而不是每格独立高亮。
- 输入框使用轻边框及聚焦底线；操作按钮收敛为蓝色主操作和弱化的次要操作。
- 缩略图、条件区预览、数字字体及图表统一到浅色环境；预览按设备像素比缩放，保留透明留白，不裁切枪械边缘。

现有 C++ / Qt Widgets 框架继续使用。本轮没有替换为 WinUI 控件，也没有接入系统 Mica 或 Acrylic 壁纸采样材质；当前浅灰背景是应用绘制的静态背景。这里采用的是参照截图的视觉层次和几何风格。

## 工作流保持不变

类型、关注状态、品级、任务关联筛选，右侧创建任务，配置导入导出，任务增删改，CSV 和本地模拟运行继续保留。原数据模型及 48 项测试未修改。

演示图集、皮肤名称组合、价格和执行结果仍是本地演示内容，不代表实时市场或真实账户记录。

## 验证和回滚

在项目根目录构建、离屏测试并更新已解压发布目录：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test -Package
python artifacts/fluent_ui/verify_ui_refresh.py baseline
python artifacts/fluent_ui/verify_ui_refresh.py modified
python artifacts/fluent_ui/verify_ui_refresh.py rollback
```

最后一条命令在 `artifacts/fluent_ui/rollback_test` 副本上恢复 0.3 并测试，实际工作目录保留 0.4。哈希、命令、字面输出及退出码见 `artifacts/fluent_ui/VERIFICATION.txt`。

本轮检查包括原有 131 项 UI 检查，以及浅色调色板、预览图边界、分层工作面和圆角选中行实际像素检查。七页分别在标准与紧凑尺寸、100% 与 150% 离屏缩放下验证；这不是可见桌面上的人工交互验收。

需要恢复上一版时，先退出正在运行的 Relink，再在项目根目录运行：

```powershell
& 'C:\Program Files\Git\bin\bash.exe' artifacts/fluent_ui/ROLLBACK.sh .
```

恢复范围为本轮保留的源码、项目说明与发布程序，不涉及用户应用配置。完整历史版本和图集保存在本轮 `baseline` 目录。
