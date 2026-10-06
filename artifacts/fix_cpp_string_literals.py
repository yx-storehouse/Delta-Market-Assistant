from pathlib import Path


def repair(path: Path) -> int:
    text = path.read_text(encoding="utf-8")
    out = []
    fixes = 0
    delimiters = set(",);]}:")
    for line in text.splitlines(keepends=True):
        if path.name == "domain.cpp" and ("QStringLiteral" in line and ("?" in line or "�" in line)):
            print("DEBUG", repr(line), [hex(ord(c)) for c in line if c in "?�"][:8])
        if path.name == "domain.cpp" and line.count('"') % 2:
            print("ODD", repr(line))
        in_string = False
        escaped = False
        chars = list(line)
        result = []
        i = 0
        while i < len(chars):
            ch = chars[i]
            if ch == '"' and not escaped:
                in_string = not in_string
                result.append(ch)
                i += 1
                escaped = False
                continue
            if in_string and ch == '?':
                j = i + 1
                while j < len(chars) and chars[j].isspace() and chars[j] not in "\r\n":
                    j += 1
                if j < len(chars) and chars[j] in delimiters:
                    result.extend(['?', '"'])
                    in_string = False
                    escaped = False
                    fixes += 1
                    i += 1
                    continue
            result.append(ch)
            escaped = (ch == '\\' and not escaped)
            i += 1
        out.append(''.join(result))
    if fixes:
        path.write_text(''.join(out), encoding="utf-8", newline="")
    return fixes


if __name__ == "__main__":
    roots = [Path("src"), Path("tests")]
    total = 0
    for root in roots:
        for path in root.rglob("*"):
            if path.suffix in {".cpp", ".h"}:
                count = repair(path)
                if count:
                    print(f"{path}: {count}")
                    total += count
    print(f"TOTAL_FIXES={total}")
