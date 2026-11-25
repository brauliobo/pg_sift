#!/usr/bin/env bash
# Capacity patterns of src/ that a grep can decide (needs no PostgreSQL and no build), the family of the page-generation incident (a count that outgrew the container it indexes):
#   - cast-after-product: `(uint64) (a * b)` multiplies in the type of a and b and widens the wrapped result; widen an operand, `(uint64) a * b`;
#   - blocknumber-sum: `BlockNumber x = a + b` wraps at 2^32 blocks (32 TB); add in 64 bits and compare with MaxBlockNumber, as core/freelist/alloc.c extend does;
#   - array-append: `arr[n++] = v` into an array declared with a constant size needs a bound the reader can see within the 12 lines above (a comparison of n, an Assert or
#     SIFT_INVARIANT, an error, Min, lengthof, a break), or a reason in scripts/capacity.allow.
# A site that is bounded some other way (a loop over the directory entries, a count checked by the caller, a double estimate) is a line of scripts/capacity.allow:
# RULE, PATH, the text of the line, the reason; one tab between them. A line that matches no site any more fails, so the list shrinks with the code.
#   scripts/check-capacity.sh [--selftest]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec python3 -I - "$ROOT" "${1:-}" <<'PY'
import os, re, subprocess, sys, tempfile

CAST_AFTER_PRODUCT = re.compile(r"\((?:uint64|int64|Size|size_t|uint64_t|int64_t)\)\s*\(\s*[A-Za-z_][A-Za-z0-9_>.\-]*\s*\*\s*[A-Za-z_(]")
BLOCK_SUM = re.compile(r"\bBlockNumber\s+\w+\s*=[^;]*[^-+]\+[^+=][^;]*;")
ARRAY_DECL = re.compile(r"^\s*(?:static\s+|const\s+)*[A-Za-z_][\w\s\*]*?\b(\w+)\[\s*([A-Z_][A-Z0-9_]*|\d+)\s*\](?:\[[^\]]*\])?\s*(?:=\s*\{[^;]*\})?;")
APPEND = re.compile(r"\b(\w+)\[(\w+(?:->\w+|\.\w+)?)\+\+\]\s*=")
SKIP = re.compile(r"[_/]test|kern|[_/]avx|[_/]neon|/vk")
COMMENT = re.compile(r"^\s*(/?\*|//)")


def norm(line):
    return re.sub(r"\s+", " ", line.strip())


def sites(root, files):
    found = []
    for rel in files:
        if SKIP.search(rel):
            continue
        lines = open(os.path.join(root, rel), errors="replace").read().split("\n")
        arrays = {}
        for i, line in enumerate(lines):
            m = ARRAY_DECL.match(line)
            if m:
                arrays[m.group(1)] = m.group(2)
        for i, line in enumerate(lines):
            if COMMENT.match(line):
                continue
            if CAST_AFTER_PRODUCT.search(line) and "sizeof" not in line:
                found.append(("cast-after-product", rel, norm(line), i + 1))
            if BLOCK_SUM.search(line) and "(uint64)" not in line and "(int64)" not in line:
                found.append(("blocknumber-sum", rel, norm(line), i + 1))
            for m in APPEND.finditer(line):
                array, counter = m.group(1), m.group(2)
                if array not in arrays:
                    continue
                context = "\n".join(lines[max(0, i - 12):i + 1])
                bound = r"(?:%s)\s*(?:<|>=|==|<=|>)|Assert|INVARIANT|ereport|elog|Min\(|lengthof|\bbreak\b" % re.escape(counter)
                if not re.search(bound, context):
                    found.append(("array-append", rel, norm(line), i + 1))
    return found


def allowed(root):
    path = os.path.join(root, "scripts", "capacity.allow")
    entries = {}
    if os.path.exists(path):
        for n, line in enumerate(open(path).read().split("\n"), 1):
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 4 or not parts[3].strip():
                print("capacity.allow:%d: RULE, PATH, the text of the line and the reason, a tab between them" % n)
                sys.exit(1)
            entries[tuple(parts[:3])] = n
    return entries


def tracked(root):
    out = subprocess.run(["git", "ls-files", "src/*.c", "src/*.h"], cwd=root, capture_output=True, text=True, check=True)
    return out.stdout.split()


def check(root, files):
    entries = allowed(root)
    found = sites(root, files)
    keys = {(r, p, t) for r, p, t, _ in found}
    bad = ["%s:%d: %s: %s" % (p, n, r, t) for r, p, t, n in found if (r, p, t) not in entries]
    stale = ["scripts/capacity.allow:%d: matches no site: %s %s" % (n, k[0], k[1]) for k, n in entries.items() if k not in keys]
    return bad, stale, len(files)


def selftest():
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "src"))
        os.makedirs(os.path.join(d, "scripts"))
        subprocess.run(["git", "init", "-q"], cwd=d, check=True)
        mutants = {
            "cast.c": "x = (uint64) (rows * width);\n",
            "block.c": "BlockNumber end = start + nblocks;\n",
            "append.c": "void f(void)\n{\n\tint\t\tout[SIFT_MAX_THINGS];\n\tint\t\tn = 0;\n\n\tfor (;;)\n\t\tout[n++] = 1;\n}\n",
        }
        clean = {
            "ok.c": "x = (uint64) rows * width;\ny = (double) (rows * width);\nBlockNumber e = (BlockNumber) Min((uint64) start + n, MaxBlockNumber);\n"
                    "/* x = (uint64) (a * b) in a comment */\nz = (Size) (n * sizeof(Oid));\n",
            "bounded.c": "void f(void)\n{\n\tint\t\tout[SIFT_MAX_THINGS];\n\tint\t\tn = 0;\n\n\tif (n >= SIFT_MAX_THINGS)\n\t\treturn;\n\tout[n++] = 1;\n}\n",
            "grown.c": "void f(int *p)\n{\n\tp[n++] = 1;\n}\n",
        }
        for name, text in {**mutants, **clean}.items():
            open(os.path.join(d, "src", name), "w").write(text)
        subprocess.run(["git", "add", "src"], cwd=d, check=True)
        files = tracked(d)
        found = {(r, p) for r, p, _, _ in sites(d, files)}
        for want in (("cast-after-product", "src/cast.c"), ("blocknumber-sum", "src/block.c"), ("array-append", "src/append.c")):
            if want not in found:
                print("check-capacity: selftest: %s %s was not found" % want)
                sys.exit(1)
        extra = {p for _, p in found} - {"src/cast.c", "src/block.c", "src/append.c"}
        if extra:
            print("check-capacity: selftest: a clean file was reported: %s" % sorted(extra))
            sys.exit(1)
        with open(os.path.join(d, "scripts", "capacity.allow"), "w") as f:
            for r, p, t, _ in sites(d, files):
                f.write("%s\t%s\t%s\treason\n" % (r, p, t))
            f.write("array-append\tsrc/gone.c\tnothing\treason\n")
        bad, stale, _ = check(d, files)
        if bad or len(stale) != 1:
            print("check-capacity: selftest: the allowlist did not clear the sites or did not find the stale line: %s %s" % (bad, stale))
            sys.exit(1)
    print("check-capacity: selftest ok")


root = sys.argv[1]
if sys.argv[2] == "--selftest":
    selftest()
    sys.exit(0)
bad, stale, count = check(root, tracked(root))
if bad or stale:
    print("check-capacity: a count that outgrows its container is a silent overwrite or a wrong answer. Fix the site at its cause: widen an operand before the multiplication,")
    print("add block numbers in 64 bits and compare with MaxBlockNumber, bound the append where the reader sees it. Only a site that is bounded elsewhere is a line of scripts/capacity.allow, with that bound as its reason.")
    print("\n".join(bad + stale))
    sys.exit(1)
print("check-capacity: ok, %d files" % count)
PY
