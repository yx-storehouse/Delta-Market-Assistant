from pathlib import Path

text = Path("src/domain.cpp").read_text(encoding="utf-8")
for number, line in enumerate(text.splitlines(), 1):
    if number in {32, 33, 34, 35, 255, 256, 257, 258, 259}:
        print(number, repr(line), line.count('"'))

main = Path("src/main.cpp").read_text(encoding="utf-8")
for number, line in enumerate(main.splitlines(), 1):
    if "TASK_CONDITION_COLUMN" in line or "horizontalHeaderItem(2)" in line:
        print("MAIN", number, repr(line))
        print("MAIN_CODEPOINTS", [hex(ord(ch)) for ch in line if ord(ch) > 127][:20])

window = Path("src/mainwindow.cpp").read_text(encoding="utf-8")
for number, line in enumerate(window.splitlines(), 1):
    if "m_tasksTable = table" in line or number == 1102:
        print("WINDOW", number, repr(line))
        print("WINDOW_CODEPOINTS", [hex(ord(ch)) for ch in line if ord(ch) > 127][:40])
        for token in line.split('QStringLiteral(')[1:]:
            print("TOKEN", repr(token[:30]), [hex(ord(ch)) for ch in token[:30] if ord(ch) > 127])
        print("ALL_TOKENS", line)
        Path("artifacts/window_tokens.json").write_text(
            __import__("json").dumps(
                [
                    {"text": token[:40], "codes": [ord(ch) for ch in token[:40]]}
                    for token in line.split("QStringLiteral(")[1:]
                ],
                ensure_ascii=True,
            ),
            encoding="utf-8",
        )

for root in (Path("src"), Path("tests")):
    for path in root.rglob("*"):
        if path.suffix not in {".cpp", ".h"}:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            escaped = False
            quotes = 0
            for char in line:
                if char == '"' and not escaped:
                    quotes += 1
                escaped = (char == '\\' and not escaped)
            if quotes % 2:
                print("ODD_QUOTES", path, number, repr(line))
