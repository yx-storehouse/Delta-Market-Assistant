# 收藏任务导入与后续开发

2026-10-07。程序现在可以把原 `.savedValue` 收藏任务直接追加到真实皮肤目录，不再只显示解析预览。

## 在程序里使用

1. 打开“任务”，点击“导入收藏任务”，选择 `.savedValue`。右上角“导入”也识别这个文件类型。
2. 核对赛季、商品、菜单颜色/极品优品、成色、最低价、最高价、最大磨损、限量及启用状态。
3. 点击“确认追加”。程序先备份原配置、原子保存，再把新任务显示到任务页。保存失败保留预览供重试。
4. 导入不会自动启动收藏或购买，不覆盖既有关注及任务。相同文件改名后仍识别为重复；导入后手改的条件不会被重复导入覆盖。

限量 **0 = 不限**。任务编辑器、关注页右侧数量框、任务列表与 JSON 保存统一支持这个含义，不把 0 改成 1。

## 此次用户数据核对

原始来源 `S11新赛季0925.savedValue` 为 8,926 字节，SHA-256：

```text
cff452bcd4b77a17fb3cc9a23f55e0131304098f7ac0057205f619bbd4406253
```

共有 **50 行、21 行启用、13 个全部商品、9 个启用商品**。总商品数包含停用行；不要把 9 个启用商品误写成整份文件的商品总数。之前新测试猜测总商品 10 已由独立解码纠正，程序实际统计一直是 13。

保留原数值行顺序、启用状态、成色、价格、磨损、限量，以及原始七个字段。商品 ID 精确匹配已核对目录；禁用行也不能用未知 ID 混入。`疾风魅影` 与 `疾光魅影` 仍视为不同文字，不自动替换。来源中的购买/运行参数本轮仅作为解析元数据，不悄悄覆盖当前运行参数。

## 保存、重复与回退

- `Task.importSource` 保存原文件与字典 SHA-256、商品/成色 ID、行号和原始字段；可编辑条件与来源字段分离。
- 稳定任务 ID 为 `savedvalue:<源文件SHA-256>:<原始行号>`，同一来源重复导入不再追加。
- 原配置备份为 `config.json.before-task-import.bak`，重名时增编号。原 `.savedValue` 从不写入。
- 无新增的重复导入不保存无关草稿，不标记未保存编辑为已保存。
- 程序包回退不删除用户配置或来源文件；旧程序可能不认识数量 0 或来源字段。需要恢复旧配置时使用对应导入前备份，不将程序回退冒称为撤销游戏收藏。

无需显示窗口的同一提交入口（必须明确配置路径）：

```powershell
& .\dist\RelinkStudio\RelinkStudio.exe --import-collection-tasks 'C:\Users\Administrator\Desktop\S11新赛季0925.savedValue' --config 'C:\Users\Administrator\AppData\Local\RelinkStudio\RelinkStudio\config.json'
```

CLI 先在内存中解析与校验；输入错误时不迁移或覆盖目标配置。此命令不启动游戏动作。

## 模块及验证

- `src/application/collection_task_import.*`：严格身份关联、顺序、去重和纯内存预览。
- `src/application/collection_task_commit.*`：导入锁、原配置备份、原子写入和成功后的状态发布。
- `src/ui/dialogs/collection_import_dialog.*`：预览与确认，不执行点击或购买。
- `src/diagnostics/collection_import_self_test.*`：从正常目录启动，操作真实弹窗、测试写失败、追加、重开及重复导入；全部使用临时目录。
- `tests/catalog/collection_task_*_tests.cpp`、`tests/ui/collection_import_dialog_tests.cpp`：包含实际原文件可选输入。

```powershell
powershell -NoProfile -File .\build.ps1 -Test -Package
python -X utf8 -m unittest discover -s tests/manual -p 'test_collection*.py' -v
```

## 实机与滚动状态

本轮恢复原启动预检：游戏实测已回到大厅；按“大厅 → 曼德尔砖 → 典藏 → 我的关注”导航后，当前画面显示空关注。此前 25 次成功收藏保留为历史事实，不等于现在仍有 25 个在售关注。没有清空、取消或新加关注，也没有购买。每个完整批次仅前置游戏一次，结束统一恢复 IDE。

`tests/manual/collection_scroll.py` 新增同帧可见布局、半卡拒绝、坐标凭据失效、滚动条位移验证和 journal 对账契约。旧固定六卡包会明确报 `COLLECTION_LAYOUT_MISSING`，不能据此处理滚动后的坐标。新增测试覆盖的是离线字段契约，**还未接上 C++ 动态边界检测或宣称真实滚动收藏通过**。下一步需在新的当前页重新完成几何采集、选中卡片绑定及结果回读，而不是复用旧 P90 B 第六张的坐标。

交付、源数据副本验证、真实命令、失败尝试与隔离回退均记录于 `artifacts/collection_workflow/VERIFICATION.txt`。
