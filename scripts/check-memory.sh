#!/usr/bin/env bash
# Memory-safety patterns of src/ that a grep can decide (needs no PostgreSQL and no build), see docs/SAFETY.md "Relation cache state":
#   - rd_amcache, the slot the relcache frees whenever it reloads the entry, is touched only by the files that own an accessor of it
#     (col/am.c sift_col_rel, bm25/wbuf.c get_hint, agg/stub_src.c, table/store.c sift_tbl_rel, which only tests it for NULL): every other file asks the accessor at the point of use;
#   - a local that keeps the result of the accessor, `SiftColRel *r = sift_col_rel(index)`, is a pointer into memory the relcache frees: it
#     is read after a lock or a catalog read only if the entry was fetched again. A site says why none comes between the fetch and its last
#     use (or takes sift_col_rel_private, which is not flagged); the earlier report, sift.col_verify and the cost estimate of a scan all kept one;
#   - a loop `for (int64 k = lo; k <= hi; k++)` over a 64-bit counter never ends when hi is the largest value of the type (the key loop of
#     the point directory ran past its array on `timestamp = 'infinity'`): count the iterations, or say why hi is bounded.
#   A 64-bit loop whose condition also bounds the counter with `<` is fine. Any other site says `check-memory: allow <why>` in a comment on
#   the line or the line above it. Test-only files are checked like the others.
#
#   scripts/check-memory.sh [--selftest]
set -euo pipefail

SRC=$(cd "$(dirname "$0")/../src" && pwd)
exec python3 -I - "$SRC" "${1:-}" <<'PY'
import os, re, sys, tempfile

AMCACHE_FILES = {"col/am.c", "bm25/wbuf.c", "agg/stub_src.c", "table/store.c"}
loop = re.compile(r"\bfor\s*\(\s*(?:const\s+)?(?:int64|uint64|int64_t|uint64_t)\s+(\w+)\s*=[^;]*;\s*\1\s*<=([^;]*);")
amcache = re.compile(r"\brd_amcache\b")
held = re.compile(r"\bSiftColRel\s*\*\s*\w+\s*=\s*sift_col_rel\(")


def scan(src):
    bad, files = [], 0
    for root, _, names in os.walk(src):
        for name in sorted(names):
            if not name.endswith((".c", ".h")):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, src)
            with open(path, encoding="utf-8") as f:
                lines = f.read().split("\n")
            files += 1
            for i, line in enumerate(lines):
                if "check-memory: allow" in line or (i and "check-memory: allow" in lines[i - 1]):
                    continue
                code = line.split("//")[0]
                if code.lstrip().startswith(("/*", "*")):
                    continue
                if amcache.search(code) and rel not in AMCACHE_FILES:
                    bad.append(f"{rel}:{i + 1}: rd_amcache outside its accessors ({', '.join(sorted(AMCACHE_FILES))}): the relcache frees it on every reload, ask the accessor where it is used")
                if held.search(code):
                    bad.append(f"{rel}:{i + 1}: a local kept from sift_col_rel(): the relcache frees that state on every reload (a lock, a catalog read); fetch it where it is used, take sift_col_rel_private, or allow with the reason nothing of the kind comes before its last use")
                m = loop.search(code)
                if m and not re.search(r"&&.*\b" + m.group(1) + r"\s*<[^=]|\b" + m.group(1) + r"\s*<[^=].*&&", m.group(2) + ";"):
                    bad.append(f"{rel}:{i + 1}: inclusive loop over a 64-bit counter wraps when the bound is the largest value: count the iterations, or allow with the reason the bound is small")
    return bad, files


def selftest():
    cases = {
        "col/am.c": "x = index->rd_amcache;\n",
        "col/other.c": "x = index->rd_amcache;\n",
        "a.c": "for (int64 k = lo; k <= hi; k++)\n",
        "b.c": "for (int64 k = lo; k <= hi && k < n; k++)\n",
        "c.c": "/* check-memory: allow hi is an int32 */\nfor (int64 k = lo; k <= hi; k++)\n",
        "d.c": "for (int k = lo; k <= hi; k++)\n",
        "f.c": "SiftColRel *r = sift_col_rel(index);\n",
        "g.c": "SiftColRel *r = sift_col_rel_private(index, cx);\n",
        "h.c": "/* check-memory: allow nothing is read between the fetch and the copy */\nSiftColRel *r = sift_col_rel(index);\n",
        "e.c": "for (uint64 w = 0; w <= m; w++) /* check-memory: allow m is a table size */\n",
    }
    want = {"col/other.c", "a.c", "f.c"}
    with tempfile.TemporaryDirectory() as d:
        for name, text in cases.items():
            os.makedirs(os.path.dirname(os.path.join(d, name)), exist_ok=True)
            with open(os.path.join(d, name), "w") as f:
                f.write(text)
        got = {b.split(":")[0] for b in scan(d)[0]}
    if got != want:
        print(f"check-memory selftest: flagged {sorted(got)}, wanted {sorted(want)}", file=sys.stderr)
        sys.exit(1)
    print("check-memory selftest: ok")


if sys.argv[2] == "--selftest":
    selftest()
    sys.exit(0)
bad, files = scan(sys.argv[1])
for b in bad:
    print(b)
if bad:
    print(f"check-memory: {len(bad)} site(s)", file=sys.stderr)
    sys.exit(1)
print(f"check-memory: ok, {files} files")
PY
