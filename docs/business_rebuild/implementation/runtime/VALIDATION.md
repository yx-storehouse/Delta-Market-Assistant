# Runtime文档实核记录 · 2026-10-06

本记录仅证明**契约文件可解析、结构正反例符合schema、链接可打开、fixture内部静态计数一致**。未运行M1业务Reducer、真实截图、OCR、共享内存通信或输入适配器；所有行为fixture仍为planned_not_run。

## 实际执行与结果

工作目录：`C:\Users\Administrator\Desktop\price`。

1. Python解释器使用 `python -I -X utf8 -`，从stdin执行离线校验；显式添加 `.tools/doc-render` 查找jsonschema，不安装或加载样本依赖。
2. 扫描 `docs/business_rebuild/implementation/**/*.schema.json`，对6个schema调用 `Draft202012Validator.check_schema`，按$id注册 `referencing.Registry`。retrieve函数始终报错，禁止网络解析引用。
3. 读取 `runtime/validation_manifest.json`，逐条读取instance；验证通过/拒绝与valid标志比较。22例包含合法消息、临时文件传图拒绝、超限/缺字段/版本错误等反例。
4. 检查4份Markdown中的14条本地链接；读取15个回放fixture，核对初始预留/成功数、事件时间非递减及planned_not_run标签。
5. 输出 `validation_results.json`，重新读取文件并复核统计。

最后一次工具原样输出：

```json
{"registered_schemas": 6, "runtime_cases": 22, "passed": 22, "failed": [], "local_links": 14, "broken_links": [], "replay_structure_checked": 15, "backend_executed": false}
```

退出状态：**0**。以上是2026-10-06本分支检查时的计数；主代理后续新增schema/链接时总数可能增加，不应拿此快照冒充后续结果。

## 复核核心命令

以下命令仅检查schema正反例，使用已存在的文档工具依赖；它不执行任何样本文件。完整链接/初始计数检查细节在 `validation_results.json` 中可审计。

```powershell
@'
import sys,pathlib,json
sys.path.insert(0,str(pathlib.Path(".tools/doc-render").resolve()))
from jsonschema import Draft202012Validator
from referencing import Registry,Resource
base=pathlib.Path("docs/business_rebuild/implementation")
def deny(uri):
    raise RuntimeError("Network schema retrieval forbidden: "+uri)
registry=Registry(retrieve=deny)
for path in base.rglob("*.schema.json"):
    schema=json.loads(path.read_text(encoding="utf-8"))
    Draft202012Validator.check_schema(schema)
    registry=registry.with_resource(schema["$id"],Resource.from_contents(schema))
root=base/"runtime"
cases=json.loads((root/"validation_manifest.json").read_text(encoding="utf-8"))["cases"]
for case in cases:
    value=json.loads((root/case["instance"]).read_text(encoding="utf-8"))
    actual=Draft202012Validator({"$ref":case["schema_id"]},registry=registry).is_valid(value)
    assert actual==case["valid"],case["id"]
print("runtime schema cases: "+str(len(cases))+" PASS")
'@ | python -I -X utf8 -
```

## 官方资料实核

`references.runtime.json`记录RRT01 Microsoft命名共享内存、RRT02 Python shared_memory、RRT03 JSON Schema Draft 2020-12：
- 三项均HTTP 200，标题/关键标记存在。
- 记录获取时间、最终URL、字节数、SHA-256。
- 资料用于支持新设计的可行性和术语，不用于证明BBZPS实际实现。
- 未访问样本服务端、未下载执行模型、未运行样本。

## 尚需真实开发后执行

15个状态序列、15个语义错误场景、6个lease时序场景的**行为结果未测试**。第一条完整事件序列RT-01可以直接成为M1首个单测；之后逐步将required_events子序列收紧为exact_events。M2另测进程I/O、实际捕获取消、共享内存读者退出、模型质量/性能，不用schema通过代替后端验收。

