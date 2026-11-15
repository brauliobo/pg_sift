#!/usr/bin/env bash
# The focus tables name functions that exist (needs no PostgreSQL and no build): every FILE:FUNCTION of scripts/focus/*.tsv and every FILE of
# scripts/focus-pins.tsv must be a file of src/ (or the old name of a split file) that defines the function, or a file that scripts/moved-sources.tsv says was split into a folder that does.
# A function that moved to another file makes `scripts/ci.sh affected --focus` run no test for a change of its new place; the fix is
# `scripts/focus-gen.sh NAME...` for the rows named (no names: the whole table). A function that no file defines is not told: a macro may make its name
# (gcov names the generated functions), so a renamed or removed one is found only when the table is made again. A header is not asked where its functions are
# (a template header's functions are defined in the files that include it).
#
#   scripts/check-focus.sh [--selftest]
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
exec python3 -I - "$REPO" "${1:-}" <<'PY'
import glob, os, re, sys, tempfile

repo, mode = sys.argv[1], sys.argv[2]
cache = {}


def parse(path):
    """(the functions a file defines,), None for a file that is not there"""
    if path not in cache:
        cache[path] = None
        if os.path.isfile(path):
            names, text = set(), open(path, encoding="utf-8", errors="replace").read()
            head = ""
            for line in text.split("\n"):
                if line and line[0] in " \t}/*":
                    if head:
                        head += line
                    continue
                m = re.match(r"#\s*define\s+(\w+)", line)
                if m:
                    names.add(m.group(1))
                    head = ""
                    continue
                m = re.match(r"PG_FUNCTION_INFO_V1\((\w+)\)", line)
                if m:
                    names.add("pg_finfo_" + m.group(1))
                m = re.match(r"[A-Z][A-Z0-9_]+\((\w+)\b", line)
                if m:
                    names.add(m.group(1))
                if line.startswith("#"):
                    head = ""
                    continue
                head = line if re.match(r"\w+\s*\(", line) or not head else head + " " + line
                end = re.search(r"[;{]", head)
                if end:
                    # a definition reaches its brace before any semicolon: the name is the last word before the first parenthesis
                    m = re.match(r"[^(]*?(\w+)\s*\(", head) if end.group() == "{" else None
                    if m:
                        names.add(m.group(1))
                    head = ""
            cache[path] = (names,)
    return cache[path]


def check(root, tables, pins, moved):
    os.chdir(root)
    cache.clear()
    splits = [l.rstrip("\n").split("\t") for l in open(moved) if l.strip() and not l.startswith("#")]
    defined = {}

    def where(fn):
        if not defined:
            for p in glob.glob("src/**/*.[ch]", recursive=True):
                for n in parse(p)[0]:
                    defined.setdefault(n, p)
        return defined.get(fn)

    def in_split(path, fn):
        for new, old, pre in splits:
            if old != path:
                continue
            names = {fn, pre + fn} | ({fn[len(pre):]} if pre and fn.startswith(pre) else set())
            if any(names & parse(p)[0] for p in glob.glob(new + "**/*.[ch]", recursive=True)):
                return True
        return False

    bad = {}
    for table in tables:
        for line in open(table, encoding="utf-8"):
            row, _, tokens = line.rstrip("\n").partition("\t")
            for token in tokens.split():
                path, _, fn = token.partition(":")
                got = parse(path)
                if not fn and got or got and fn in got[0] or fn and in_split(path, fn):
                    continue
                elif got is None:
                    why = "is not a file of src/"
                elif where(fn) and not path.endswith(".h"):
                    why = "is defined in " + where(fn)
                else:
                    continue
                bad.setdefault((os.path.basename(table), token, why), set()).add(row)
    for pin in pins:
        for line in open(pin, encoding="utf-8"):
            if line.strip() and not line.startswith("#") and parse(line.split("\t")[0]) is None and line.split("\t")[0] not in [r[1] for r in splits]:
                bad.setdefault((os.path.basename(pin), line.split("\t")[0], "is not a file of src/"), set()).add("")
    return bad


def run(root):
    j = lambda *p: os.path.join(root, *p)
    return check(root, sorted(glob.glob(j("scripts", "focus", "*.tsv"))), [j("scripts", "focus-pins.tsv")], j("scripts", "moved-sources.tsv"))


if mode == "--selftest":
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(d + "/src/a/split")
        os.makedirs(d + "/scripts/focus")
        open(d + "/src/a/one.c", "w").write("static int\nkept(void)\n{\n\treturn 1;\n}\n#define PASTE(n) int f_##n(void) { return 0; }\n")
        open(d + "/src/a/two.c", "w").write("int\nmoved_here(void)\n{\n\treturn 1;\n}\nPG_FUNCTION_INFO_V1(sift_x);\n")
        open(d + "/src/a/split/in.c", "w").write("int\nsplit_fn(void)\n{\n\treturn 1;\n}\n")
        open(d + "/scripts/moved-sources.tsv", "w").write("src/a/split/\tsrc/a/old.c\t\n")
        open(d + "/scripts/focus-pins.tsv", "w").write("# pins\nsrc/a/gone.c\tt1\n")
        open(d + "/scripts/focus/t1.tsv", "w").write("t1\tsrc/a/one.c:kept src/a/two.c:pg_finfo_sift_x src/a/old.c:split_fn src/a/one.c:f_made\n")
        open(d + "/scripts/focus/t2.tsv", "w").write("t2\tsrc/a/one.c:moved_here src/a/two.c:renamed src/a/missing.c:kept\n")
        got = {(t, w) for (_, t, w) in run(d)}
        want = {("src/a/one.c:moved_here", "is defined in src/a/two.c"), ("src/a/missing.c:kept", "is not a file of src/"), ("src/a/gone.c", "is not a file of src/")}
        if got != want:
            print("check-focus: selftest: got %s, want %s" % (sorted(got), sorted(want)), file=sys.stderr)
            sys.exit(1)
    print("check-focus: selftest ok: a moved function and a missing file are told; split files, macro names, declarations and PG_FUNCTION_INFO_V1 are not")
    sys.exit(0)

bad = run(repo)
for (table, token, why), rows in sorted(bad.items()):
    print("check-focus: %s: %s %s (rows: %s%s)" % (table, token, why, " ".join(sorted(rows)[:3]), " ..." if len(rows) > 3 else ""), file=sys.stderr)
if bad:
    print("check-focus: %d stale reference(s): scripts/focus-gen.sh NAME... makes the rows again" % len(bad), file=sys.stderr)
    sys.exit(1)
print("check-focus: ok, %d rows" % sum(1 for t in glob.glob(repo + "/scripts/focus/*.tsv") for _ in open(t)))
PY
