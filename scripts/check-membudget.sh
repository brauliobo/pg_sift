#!/usr/bin/env bash
# The memory scopes of the main paths (core/membudget.h): every SIFT_MEM_SCOPE(name, ...) of src/ is a row of test/mem-budgets.tsv and of test/mem-required-scopes.tsv, every
# row of the budget table is used in src/, a required path in state `scoped` has its scope in its file, and the generated files (src/core/membudget_table.h, the table of
# docs/SIZING.md) match the table. The paths in state `todo` are listed, not failed. A fix is a scope in the path, never a row removed.
#
#   scripts/check-membudget.sh [--selftest]
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -I - "${1:-}" <<'PY'
import os, re, subprocess, sys, tempfile

def tsv(path):
    return [l.rstrip("\n").split("\t") for l in open(path) if l.strip() and not l.startswith("#")]

def scan(root):
    used = {}
    for d, _, names in os.walk(os.path.join(root, "src")):
        for n in names:
            if n.endswith(".c"):
                p = os.path.join(d, n)
                for m in re.finditer(r"\bSIFT_MEM_SCOPE\(\s*(\w+)\s*,", open(p).read()):
                    used.setdefault(m.group(1), set()).add(os.path.relpath(p, root))
    return used

def check(root):
    bad = []
    budgets = {r[0] for r in tsv(os.path.join(root, "test/mem-budgets.tsv"))}
    required = {r[0]: r for r in tsv(os.path.join(root, "test/mem-required-scopes.tsv"))}
    used = scan(root)
    for n in sorted(used):
        if n not in budgets:
            bad.append(f"SIFT_MEM_SCOPE({n}) in {sorted(used[n])[0]} has no row in test/mem-budgets.tsv: add the formula (scripts/gen-membudget.py) from the measured peak, do not drop the scope")
        if n not in required:
            bad.append(f"SIFT_MEM_SCOPE({n}) is not in test/mem-required-scopes.tsv: name the path (file, function) it asserts")
    for n in sorted(budgets - set(used)):
        bad.append(f"test/mem-budgets.tsv row {n} has no SIFT_MEM_SCOPE in src/: put the scope in the path, a row without one asserts nothing")
    for n, (_, f, fn, state) in sorted(required.items()):
        if state == "scoped" and f not in used.get(n, ()):
            bad.append(f"{f}: the path {fn} must declare SIFT_MEM_SCOPE({n}, <its own context>) at its entry: its budget is asserted by every test that runs it")
        if state == "todo" and f in used.get(n, ()):
            bad.append(f"test/mem-required-scopes.tsv: {n} is scoped in {f}, set its state to scoped")
        if state not in ("scoped", "todo"):
            bad.append(f"test/mem-required-scopes.tsv: {n} has state {state}, scoped or todo")
    return bad, [n for n, r in required.items() if r[3] == "todo"]

def selftest():
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as d:
        os.makedirs(f"{d}/src/x"); os.makedirs(f"{d}/test")
        def put(name, text):
            open(f"{d}/{name}", "w").write(text)
        put("test/mem-budgets.tsv", "a\tclosed\t\t1MB\t\tx\nb\tclosed\t\t1MB\t\tx\n")
        put("test/mem-required-scopes.tsv", "a\tsrc/x/a.c\tf\tscoped\nb\tsrc/x/b.c\tg\ttodo\n")
        put("src/x/a.c", "void f(void) { SIFT_MEM_SCOPE(a, cx); }\n")
        bad, todo = check(d)
        assert bad == ["test/mem-budgets.tsv row b has no SIFT_MEM_SCOPE in src/: put the scope in the path, a row without one asserts nothing"], bad
        put("src/x/a.c", "void f(void) { }\n"); put("src/x/b.c", "void g(void) { SIFT_MEM_SCOPE(b, cx); SIFT_MEM_SCOPE(c, cx); }\n")
        bad, _ = check(d)
        assert any("must declare SIFT_MEM_SCOPE(a" in b for b in bad) and any("row a has no" in b for b in bad) and any("SIFT_MEM_SCOPE(c)" in b for b in bad) \
            and any("b is scoped" in b for b in bad), bad
    print("check-membudget selftest ok")

if sys.argv[1] == "--selftest":
    selftest(); sys.exit(0)
root = os.getcwd()
bad, todo = check(root)
r = subprocess.run([sys.executable, "-I", "scripts/gen-membudget.py", "--check"])
for b in bad:
    print(b, file=sys.stderr)
if todo:
    print("memory scopes still to add: " + " ".join(sorted(todo)))
sys.exit(1 if bad or r.returncode else 0)
PY
