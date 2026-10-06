# 使用终端工作区

Relink Studio 0.3 将「关注」设为启动页。黑灰工作面、琥珀选中态、顶部导航和连续三栏布局按用户选中的截图实现，不是截图覆盖在程序上的静态界面。

## 打开和操作

打开 `dist/RelinkStudio/RelinkStudio.exe`。发布目录中的 DLL 和 `platforms` 文件夹需保留在一起。

1. 在左侧按已关注、未关注、武器类型或任务关联筛选条目。
2. 使用上方搜索框、品级及关联状态筛选进一步缩小列表。
3. 选择条目后，右侧显示对应演示插图、价格和已有任务条件，底部更新 24 个合成价格样本。
4. 在右侧填写最高价格、最大磨损、数量上限，点击「创建任务」。这会增加一条启用的本地模拟任务，不会操作游戏。最低价格为 0。
5. 「查看任务队列」进入任务页，可编辑完整条件、批量启停或删除。

同一条目刷新时保留尚未提交的输入；切换到另一个条目后按新条目的已有任务加载条件。无搜索结果时，创建按钮和条件输入禁用。存在多条关联任务时，右侧初始值和列表目标价来自首条关联任务；创建操作新增任务，不覆盖已有任务。

## 数据与图像

- 当前依然使用本地演示数据；价格、样本趋势、任务执行和统计均不代表真实行情或成交。
- `src/assets/demo_skin_atlas.png` 是通过内置图像生成工具制作的透明演示图集，不是官方游戏素材。
- 图集为四列三行；程序在运行时按格子取图并裁去透明留白，不改变图集原文件。未匹配的条目显示无插图状态。
- 横轴 S1–S24 为样本顺序，不是伪造的采集时间。
- 没有照搬概念图中的 PUBG、多游戏分类、标签或备注占位入口。

## 构建与验收

在项目根目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test -Package
```

离屏验收包含原有流程、七页标准/紧凑布局、文字编码、控件裁切、透明图集、目录筛选、关联状态筛选、右侧条件创建、输入刷新保留和无结果状态。数据模型及其 48 项测试未修改。

```powershell
python artifacts/terminal_ui/verify_ui_refresh.py baseline
python artifacts/terminal_ui/verify_ui_refresh.py modified
python artifacts/terminal_ui/verify_ui_refresh.py rollback
```

第三条命令仅在 `artifacts/terminal_ui/rollback_test` 副本上恢复旧版并重新测试，保留工作目录中的新版。逐条命令、字面输出、退出码、哈希和修改清单在 `artifacts/terminal_ui/VERIFICATION.txt`。

## 手动恢复上一版

关闭正在运行的 Relink 后，在项目根目录执行：

```powershell
& 'C:\Program Files\Git\bin\bash.exe' artifacts/terminal_ui/ROLLBACK.sh .
```

这会恢复本轮保留的 0.2 源码、说明文件和发布程序，并移除本轮新增的图集及本说明。用户应用配置不在回滚目标中。构建目录不回退；需要重新编译时运行 `build.ps1`。
