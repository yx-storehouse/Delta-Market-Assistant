"""Build documentation-only schemas and synthetic contract fixtures; no app imports."""
from pathlib import Path
from copy import deepcopy
import json
import hashlib

HERE = Path(__file__).resolve().parent
BASE = "https://schemas.relink.invalid/business/v1/domain/"
DIALECT = "https://json-schema.org/draft/2020-12/schema"

def obj(props, required=None, **more):
    return {"type": "object", "properties": props, "required": list(props) if required is None else required,
            "additionalProperties": False, **more}

def ref(name): return {"$ref": f"#/$defs/{name}"}
def ext(name): return {"$ref": f"core.schema.json#/$defs/{name}"}
def nullable(x): return {"anyOf": [x, {"type": "null"}]}
def arr(x, maximum=10000): return {"type": "array", "items": x, "maxItems": maximum}
def string(maximum=256, minimum=1): return {"type": "string", "minLength": minimum, "maxLength": maximum}
def integer(lo=0, hi=9007199254740991): return {"type": "integer", "minimum": lo, "maximum": hi}
def enum(*values): return {"enum": list(values)}
def dump(name, data):
    p = HERE / name; p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

defs = {}
defs["Id"] = {"type": "string", "pattern": "^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$"}
defs["MonoMs"] = integer()
defs["Extensions"] = {"type": "object", "maxProperties": 32, "propertyNames": {"pattern": "^x-[A-Za-z0-9._-]{1,60}$"}}
defs["DecimalValue"] = obj({"unscaled": {"type": "string", "pattern": "^(0|[1-9][0-9]{0,23})$"}, "scale": integer(0,12)})
defs["Money"] = obj({"value": ref("DecimalValue"), "unit": nullable(ref("Id"))})
defs["RuleRef"] = obj({"task_id": ref("Id"), "revision": integer(1,2147483647)})
defs["Selector"] = {"oneOf": [obj({"op":{"const":"any"}}), obj({"op":{"const":"eq"}, "value": string(128)})]}
defs["ConditionSelector"] = {"oneOf": [obj({"op":{"const":"any"}}), obj({"op":{"const":"eq"}, "value":enum("S","A","B","C")})]}
defs["OwnershipSelector"] = {"oneOf": [obj({"op":{"const":"any"}}), obj({"op":{"const":"eq"}, "value":enum("owned","unowned")})]}
defs["PublicitySelector"] = {"oneOf": [obj({"op":{"const":"any"}}), obj({"op":{"const":"eq"}, "value":enum("active","ended","none")})]}
defs["FieldEvidence"] = obj({"status":enum("observed","missing","invalid","ambiguous"), "raw": nullable(string(4096,0)),
    "confidence":nullable({"type":"number","minimum":0,"maximum":1}), "source_ref":ref("Id")})
defs["Association"] = obj({"association_ref":nullable(ref("Id")), "status":enum("confirmed","provisional","ambiguous","unknown")})
defs["SkinCatalogEntry"] = obj({"product_id":ref("Id"), "name":string(256), "series":nullable(string()), "season":nullable(string(128)),
    "grade":nullable(string(128)), "rarity":nullable(string(128)), "aliases":arr(string(256),32),
    "source_kind":enum("native","legacy_v1","legacy_13","synthetic"), "extensions":ref("Extensions")})
field_names = ["product_ref","price","wear","condition","season","ownership","grade","rarity","publicity"]
defs["ListingObservation"] = obj({"observation_id":ref("Id"), "product_ref":nullable(ref("Id")), "market_listing_id":nullable(string(256)),
    "association":ref("Association"), "price":nullable(ref("Money")), "wear":nullable(ref("DecimalValue")),
    "condition":nullable(enum("S","A","B","C")), "season":nullable(string(128)), "ownership":nullable(enum("owned","unowned")),
    "grade":nullable(string(128)), "rarity":nullable(string(128)), "publicity":nullable(enum("active","ended","none")),
    "frame_ref":ref("Id"), "session_id":ref("Id"), "clock_domain_id":ref("Id"), "observed_mono_ms":ref("MonoMs"),
    "viewport_generation":integer(), "field_evidence":obj({n:ref("FieldEvidence") for n in field_names}), "extensions":ref("Extensions")})
defs["LegacyRaw"] = obj({"source_kind":enum("v1","bbzps_13","native"), "source_sha256":{"type":"string","pattern":"^[a-f0-9]{64}$"},
    "source_line":nullable(integer(1,1000000)), "raw_line":nullable(string(65536,0)), "columns":arr(string(65536,0),13),
    "unknown_json": {"type":"object"}}, required=["source_kind","source_sha256","source_line","raw_line","columns","unknown_json"])
defs["TaskRule"] = obj({"task_id":ref("Id"), "revision":integer(1,2147483647), "name":string(60), "product_ref":nullable(ref("Id")),
    "enabled":{"type":"boolean"}, "review_required":{"type":"boolean"}, "review_codes":arr(ref("Id"),64),
    "filters":obj({"season":ref("Selector"),"ownership":ref("OwnershipSelector"),"grade":ref("Selector"),"condition":ref("ConditionSelector"),
                   "publicity":ref("PublicitySelector"),"rarity":ref("Selector")}),
    "price_range":nullable(obj({"min":ref("Money"),"max":ref("Money")})), "max_wear":nullable(ref("DecimalValue")),
    "requested_sort":enum("default","price_asc","price_desc","rarity_asc","rarity_desc","unknown"),
    "quantity_candidate":nullable(integer(1,9999)), "quantity_semantics":enum("unreviewed","demo_count_only","none"),
    "legacy_raw":nullable(ref("LegacyRaw")), "extensions":ref("Extensions")},
    allOf=[{"if":{"properties":{"review_required":{"const":True}}}, "then":{"properties":{"enabled":{"const":False},"review_codes":{"minItems":1}}}},
           {"if":{"properties":{"review_required":{"const":False}}}, "then":{"properties":{"product_ref":ref("Id"),"price_range":obj({"min":ref("Money"),"max":ref("Money")}),"review_codes":{"maxItems":0}}}}])
defs["EvaluationContext"] = obj({"session_id":ref("Id"),"clock_domain_id":ref("Id"),"viewport_generation":integer(),
    "as_of_mono_ms":ref("MonoMs"),"max_age_ms":integer(0,60000),"purpose":enum("filter_only","intent_candidate"),
    "known_units":arr(ref("Id"),32)})
reason_codes = ["MATCH","RULE_INVALID","RULE_REVIEW_REQUIRED","RULE_DISABLED","CONTEXT_MISMATCH","OBSERVATION_FUTURE","OBSERVATION_STALE",
    "ASSOCIATION_UNKNOWN","ASSOCIATION_AMBIGUOUS","ASSOCIATION_PROVISIONAL","PRODUCT_UNRESOLVED","FIELD_MISSING","FIELD_INVALID","FIELD_AMBIGUOUS",
    "UNIT_UNKNOWN","UNIT_MISMATCH","PRODUCT_MISMATCH","SEASON_MISMATCH","OWNERSHIP_MISMATCH","GRADE_MISMATCH","CONDITION_MISMATCH",
    "PUBLICITY_MISMATCH","RARITY_MISMATCH","PRICE_BELOW_MIN","PRICE_ABOVE_MAX","WEAR_ABOVE_MAX"]
defs["ReasonCode"] = enum(*reason_codes)
defs["DecisionReason"] = obj({"code":ref("ReasonCode"),"field":string(128,0),"evidence_ref":nullable(ref("Id"))})
defs["Decision"] = obj({"status":enum("Match","NoMatch","NeedsReview"),"primary_reason":ref("ReasonCode"),
    "reasons":arr(ref("DecisionReason"),64), "rule_ref":ref("RuleRef"),"observation_id":ref("Id"),
    "eligible_for_action":{"const":False}})
core = {"$schema":DIALECT,"$id":BASE+"core.schema.json","title":"Relink M1 domain contract; specification, not implementation", "$defs":defs}
dump("core.schema.json",core)

run_props={"profile":string(40),"hotkey":{"type":"string","pattern":"^F([1-9]|1[0-2])$"},"purchaseDelayMs":integer(0,60000),
    "dynamicDelay":{"type":"boolean"},"queueFullTrigger":integer(1,9999),"queueFullStepMs":{"type":"number","minimum":0,"maximum":1000},
    "publicityTrigger":integer(1,9999),"publicityStepMs":{"type":"number","minimum":0,"maximum":1000},
    "burstClick":{"type":"boolean"},"clickIntervalMs":integer(1,10000),"limitOrange":integer(0,9999),"limitPurple":integer(0,9999),"limitBlue":integer(0,9999)}
for f in ["refreshPage","skipLotteryPage","skipSuccessPage","autoCollect","collectOsd","scheduleEnabled"]:run_props[f]={"type":"boolean"}
for f in ["scheduleStart","scheduleStop"]: run_props[f]={"type":"string","pattern":"^([01][0-9]|2[0-3]):[0-5][0-9]$"}
cfgdefs={"UiRunSettings":obj(run_props),
    "UnitDefinition":obj({"unit_id":ext("Id"),"display_name":string(128),"quantum":ext("DecimalValue"),"reviewed":{"const":True},"synthetic":{"type":"boolean"}}),
    "Profile":obj({"profile_id":ext("Id"),"revision":integer(1,2147483647),"name":string(40),"mode":enum("demo","replay","observe"),
        "rule_refs":arr(ext("RuleRef")),"ui_run_settings":ref("UiRunSettings"),"execution_policy":{"const":"not_bound"},
        "schedule_timezone":nullable(string(128)),"extensions":ext("Extensions")}),
    "MigrationDecision":obj({"decision_id":ext("Id"),"source_sha256":{"type":"string","pattern":"^[a-f0-9]{64}$"},
        "source_line":nullable(integer(1,1000000)),"field":string(256),"choice":string(1024),"actor":enum("user","compatibility_adapter"),
        "note":string(4096,0)})}
config={"$schema":DIALECT,"$id":BASE+"config-v2.schema.json","title":"Relink schema v2 proposed M1 configuration",
    **obj({"schema_version":{"const":2},"contract_version":{"const":"0.3.0"},"config_id":ext("Id"),"revision":integer(1,2147483647),
        "catalog":arr(ext("SkinCatalogEntry")),"rules":arr(ext("TaskRule")),"profiles":arr(ref("Profile"),100),
        "active_profile_id":ext("Id"),"units":arr(ref("UnitDefinition"),32),"migration_decisions":arr(ref("MigrationDecision")),
        "extensions":ext("Extensions")}),"$defs":cfgdefs}
dump("config-v2.schema.json",config)

importdefs={"Diagnostic":obj({"severity":enum("error","review","info"),"code":ext("Id"),"line":nullable(integer(1,1000000)),
    "column":nullable(integer(1,1000000)),"field":nullable(string(256)),"message":string(4096)}),
    "PreviewRow":obj({"line":integer(1,1000000),"raw_line":string(65536,0),"columns":arr(string(65536,0),13),
        "status":enum("invalid","review","ready"),"candidate":nullable(ext("TaskRule")),"diagnostics":arr(ref("Diagnostic"),128)})}
preview={"$schema":DIALECT,"$id":BASE+"import-preview.schema.json","title":"Read-only import preview; never an executable configuration",
    **obj({"kind":{"const":"LegacyImportPreview"},"source_format":enum("schema_v1","bbzps_13"),
        "source_sha256":{"type":"string","pattern":"^[a-f0-9]{64}$"},"source_bytes":integer(0,8388608),
        "encoding":enum("UTF-8","UTF-8-BOM","UTF-16LE-BOM","GB18030-explicit"),"rows":arr(ref("PreviewRow")),
        "diagnostics":arr(ref("Diagnostic"),1000),"committable":{"const":False},"extensions":ext("Extensions")}),"$defs":importdefs}
dump("import-preview.schema.json",preview)

def decimal(text):
    before, sep, after = text.partition(".")
    return {"unscaled":str(int(before+after)),"scale":len(after) if sep else 0}
def money(text,unit="SYNTHETIC_CREDIT"): return {"value":decimal(text),"unit":unit}
def selector(value=None): return {"op":"any"} if value is None else {"op":"eq","value":value}
rule={"task_id":"task-01","revision":1,"name":"合成边界任务","product_ref":"product-01","enabled":True,"review_required":False,"review_codes":[],
    "filters":{"season":selector(),"ownership":selector(),"grade":selector(),"condition":selector("S"),"publicity":selector(),"rarity":selector()},
    "price_range":{"min":money("230"),"max":money("600")},"max_wear":decimal("0.399"),"requested_sort":"default",
    "quantity_candidate":None,"quantity_semantics":"none","legacy_raw":None,"extensions":{}}
obs={"observation_id":"obs-01","product_ref":"product-01","market_listing_id":None,
    "association":{"association_ref":"assoc-01","status":"confirmed"},"price":money("340"),"wear":decimal("0.3"),"condition":"S",
    "season":None,"ownership":None,"grade":None,"rarity":None,"publicity":None,
    "frame_ref":"frame-01","session_id":"session-01","clock_domain_id":"clock-01","observed_mono_ms":1000,"viewport_generation":1,
    "field_evidence":{},"extensions":{}}
for field in field_names:
    obs["field_evidence"][field]={"status":"observed" if obs[field] is not None else "missing","raw":str(obs[field]) if obs[field] is not None else None,
        "confidence":None,"source_ref":"frame-01"}
context={"session_id":"session-01","clock_domain_id":"clock-01","viewport_generation":1,"as_of_mono_ms":1100,"max_age_ms":1000,
    "purpose":"filter_only","known_units":["SYNTHETIC_CREDIT"]}
cases=[]
def case(cid,label,mutate=None,status="Match",code="MATCH",field="",stage="decision",refs=None):
    r,o,c=deepcopy(rule),deepcopy(obs),deepcopy(context)
    if mutate: mutate(r,o,c)
    for name in field_names:
        value = o[name]
        if value is None: o["field_evidence"][name]["raw"] = None
        elif isinstance(value, dict): o["field_evidence"][name]["raw"] = json.dumps(value, ensure_ascii=False, separators=(",",":"))
        else: o["field_evidence"][name]["raw"] = str(value)
    reason={"code":code,"field":field,"evidence_ref":o["frame_ref"] if field else None}
    expected={"stage":stage,"decision":{"status":status,"primary_reason":code,"reasons":[reason],"rule_ref":{"task_id":r["task_id"],"revision":r["revision"]},
        "observation_id":o["observation_id"],"eligible_for_action":False}} if stage=="decision" else {"stage":stage,"diagnostic_code":code}
    cases.append({"id":cid,"description":label,"specification_only":True,"trace":refs or ["B16","T16-A","WI01"],
        "input":{"rule":r,"observation":o,"context":c},"expected":expected})
case("G01","普通匹配")
case("G02","价格恰等于下界仍匹配",lambda r,o,c:o.update(price=money("230")),refs=["B14","T14-A","WI01"])
case("G03","价格恰等于上界仍匹配",lambda r,o,c:o.update(price=money("600.00")),refs=["B14","T14-A","WI01"])
case("G04","价格比下界低0.000000000001",lambda r,o,c:o.update(price=money("229.999999999999")),"NoMatch","PRICE_BELOW_MIN","price")
case("G05","价格比上界高0.000000000001",lambda r,o,c:o.update(price=money("600.000000000001")),"NoMatch","PRICE_ABOVE_MAX","price")
case("G06","磨损上界等值不同scale",lambda r,o,c:o.update(wear=decimal("0.399000")),refs=["B15","T15-A","WI01"])
case("G07","磨损刚高于阈值",lambda r,o,c:o.update(wear=decimal("0.399000000001")),"NoMatch","WEAR_ABOVE_MAX","wear")
case("G08","币值单位缺失不是人民币",lambda r,o,c:o["price"].update(unit=None),"NeedsReview","UNIT_UNKNOWN","price")
case("G09","单位不同不做隐式换算",lambda r,o,c:(o["price"].update(unit="OTHER_CREDIT"),c["known_units"].append("OTHER_CREDIT")),"NeedsReview","UNIT_MISMATCH","price")
case("G10","缺磨损且规则限制磨损",lambda r,o,c:(o.update(wear=None),o["field_evidence"]["wear"].update(status="missing",raw=None)),"NeedsReview","FIELD_MISSING","wear")
case("G11","无磨损限制时缺磨损可以匹配",lambda r,o,c:(r.update(max_wear=None),o.update(wear=None),o["field_evidence"]["wear"].update(status="missing",raw=None)))
case("G12","成色标签不一致",lambda r,o,c:o.update(condition="A"),"NoMatch","CONDITION_MISMATCH","condition")
case("G13","导入未审禁用规则优先提示审查",lambda r,o,c:r.update(enabled=False,review_required=True,review_codes=["PRICE_MAPPING_UNREVIEWED"]),"NeedsReview","RULE_REVIEW_REQUIRED")
case("G14","普通禁用规则",lambda r,o,c:r.update(enabled=False),"NoMatch","RULE_DISABLED")
case("G15","同款相似卖单关联有歧义",lambda r,o,c:o["association"].update(status="ambiguous"),"NeedsReview","ASSOCIATION_AMBIGUOUS","association")
case("G16","过期1毫秒",lambda r,o,c:c.update(as_of_mono_ms=2001),"NeedsReview","OBSERVATION_STALE","observed_mono_ms")
case("G17","年龄恰等于max_age允许",lambda r,o,c:c.update(as_of_mono_ms=2000))
case("G18","旧视口代次",lambda r,o,c:c.update(viewport_generation=2),"NeedsReview","CONTEXT_MISMATCH","viewport_generation")
case("G19","未来帧不负年龄放行",lambda r,o,c:o.update(observed_mono_ms=1101),"NeedsReview","OBSERVATION_FUTURE","observed_mono_ms")
case("G20","未知赛季且规则为不限不阻塞")
case("G21","稀有度和品阶独立",lambda r,o,c:(r["filters"].update(grade=selector("epic"),rarity=selector("rare")),o.update(grade="epic",rarity="common"),o["field_evidence"]["grade"].update(status="observed"),o["field_evidence"]["rarity"].update(status="observed")),"NoMatch","RARITY_MISMATCH","rarity")
case("G22","卖单价格存在但识别被标为歧义",lambda r,o,c:o["field_evidence"]["price"].update(status="ambiguous"),"NeedsReview","FIELD_AMBIGUOUS","price")
case("G23","筛选阶段允许临时关联",lambda r,o,c:o["association"].update(status="provisional"))
case("G24","意图候选要求关联已确认",lambda r,o,c:(o["association"].update(status="provisional"),c.update(purpose="intent_candidate")),"NeedsReview","ASSOCIATION_PROVISIONAL","association")
case("G25","最高精度超界拒绝不截断",lambda r,o,c:o.update(wear={"unscaled":"1","scale":13}),code="SCHEMA_INVALID",stage="structural_validation")
case("G26","不接受double型金额",lambda r,o,c:o.update(price=340.0),code="SCHEMA_INVALID",stage="structural_validation")
case("G27","上下限倒置语义拒绝",lambda r,o,c:r["price_range"].update(min=money("601")),code="PRICE_RANGE_REVERSED",stage="semantic_validation")
case("G28","单位未知高于已知价格不匹配优先",lambda r,o,c:o.update(price=money("999",None)),"NeedsReview","UNIT_UNKNOWN","price")
dump("fixtures/rule-golden.json",{"kind":"rule_golden_specification","contract_version":"0.3.0","cases":cases})

raw="AUG突击步枪-天命|全部赛季|未拥有|史诗品阶|成色S|不限公示期|230|600|0.399|所有稀有度|默认排序|不限|不限"
importcases=[
 {"id":"I01","input":{"format":"bbzps_13","text":raw,"encoding":"UTF-8"},"expected":{"row_status":"review","enabled":False,"review_required":True,"raw_columns":raw.split("|"),"quantity_candidate":None,"diagnostic_codes":["PRICE_MAPPING_UNREVIEWED","UNIT_UNREVIEWED","PRODUCT_UNRESOLVED","TAXONOMY_UNREVIEWED","UNKNOWN_TRAILING_COLUMNS"],"committable":False}},
 {"id":"I02","input":{"format":"bbzps_13","text":"|".join(raw.split("|")[:-1]),"encoding":"UTF-8"},"expected":{"row_status":"invalid","candidate":None,"diagnostic_codes":["COLUMN_COUNT"],"error_line":1,"committable":False}},
 {"id":"I03","input":{"format":"bbzps_13","text":raw.replace("|230|600|","|601|600|"),"encoding":"UTF-8"},"expected":{"row_status":"invalid","diagnostic_codes":["PRICE_RANGE_REVERSED"],"error_line":1,"error_column":7,"committable":False}},
 {"id":"I04","input":{"format":"bbzps_13","text":raw.replace("|230|600|","|2O0|600|"),"encoding":"UTF-8"},"expected":{"row_status":"invalid","diagnostic_codes":["DECIMAL_INVALID"],"error_line":1,"error_column":7,"committable":False}},
 {"id":"I05","input":{"format":"bbzps_13","text":raw+"\n"+raw,"encoding":"UTF-8"},"expected":{"row_count":2,"deduplicated":False,"diagnostic_codes":["POSSIBLE_DUPLICATE_ROW"],"committable":False}},
 {"id":"I06","input":{"format":"bbzps_13","text":raw.replace("|不限|不限","|7|9"),"encoding":"UTF-8"},"expected":{"last_columns":["7","9"],"quantity_candidate":None,"diagnostic_codes":["UNKNOWN_TRAILING_COLUMNS"],"committable":False}},
 {"id":"I07","input":{"format":"schema_v1","task_patch":{"condition":"<absent>","quantity":3},"demo":True},"expected":{"condition_filter":{"op":"any"},"quantity_candidate":3,"quantity_semantics":"demo_count_only","target_mode":"demo","review_required":True,"committable":False}},
 {"id":"I08","input":{"format":"schema_v1","task_patch":{"minPrice":0.30000000000000004},"demo":True},"expected":{"decimal_candidate":{"unscaled":"30","scale":2},"diagnostic_codes":["V1_FLOAT_NOISE_NORMALIZED"],"normalization_audit_required":True}},
 {"id":"I09","input":{"format":"schema_v1","task_patch":{"minPrice":0.301},"demo":True},"expected":{"row_status":"invalid","diagnostic_codes":["V1_PRECISION_INVALID"],"committable":False}},
 {"id":"I10","input":{"format":"bbzps_13","text":raw.replace("|230|600|","|1,000|1,500|"),"encoding":"UTF-8"},"expected":{"candidate_min":{"unscaled":"1000","scale":0},"candidate_max":{"unscaled":"1500","scale":0},"row_status":"review"}},
 {"id":"I11","input":{"format":"bbzps_13","text":raw.replace("|230|600|","|1,00|600|"),"encoding":"UTF-8"},"expected":{"row_status":"invalid","diagnostic_codes":["DECIMAL_GROUPING_INVALID"],"committable":False}},
 {"id":"I12","input":{"format":"commit","candidate_ready":True,"output_exists":True,"destination_equals_source":False},"expected":{"result":"rejected","diagnostic_codes":["OUTPUT_EXISTS"],"source_unchanged":True,"active_config_unchanged":True}},
 {"id":"I13","input":{"format":"commit","candidate_ready":True,"write_fault":"commit_failed"},"expected":{"result":"failed","diagnostic_codes":["COMMIT_FAILED"],"source_unchanged":True,"active_config_unchanged":True}},
 {"id":"I14","input":{"format":"commit","candidate_ready":True,"source_hash_changed_after_preview":True},"expected":{"result":"rejected","diagnostic_codes":["SOURCE_CHANGED"],"active_config_unchanged":True}},
]
baseline_path = HERE.parents[3] / "artifacts/BASELINE.json"
baseline_bytes = baseline_path.read_bytes()
baseline = json.loads(baseline_bytes.decode("utf-8-sig"))
assert baseline["schema_version"] == 1 and baseline["demo"] is True
baseline_sha = hashlib.sha256(baseline_bytes).hexdigest()
(HERE / "fixtures/v1.synthetic.json").write_bytes(baseline_bytes)
dump("fixtures/v1.synthetic.origin.json",{"source":"artifacts/BASELINE.json","source_kind":"existing_project_demo_fixture",
    "sha256":baseline_sha,"bytes":len(baseline_bytes),"copied_byte_for_byte":True,
    "interpretation":"Synthetic presentation data; not an observed market snapshot or a successful order."})
operations={"I07":[{"op":"remove_if_present","path":"/tasks/0/condition"},{"op":"replace","path":"/tasks/0/quantity","value":3}],
    "I08":[{"op":"replace","path":"/tasks/0/minPrice","value":0.30000000000000004}],
    "I09":[{"op":"replace","path":"/tasks/0/minPrice","value":0.301}]}
for item in importcases:
    item.update(specification_only=True,trace=["B05","B39","T05-B","T39-B","WI02"])
    if item["id"] in operations:
        item["input"]={"format":"schema_v1","base_ref":"v1.synthetic.json","base_sha256":baseline_sha,"operations":operations[item["id"]]}
dump("fixtures/import-golden.json",{"kind":"import_golden_specification","contract_version":"0.3.0",
    "assembly":"Resolve base_ref relative to this fixture file, verify SHA-256, deep copy, apply operations in listed order, UTF-8 serialize, then import. No source file is modified.","cases":importcases})
ui={"profile":"合成回放方案","hotkey":"F2","purchaseDelayMs":830,"dynamicDelay":False,"queueFullTrigger":1,"queueFullStepMs":1.0,
    "publicityTrigger":1,"publicityStepMs":1.0,"burstClick":True,"clickIntervalMs":10,"limitOrange":10,"limitPurple":10,"limitBlue":10,
    "refreshPage":False,"skipLotteryPage":True,"skipSuccessPage":True,"autoCollect":True,"collectOsd":False,
    "scheduleEnabled":False,"scheduleStart":"09:00","scheduleStop":"23:00"}
sample={"schema_version":2,"contract_version":"0.3.0","config_id":"config-synthetic","revision":1,
    "catalog":[{"product_id":"product-01","name":"合成商品","series":None,"season":None,"grade":None,"rarity":None,"aliases":[],"source_kind":"synthetic","extensions":{}}],
    "rules":[rule],"profiles":[{"profile_id":"profile-01","revision":1,"name":"合成回放方案","mode":"replay","rule_refs":[{"task_id":"task-01","revision":1}],
        "ui_run_settings":ui,"execution_policy":"not_bound","schedule_timezone":None,"extensions":{}}],
    "active_profile_id":"profile-01","units":[{"unit_id":"SYNTHETIC_CREDIT","display_name":"合成测试单位","quantum":decimal("0.000000000001"),"reviewed":True,"synthetic":True}],
    "migration_decisions":[],"extensions":{}}
dump("fixtures/config-v2.synthetic.json",sample)
manifest=[]
def schema_case(cid, name, definition, instance, valid):
    path=f"fixtures/schema/{cid}.json"
    dump(path,instance)
    schema_id=BASE+name+("#/$defs/"+definition if definition else "")
    manifest.append({"id":cid,"schema_id":schema_id,"instance":path,"valid":valid})
schema_case("S01","config-v2.schema.json","",sample,True)
schema_case("S02","core.schema.json","TaskRule",rule,True)
schema_case("S03","core.schema.json","ListingObservation",obs,True)
schema_case("S04","core.schema.json","DecimalValue",decimal("0.399000000001"),True)
schema_case("S05","core.schema.json","DecimalValue",{"unscaled":"1","scale":13},False)
schema_case("S06","core.schema.json","DecimalValue",{"unscaled":1,"scale":0},False)
schema_case("S07","core.schema.json","DecimalValue",{"unscaled":"-1","scale":0},False)
schema_case("S08","core.schema.json","DecimalValue",{"unscaled":"1"*25,"scale":0},False)
bad=deepcopy(rule);bad["unknown_field"]=1
schema_case("S09","core.schema.json","TaskRule",bad,False)
bad=deepcopy(rule);bad["review_required"]=True;bad["review_codes"]=["UNIT_UNREVIEWED"]
schema_case("S10","core.schema.json","TaskRule",bad,False)
bad=deepcopy(rule);bad["price_range"]=None
schema_case("S11","core.schema.json","TaskRule",bad,False)
good=deepcopy(rule);good["enabled"]=False;good["review_required"]=True;good["review_codes"]=["UNIT_UNREVIEWED"];good["price_range"]=None
schema_case("S12","core.schema.json","TaskRule",good,True)
bad=deepcopy(obs);bad["condition"]="SS"
schema_case("S13","core.schema.json","ListingObservation",bad,False)
schema_case("S14","core.schema.json","Id","item with spaces",False)
bad=deepcopy(sample);bad["schema_version"]=3
schema_case("S15","config-v2.schema.json","",bad,False)
schema_case("S16","core.schema.json","MonoMs",9007199254740992,False)
preview_sample={"kind":"LegacyImportPreview","source_format":"bbzps_13","source_sha256":"0"*64,"source_bytes":len(raw.encode()),"encoding":"UTF-8",
    "rows":[{"line":1,"raw_line":raw,"columns":raw.split("|"),"status":"review","candidate":good,"diagnostics":[{"severity":"review","code":"UNIT_UNREVIEWED","line":1,"column":None,"field":"price_range","message":"确认显示单位后再生成配置"}]}],
    "diagnostics":[],"committable":False,"extensions":{}}
schema_case("S17","import-preview.schema.json","",preview_sample,True)
bad=deepcopy(preview_sample);bad["committable"]=True
schema_case("S18","import-preview.schema.json","",bad,False)
schema_case("S19","core.schema.json","Decision",cases[0]["expected"]["decision"],True)
bad=deepcopy(cases[0]["expected"]["decision"]);bad["eligible_for_action"]=True
schema_case("S20","core.schema.json","Decision",bad,False)
dump("validation_manifest.json",{"cases":manifest})
print("GENERATED core, config-v2, import-preview schemas; 28 rule + 14 import specifications; synthetic config")
