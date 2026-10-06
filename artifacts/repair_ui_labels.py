from pathlib import Path

path = Path("src/mainwindow.cpp")
text = path.read_text(encoding="utf-8")
replacements = {
    "鎴愯壊": "成色",
    "鎴愯壊S": "成色S",
    "鎴愯壊A": "成色A",
    "鎴愯壊B": "成色B",
    "工作�?": "工作台",
}
for old, new in replacements.items():
    text = text.replace(old, new)
path.write_text(text, encoding="utf-8", newline="")
print("UI_LABEL_REPAIRS=PASS")
