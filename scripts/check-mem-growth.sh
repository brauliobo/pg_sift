#!/usr/bin/env bash
# Memory that grows with the data, found by a grep (needs no PostgreSQL and no build). A statement or a backend holds what its setting gives (work_mem,
# maintenance_work_mem, a cache's own _kb) and no more: PostgreSQL's sorts, hashes and aggregates spill, its maintenance honours maintenance_work_mem.
# Four patterns of src/ break that, each is a rule; a site that is right says what bounds it in scripts/mem-growth.allow, never in a comment at the site:
#   loop      an allocation in the body of a loop over the rows of a scan or a unit (a table scan, nrows, ntuples, ndocs, nentries) with no context reset, delete or pfree in
#             that body: the loop holds memory for every row until its context goes. Allocate in a context the loop resets, or free what it makes;
#   double    an array grown by doubling (cap *= 2, cap <<= 1) with repalloc and no limit within 12 lines (MaxAllocSize, a *_MAX or *_LIMIT, a budget, Min(), huge): the
#             array passes 1 GB with "invalid memory alloc request size", or takes all the memory, on a large table. Bound it by a budget, or use the huge allocation where the
#             algorithm needs one array;
#   int       a size that multiplies two integers narrower than Size (int, uint32) in the argument of an allocation, with no sizeof, cast or MaxAllocSize in it: the product
#             wraps at 4 GB before it reaches the allocator and the array is short. Cast one factor to Size;
#   lifetime  memory that lives as long as the backend or the relcache entry (TopMemoryContext, CacheMemoryContext): a cache, a memo, a registry. It grows with the data or with
#             the statements unless something bounds it, so each site is named in the allowlist with its bound (a byte limit with eviction, a fixed count, the number of
#             relations, a setting) and a new one fails here until it has one. A cache needs a limit in bytes or a count before it needs a name.
# The allowlist rows are RULE<TAB>FILE<TAB>THE LINE (trimmed)<TAB>WHAT BOUNDS IT; a row no line matches any more fails (the line moved or went: delete the row).
#
#   scripts/check-mem-growth.sh [--selftest | --list]   --list prints every site found, allowed or not, as RULE FILE LINE-NUMBER TEXT
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
exec python3 -I - "$HERE/../src" "$HERE/mem-growth.allow" "${1:-}" <<'PY'
import os, re, sys, tempfile

ALLOC = r"(?:palloc0?|palloc_array|palloc0_array|MemoryContextAlloc(?:Zero)?|repalloc(?:_array)?)"
alloc = re.compile(r"\b" + ALLOC + r"\s*\(")
allocs = re.compile(r"\b(?:palloc0?|palloc_array|palloc0_array|MemoryContextAlloc(?:Zero)?|psprintf|pstrdup|datumCopy|heap_copytuple|ExecCopySlotHeapTuple|lappend\w*|"
                    r"list_make\w+|makeStringInfo|repalloc|text_to_cstring|cstring_to_text\w*)\s*\(")
loophead = re.compile(r"^\s*(?:for|while)\s*\((.*)\)\s*$")
rowloop = re.compile(r"(?:\btable_scan_getnextslot|\bheap_getnext|\bindex_getnext\w*|\btuplestore_gettupleslot|\btuplesort_get\w+|\bExecProcNode|\w*\bn?rows?\b|\w*ntuples?\b|"
                     r"\w*ndocs?\b|\w*nentr\w*|\w*nunits?\b|\w*ntids?\b|\w*npost\w*)")
release = re.compile(r"MemoryContext(?:Reset|Delete|SwitchTo)\w*|\bpfree\b|list_free\w*|resetStringInfo")
doubling = re.compile(r"(\w+(?:->|\.))*\w*cap\w*\s*(?:\*=\s*2|<<=\s*1)|\bcap\w*\s*=\s*(?:Max\()?[^;]*\*\s*2\b|\b\w*cap\w*\s*<<=\s*1")
capword = re.compile(r"MaxAllocSize|AllocSizeIsValid|sift_rows_alloc|sift_budget_check|get_hash_memory_limit|work_mem|\bMin\(|_MAX\b|_LIMIT\b|[Hh]uge|[Bb]udget|sift_limit|MAX_")
mulint = re.compile(r"\w[\w>.\-\[\]]*\s*\*\s*[\w(]")
sized = re.compile(r"\(Size\)|\(size_t\)|\(uint64\)|\(int64\)|\(Size \)|sizeof|MaxAllocSize|\bSize\b|\bsize_t\b")
lifetime = re.compile(r"(?:AllocSetContextCreate|MemoryContextAlloc(?:Zero|Huge)?|MemoryContextSwitchTo|MemoryContextStrdup|\.hcxt\s*=|hcxt\s*=|MemoryContextAllocExtended)\s*\(?[^;]*"
                      r"\b(?:TopMemoryContext|CacheMemoryContext)\b|\bhcxt\s*=\s*(?:TopMemoryContext|CacheMemoryContext)\b")
SKIP = ("_test", "/test")


def code_of(line):
    c = line.split("//")[0]
    return "" if c.lstrip().startswith(("/*", "*")) else c


def body(L, j):
    depth, k = 0, j
    while k < len(L):
        depth += L[k].count("{") - L[k].count("}")
        if depth == 0:
            break
        k += 1
    return L[j:k + 1]


def scan(src):
    found, files = [], 0
    for root, _, names in os.walk(src):
        for name in sorted(names):
            path = os.path.join(root, name)
            rel = os.path.relpath(path, src)
            if not name.endswith(".c") or any(s in rel or s in name for s in SKIP):
                continue
            files += 1
            with open(path, encoding="utf-8") as f:
                L = f.read().split("\n")
            for i, raw in enumerate(L):
                c = code_of(raw)
                line = " ".join(raw.split())
                m = loophead.match(c)
                if m and rowloop.search(m.group(1)) and i + 1 < len(L) and L[i + 1].strip() == "{":
                    b = "\n".join(code_of(x) for x in body(L, i + 1))
                    if allocs.search(b) and not release.search(b):
                        found.append(("loop", rel, i + 1, line, "an allocation in a loop over rows with no reset, delete or pfree: it holds memory for every row until the context goes; allocate in a context the loop resets, or free what it makes"))
                a = alloc.search(c)
                if a:
                    args = c[a.end():]
                    near = "\n".join(code_of(x) for x in L[max(0, i - 12):i + 4])
                    if doubling.search(c) or (a.group(0).startswith("repalloc") and doubling.search("\n".join(code_of(x) for x in L[max(0, i - 3):i + 1]))):
                        if not capword.search(near):
                            found.append(("double", rel, i + 1, line, "an array grown by doubling with no limit near it: past 1 GB it fails with 'invalid memory alloc request size', or it takes all the memory; bound it by a budget (core/rowmem.h), or take the huge allocation where one array is the algorithm"))
                    if mulint.search(args) and not sized.search(args) and "sizeof" not in args and re.search(r"[*]|<<", args):
                        found.append(("int", rel, i + 1, line, "a size computed from integers narrower than Size: the product wraps at 4 GB before the allocator sees it; cast a factor to Size"))
                if lifetime.search(c):
                    found.append(("lifetime", rel, i + 1, line, "memory that lives as long as the backend or the relcache entry: bound it (a byte limit with eviction, a fixed count, the number of relations, a setting) and name the bound in scripts/mem-growth.allow"))
    return found, files


def load_allow(path):
    rows = {}
    if os.path.exists(path):
        for n, l in enumerate(open(path, encoding="utf-8").read().split("\n"), 1):
            if not l.strip() or l.startswith("#"):
                continue
            p = l.split("\t")
            if len(p) != 4 or not p[3].strip():
                print(f"{path}:{n}: a row is RULE<TAB>FILE<TAB>LINE<TAB>WHAT BOUNDS IT, and the last is not empty", file=sys.stderr)
                sys.exit(1)
            rows[(p[0], p[1], p[2].strip())] = n
    return rows


def check(src, allow_path):
    found, files = scan(src)
    allow = load_allow(allow_path)
    used, bad = set(), []
    for rule, rel, n, line, why in found:
        k = (rule, rel, line)
        if k in allow:
            used.add(k)
        else:
            bad.append(f"{rel}:{n}: [{rule}] {line[:110]}\n    {why}")
    stale = [f"{allow_path}:{n}: no line matches [{k[0]}] {k[1]}: {k[2][:90]}" for k, n in allow.items() if k not in used]
    return bad, stale, files


def selftest():
    cases = {
        "a.c": "void f(void)\n{\n\tfor (int i = 0; i < nrows; i++)\n\t{\n\t\tchar *s = palloc(10);\n\t}\n}\n",
        "b.c": "void f(void)\n{\n\tfor (int i = 0; i < nrows; i++)\n\t{\n\t\tchar *s = palloc(10);\n\t\tpfree(s);\n\t}\n}\n",
        "c.c": "void f(void)\n{\n\tfor (int i = 0; i < nrows; i++)\n\t{\n\t\tMemoryContextReset(cx);\n\t\tq = palloc(10);\n\t}\n}\n",
        "d.c": "void f(void)\n{\n\tif (n == cap)\n\t\tp = repalloc(p, (cap *= 2) * sizeof(int));\n}\n",
        "e.c": "void f(void)\n{\n\tif (cap >= FOO_MAX)\n\t\treturn;\n\tif (n == cap)\n\t\tp = repalloc(p, (cap *= 2) * sizeof(int));\n}\n",
        "f.c": "void f(void)\n{\n\tp = palloc(n * m);\n}\n",
        "g.c": "void f(void)\n{\n\tp = palloc((Size) n * m);\n\tq = palloc(n * sizeof(int));\n}\n",
        "h.c": "void f(void)\n{\n\tcx = AllocSetContextCreate(TopMemoryContext, \"x\", ALLOCSET_DEFAULT_SIZES);\n}\n",
        "i.c": "void f(void)\n{\n\tcx = AllocSetContextCreate(CurrentMemoryContext, \"x\", ALLOCSET_DEFAULT_SIZES);\n}\n",
        "j.c": "void f(void)\n{\n\tHASHCTL ctl = {.hcxt = CacheMemoryContext};\n}\n",
        "k_test.c": "void f(void)\n{\n\tcx = AllocSetContextCreate(TopMemoryContext, \"x\", ALLOCSET_DEFAULT_SIZES);\n}\n",
    }
    want = {"a.c", "d.c", "f.c", "h.c", "j.c"}
    with tempfile.TemporaryDirectory() as d:
        for name, text in cases.items():
            with open(os.path.join(d, name), "w") as f:
                f.write(text)
        bad, _, _ = check(d, os.path.join(d, "none.allow"))
        got = {b.split(":")[0] for b in bad}
        allow = os.path.join(d, "x.allow")
        with open(allow, "w") as f:
            f.write("lifetime\th.c\tcx = AllocSetContextCreate(TopMemoryContext, \"x\", ALLOCSET_DEFAULT_SIZES);\tone per backend, 8 kB\nlifetime\tgone.c\tx\tgone\n")
        bad2, stale, _ = check(d, allow)
        got2 = {b.split(":")[0] for b in bad2}
    if got != want or got2 != want - {"h.c"} or len(stale) != 1:
        print(f"check-mem-growth selftest: flagged {sorted(got)} (wanted {sorted(want)}), with an allowlist {sorted(got2)}, stale {len(stale)}", file=sys.stderr)
        sys.exit(1)
    print("check-mem-growth selftest: ok")


if sys.argv[3] == "--selftest":
    selftest()
    sys.exit(0)
if sys.argv[3] == "--list":
    for rule, rel, n, line, _ in scan(sys.argv[1])[0]:
        print(f"{rule}\t{rel}\t{n}\t{line}")
    sys.exit(0)
bad, stale, files = check(sys.argv[1], sys.argv[2])
for b in bad + stale:
    print(b)
if bad or stale:
    print(f"check-mem-growth: {len(bad)} site(s) without a bound, {len(stale)} stale allowlist row(s)", file=sys.stderr)
    sys.exit(1)
print(f"check-mem-growth: ok, {files} files")
PY
