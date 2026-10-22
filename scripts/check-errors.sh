#!/usr/bin/env bash
# Error policy of src/ (needs no PostgreSQL and no build), see CONTRIBUTING.md "Errors say what to do":
#   - an ERROR with SQLSTATE 54000 (limit), 0A000 (unsupported) or 55000 (wrong state) names what to do: errhint or errdetail;
#   - no ERROR has SQLSTATE XX000: a condition the code rules out is SIFT_INVARIANT(cond, "text") (core/host.h), anything else
#     a reachable condition with its own errcode (54000, 0A000, 55000, 40001, XX001/XX002 for corruption, ...).
#   An `errcode(ERRCODE_INTERNAL_ERROR)` is XX000 too. The test-only files (*_test.c, test.c, compiled only with SIFT_TESTING) are
#   not checked; any other site may say `check-errors: allow` in a comment on the line of the call or one of the two above it.
#
#   scripts/check-errors.sh
set -euo pipefail

SRC=$(cd "$(dirname "$0")/../src" && pwd)
exec python3 -I - "$SRC" <<'PY'
import os, re, sys

src = sys.argv[1]
NEEDS_HELP = {"ERRCODE_PROGRAM_LIMIT_EXCEEDED", "ERRCODE_FEATURE_NOT_SUPPORTED", "ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE"}
call = re.compile(r"\b(ereport|elog)\s*\(")


def blank(text):
    """Comments and the inside of string and character literals become spaces (newlines stay); the comments are returned by line."""
    out, notes, i, n, line = [], {}, 0, len(text), 1
    while i < n:
        c = text[i]
        if text.startswith("/*", i) or text.startswith("//", i):
            j = text.find("*/", i + 2) + 2 if text[i + 1] == "*" else text.find("\n", i)
            j = n if j < 2 else j
            body = text[i:j]
            for k, part in enumerate(body.split("\n")):
                notes[line + k] = notes.get(line + k, "") + part
            out.append(re.sub(r"[^\n]", " ", body))
            line += body.count("\n")
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(c + " " * (j - i - 1) + c)
            line += text[i:j + 1].count("\n")
            i = j + 1
        else:
            out.append(c)
            line += c == "\n"
            i += 1
    return "".join(out), notes


def balanced(text, start):
    depth = 0
    for j in range(start, len(text)):
        depth += text[j] == "("
        depth -= text[j] == ")"
        if depth == 0:
            return j + 1
    return len(text)


bad = []
files = 0
for root, _, names in os.walk(src):
    for name in sorted(names):
        if not name.endswith((".c", ".h")):
            continue
        path = os.path.join(root, name)
        with open(path, encoding="utf-8") as f:
            raw = f.read()
        if re.search(r"(^|_)test(_am)?\.c$", name) and "SIFT_TESTING" in raw:
            continue
        text, notes = blank(raw)
        files += 1
        for m in call.finditer(text):
            end = balanced(text, m.end() - 1)
            body = text[m.end():end - 1]
            level = re.match(r"\s*(\w+)", body)
            if not level or level.group(1) not in ("ERROR", "FATAL"):
                continue
            n = text.count("\n", 0, m.start()) + 1
            if any("check-errors: allow" in notes.get(k, "") for k in (n - 2, n - 1, n)):
                continue
            where = f"{os.path.relpath(path, os.path.dirname(src))}:{n}"
            # the raw text keeps the names of the errcode and report calls (blank() kept every identifier)
            codes = re.findall(r"\berrcode\s*\(\s*(\w+)", body)
            if m.group(1) == "elog" or not codes or codes[0] == "ERRCODE_INTERNAL_ERROR":
                bad.append(f"{where}: XX000 ({m.group(1)}): use SIFT_INVARIANT(cond, \"text\") for a condition the code rules out, or an errcode")
            elif codes[0] in NEEDS_HELP and not re.search(r"\berr(hint|detail)\w*\s*\(", body):
                bad.append(f"{where}: {codes[0]} without errhint or errdetail: say what to do")

for b in bad:
    print(b)
if bad:
    print(f"check-errors: {len(bad)} site(s)", file=sys.stderr)
    sys.exit(1)
print(f"check-errors: ok, {files} files")
PY
