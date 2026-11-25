#!/usr/bin/env bash
# Memory-safety patterns of src/ that a grep can decide (needs no PostgreSQL and no build), see docs/SAFETY.md "Relation cache state":
#   - rd_amcache, the slot the relcache frees whenever it reloads the entry, is touched only by the files that own an accessor of it
#     (col/am/rel.c sift_col_rel, bm25/wbuf.c get_hint, agg/stub_src.c, table/v1/store.c sift_tbl_rel, which only tests it for NULL): every other file asks the accessor at the point of use;
#   - a local that keeps the result of the accessor, `SiftColRel *r = sift_col_rel(index)`, is a pointer into memory the relcache frees: it
#     is read after a lock or a catalog read only if the entry was fetched again. A site says why none comes between the fetch and its last
#     use (or takes sift_col_rel_private, which is not flagged); the earlier report, sift.col_verify and the cost estimate of a scan all kept one;
#   - a loop `for (int64 k = lo; k <= hi; k++)` over a 64-bit counter never ends when hi is the largest value of the type (the key loop of
#     the point directory ran past its array on `timestamp = 'infinity'`): count the iterations, or say why hi is bounded.
#   A 64-bit loop whose condition also bounds the counter with `<` is fine. Any other site says `check-memory: allow <why>` in a comment on
#   the line or the line above it. Test-only files are checked like the others.
#   - a plain palloc/repalloc/MemoryContextAlloc (not huge) whose size multiplies a count named for rows, tuples, values,
#     documents, units, slots, keys, entries, postings or blocks: the allocation that follows the table is the one that fails a build of hundreds of millions of rows
#     at 1 GB ("invalid memory alloc request size", 16 bytes a row in one segment). Bound it by a memory budget, say what bounds the count: `check-memory: allow <why>` on the line
#     or the line above, or `check-memory: allow-allocs <why>` in the first 40 lines of a file whose arrays all follow one unit, one plan or one query.
#   - the memory budget rules of the three-budget model (core/memtrack.h, core/cachebudget.h), each counted for every file against scripts/memory-static.allow (rule, file, the
#     sites the file has today): a file over its count, or a file with sites and no row, fails; a count above what the file has fails too, so that a clean-up lowers the row.
#       huge: MemoryContextAllocHuge, repalloc_huge, SIFT_REGROW_HUGE_IN: an allocation of one array that follows the rows is a spill, a run or a window of a tracker's budget
#       toplevel: TopMemoryContext, CacheMemoryContext outside core/cachebudget.c: every cache is a child of sift_cache_create_context()
#       defaultsizes: ALLOCSET_DEFAULT_SIZES, ALLOCSET_DEFAULT_MAXSIZE: a transient context takes sift_mem_allocset_create() or sift_mem_generation_create(), whose blocks are at most 256 kB
#       legacy: a memory setting or limit that core/memtrack.h and core/cachebudget.h replaced (no row is allowed)
#       raw_mwm: maintenance_work_mem outside core/memtrack.c: a maintenance, VACUUM or build takes sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, ...), which reads autovacuum_work_mem in an autovacuum worker
#       rowbuf_unbudgeted: a file that calls sift_rowbuf_new() and never reads mem_bytes, sift_mem_budget_bytes or sift_mem_pressure: its rows are held whole, whatever the table
#       rowbuf_notrim: a file that bounds its rows by a budget and never calls sift_mem_release_os() once a bounded batch is flushed
#     A string literal is not code (an errmsg that names maintenance_work_mem is no read of it). A call that holds one granule (at most 8192 rows) says so: `check-memory: allow bounded to one granule`.
#     --strict: no site of raw_mwm, rowbuf_unbudgeted or rowbuf_notrim in col/f2, table/f2, core/f2 or bm25 may be left, a row of scripts/memory-static.allow or not (the Format 2.0 and
#     BM25 paths honour the budget of the process); the rows of Format 1 stay a ratchet.
#
#   scripts/check-memory.sh [--selftest|--strict]
set -euo pipefail

SRC=$(cd "$(dirname "$0")/../src" && pwd)
exec python3 -I - "$SRC" "${1:-}" <<'PY'
import collections, os, re, sys, tempfile

AMCACHE_FILES = {"col/am/rel.c", "bm25/wbuf.c", "agg/stub_src.c", "table/v1/store.c", "table/am.c"}
loop = re.compile(r"\bfor\s*\(\s*(?:const\s+)?(?:int64|uint64|int64_t|uint64_t)\s+(\w+)\s*=[^;]*;\s*\1\s*<=([^;]*);")
amcache = re.compile(r"\brd_amcache\b")
alloc = re.compile(r"\b(palloc0?|repalloc|MemoryContextAlloc(?:Zero)?|palloc_array|palloc0_array|repalloc_array)\s*\(")
counted = re.compile(r"\b(\w*rows?|\w*ntuples?|\w*ntids?|\w*nentr\w*|\w*nvals?|\w*ndocs?|\w*nunits?|total|\w*nslots?|nstr|\w*nkeys?|\w*npost\w*|\w*ntomb\w*|\w*nblocks?)\b")
STATIC_RULES = {
    "huge": re.compile(r"\b(MemoryContextAllocHuge|repalloc_huge|SIFT_REGROW_HUGE_IN)\b"),
    "toplevel": re.compile(r"\b(TopMemoryContext|CacheMemoryContext)\b"),
    "defaultsizes": re.compile(r"\bALLOCSET_DEFAULT_(SIZES|MAXSIZE)\b"),
    "raw_mwm": re.compile(r"\bmaintenance_work_mem\b"),
    "legacy": re.compile(r"\b(sift_agg_lookup_cache_kb|sift_agg_lookup_shared_kb|sift_agg_dimmap_kb|sift_verdict_cache_kb|sift_col_dict_shared_cache_kb|sift_col_leaf_cache_kb|"
                         r"sift_prefix_cache_size|CDICT_CACHE_MAX_KB|SEGCACHE_MAX_KB|BATCH_SET_BYTES|BATCH_AMPLIFY|WBUF_CACHE_ROWS|DEAD_BYTES|LEAF_STATS_CACHE|"
                         r"sift_agg_semi_lut_size|sift_agg_param_set_size|sift_agg_lookup_max_kb|sift_agg_lookup_max_rows|sift_agg_combo_cache|sift_col_auto_merge_pass_mb|"
                         r"sift_col_auto_merge_partition_kb|sift_tomb_log_limit_kb|SIFT_TOMB_LOG_KB|SIFT_COL_BUILD_SERIAL_BUDGET|SIFT_COL_BUILD_MIN_BUDGET|SIFT_COLW_MIN_BUDGET|"
                         r"SIFT_COLW_(?:BUILD|MERGE)_(?:PCT|BASE)|WINDOW_MIN|PART_MIN_KB|SCOPE_SORT_MIN_KB|GROUP_NOSPILL_BASE|GROUP_NOSPILL_FACTOR|GROUP_PAR_FACTOR|"
                         r"SPILL_RUN_BYTES|SIFT_ARRAY_UNLIMITED|sift_rows_alloc|sift_colw_memory|sift_colw_budget|sift_colw_row_memory|sift_colw_leaves_memory)\b"),
}
STATIC_EXEMPT = {"toplevel": {"core/cachebudget.c"}, "raw_mwm": {"core/memtrack.c"}}
ROWBUF = re.compile(r"\bsift_rowbuf_new\s*\(")
ROWBUF_BUDGET = re.compile(r"\b(mem_bytes|sift_mem_budget_bytes|sift_mem_pressure)\b")
ROWBUF_OWNER = "col/f2/rowbuf/rowbuf.c"
STRING = re.compile(r'"(?:\\.|[^"\\])*"')
BLOCK_COMMENT = re.compile(r"/\*.*?(\*/|$)")
STRICT_RULES = ("raw_mwm", "rowbuf_unbudgeted", "rowbuf_notrim")
STRICT_DIRS = ("col/f2/", "table/f2/", "core/f2/", "bm25/")
STRICT_FIX = {
    "raw_mwm": "read the budget with sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, ...)",
    "rowbuf_unbudgeted": "flush the rows at a mem_bytes budget from sift_mem_budget_bytes(), or say what bounds them",
    "rowbuf_notrim": "call sift_mem_release_os() after every flushed batch",
}
MESSAGES = {
    "raw_mwm": "{n} site(s) of raw maintenance_work_mem (scripts/memory-static.allow allows {have}): use sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, ...) so that autovacuum_work_mem is honored",
    "rowbuf_unbudgeted": "{n} sift_rowbuf_new() site(s) in a file with no memory budget (scripts/memory-static.allow allows {have}): flush the rows at a mem_bytes budget from sift_mem_budget_bytes(), or say what bounds them",
    "rowbuf_notrim": "{n} sift_rowbuf_new() site(s) in a file that never calls sift_mem_release_os() (scripts/memory-static.allow allows {have}): return the memory of every flushed batch to the OS",
}
held = re.compile(r"\bSiftColRel\s*\*\s*\w+\s*=\s*sift_col_rel\(")


def code_of(line):
    return STRING.sub('""', line).split("//")[0]


def scan(src, allow=None, strict=False):
    bad, files = [], 0
    sites = collections.Counter()
    for root, _, names in os.walk(src):
        for name in sorted(names):
            if not name.endswith((".c", ".h")):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, src)
            with open(path, encoding="utf-8") as f:
                lines = f.read().split("\n")
            files += 1
            allocs_ok = any("check-memory: allow-allocs" in l for l in lines[:40])
            budgeted = bool(ROWBUF_BUDGET.search("\n".join(code_of(l) for l in lines)))
            trims = any("sift_mem_release_os" in code_of(l) for l in lines)
            for i, line in enumerate(lines):
                if "check-memory: allow" in line or (i and "check-memory: allow" in lines[i - 1]):
                    continue
                code = code_of(line)
                if code.lstrip().startswith(("/*", "*")):
                    continue
                for rule, rx in STATIC_RULES.items():
                    if rx.search(BLOCK_COMMENT.sub("", code) if rule == "raw_mwm" else code) and rel not in STATIC_EXEMPT.get(rule, ()):
                        sites[(rule, rel)] += 1
                if ROWBUF.search(code) and name.endswith(".c") and not name.endswith("_test.c") and rel != ROWBUF_OWNER and not (budgeted and trims):
                    sites[("rowbuf_unbudgeted" if not budgeted else "rowbuf_notrim", rel)] += 1
                if amcache.search(code) and rel not in AMCACHE_FILES:
                    bad.append(f"{rel}:{i + 1}: rd_amcache outside its accessors ({', '.join(sorted(AMCACHE_FILES))}): the relcache frees it on every reload, ask the accessor where it is used")
                if held.search(code):
                    bad.append(f"{rel}:{i + 1}: a local kept from sift_col_rel(): the relcache frees that state on every reload (a lock, a catalog read); fetch it where it is used, take sift_col_rel_private, or allow with the reason nothing of the kind comes before its last use")
                m = alloc.search(code)
                if m and not allocs_ok and name.endswith(".c"):
                    args = code[m.end():]
                    if ("sizeof" in args or "array" in m.group(1)) and counted.search(args):
                        bad.append(f"{rel}:{i + 1}: allocation sized by a count of rows, values or units: a table of hundreds of millions of rows makes this one fail at 1 GB; bound it by a memory budget (core/memtrack.h), or allow with what bounds the count")
                m = loop.search(code)
                if m and not re.search(r"&&.*\b" + m.group(1) + r"\s*<[^=]|\b" + m.group(1) + r"\s*<[^=].*&&", m.group(2) + ";"):
                    bad.append(f"{rel}:{i + 1}: inclusive loop over a 64-bit counter wraps when the bound is the largest value: count the iterations, or allow with the reason the bound is small")
    if allow is not None:
        bad += ratchet(sites, allow)
    if strict:
        bad += [f"{rel}: {n} site(s) of rule `{rule}` in a Format 2.0 or BM25 path, which no row of scripts/memory-static.allow excuses: " + STRICT_FIX[rule]
                for (rule, rel), n in sorted(sites.items()) if rule in STRICT_RULES and rel.startswith(STRICT_DIRS)]
    return bad, files


def ratchet(sites, allow):
    bad = []
    for (rule, rel), n in sorted(sites.items()):
        have = allow.get((rule, rel), 0)
        if rule == "legacy" and n:
            bad.append(f"{rel}: {n} site(s) of a memory setting or limit that core/memtrack.h and core/cachebudget.h replaced: use the budget it was folded into, not the old name")
        elif n > have and rule in MESSAGES:
            bad.append(f"{rel}: " + MESSAGES[rule].format(n=n, have=have))
        elif n > have:
            bad.append(f"{rel}: {n} site(s) of rule `{rule}` (scripts/memory-static.allow allows {have}): fix the allocation to take the budget of its tracker or cache, do not add a row")
    for (rule, rel), have in sorted(allow.items()):
        if sites.get((rule, rel), 0) < have:
            bad.append(f"scripts/memory-static.allow: `{rule} {rel}` allows {have} and the file has {sites.get((rule, rel), 0)}: lower the row")
    return bad


def read_allow(path):
    allow = {}
    for line in open(path, encoding="utf-8"):
        if line.strip() and not line.startswith("#"):
            rule, rel, n = line.rstrip("\n").split("\t")
            allow[(rule, rel)] = int(n)
    return allow


def selftest_rules():
    cases = {
        "core/memtrack.c": "w = maintenance_work_mem;\n",
        "col/f2/msg.c": 'ereport(ERROR, (errmsg("raise maintenance_work_mem")));\nx = a; // maintenance_work_mem\nint n; /* the processes that share maintenance_work_mem */\n',
        "col/f2/foo.c": "w = maintenance_work_mem;\n",
        "col/f2/a.c": "rb = sift_rowbuf_new(cx, n);\n",
        "col/f2/b.c": "rb = sift_rowbuf_new(cx, n);\nif (rb->mem_bytes >= budget)\n\tflush(rb);\n",
        "col/f2/c.c": "rb = sift_rowbuf_new(cx, n);\nif (rb->mem_bytes >= budget)\n{\n\tflush(rb);\n\tsift_mem_release_os();\n}\n",
        "col/f2/d.c": "/* check-memory: allow bounded to one granule */\nrb = sift_rowbuf_new(cx, 8192);\n",
        "col/f2/rowbuf/rowbuf.c": "sift_rowbuf *sift_rowbuf_new(MemoryContext cx, int n)\n",
        "col/f2/e_test.c": "rb = sift_rowbuf_new(cx, n);\n",
        "col/v1/g.c": "w = maintenance_work_mem;\nrb = sift_rowbuf_new(cx, n);\n",
    }
    with tempfile.TemporaryDirectory() as d:
        for name, text in cases.items():
            os.makedirs(os.path.dirname(os.path.join(d, name)), exist_ok=True)
            with open(os.path.join(d, name), "w") as f:
                f.write(text)
        allow = {("raw_mwm", "col/v1/g.c"): 1, ("rowbuf_unbudgeted", "col/v1/g.c"): 1}
        got = sorted(b.split(":")[0] for b in scan(d, allow)[0])
        want = ["col/f2/a.c", "col/f2/b.c", "col/f2/foo.c"]
        if got != want:
            print(f"check-memory selftest: the budget rules flagged {got}, wanted {want}", file=sys.stderr)
            sys.exit(1)
        strict = sorted(b.split(":")[0] for b in scan(d, {**allow, ("raw_mwm", "col/f2/foo.c"): 1, ("rowbuf_unbudgeted", "col/f2/a.c"): 1, ("rowbuf_notrim", "col/f2/b.c"): 1}, True)[0])
        if strict != want:
            print(f"check-memory selftest: --strict flagged {strict}, wanted {want} (a row of a Format 2.0 file is no exemption, one of Format 1 is)", file=sys.stderr)
            sys.exit(1)


def selftest():
    cases = {
        "col/am/rel.c": "x = index->rd_amcache;\n",
        "col/other.c": "x = index->rd_amcache;\n",
        "a.c": "for (int64 k = lo; k <= hi; k++)\n",
        "b.c": "for (int64 k = lo; k <= hi && k < n; k++)\n",
        "c.c": "/* check-memory: allow hi is an int32 */\nfor (int64 k = lo; k <= hi; k++)\n",
        "d.c": "for (int k = lo; k <= hi; k++)\n",
        "f.c": "SiftColRel *r = sift_col_rel(index);\n",
        "g.c": "SiftColRel *r = sift_col_rel_private(index, cx);\n",
        "h.c": "/* check-memory: allow nothing is read between the fetch and the copy */\nSiftColRel *r = sift_col_rel(index);\n",
        "e.c": "for (uint64 w = 0; w <= m; w++) /* check-memory: allow m is a table size */\n",
        "p.c": "e = palloc(n * sizeof(PointEnt));\nq = palloc(nrows * sizeof(uint32));\n",
        "q.c": "w = MemoryContextAllocHuge(cx, nrows * 8);\nz = palloc(ncols * sizeof(int));\n",
        "r.c": "/* check-memory: allow a unit has at most 4096 rows */\nq = palloc(nrows * sizeof(uint32));\n",
        "s.c": "/* check-memory: allow-allocs every array here follows one unit */\nq = palloc0(nrows * sizeof(uint32));\nq = palloc_array(uint32, ntuples);\n",
        "t.c": "q = palloc_array(uint32, ntuples);\n",
    }
    want = {"col/other.c", "a.c", "f.c", "p.c", "t.c"}
    with tempfile.TemporaryDirectory() as d:
        for name, text in cases.items():
            os.makedirs(os.path.dirname(os.path.join(d, name)), exist_ok=True)
            with open(os.path.join(d, name), "w") as f:
                f.write(text)
        got = {b.split(":")[0] for b in scan(d)[0]}
    if got != want:
        print(f"check-memory selftest: flagged {sorted(got)}, wanted {sorted(want)}", file=sys.stderr)
        sys.exit(1)
    with tempfile.TemporaryDirectory() as d:
        for name, text in {"x.c": "p = MemoryContextAllocHuge(cx, n);\n", "y.c": "cx = AllocSetContextCreate(TopMemoryContext, \"n\", ALLOCSET_DEFAULT_SIZES);\n",
                           "core/cachebudget.c": "cx = AllocSetContextCreate(TopMemoryContext, \"n\", 8192, 8192, 65536);\n", "z.c": "int k = sift_agg_dimmap_kb;\n"}.items():
            os.makedirs(os.path.dirname(os.path.join(d, name)), exist_ok=True)
            with open(os.path.join(d, name), "w") as f:
                f.write(text)
        got = sorted(b.split(":")[0] for b in scan(d, {("huge", "x.c"): 1, ("toplevel", "gone.c"): 1, ("legacy", "z.c"): 1})[0])
    want = ["scripts/memory-static.allow", "y.c", "y.c", "z.c"]
    if got != want:
        print(f"check-memory selftest: the memory rules flagged {got}, wanted {want}", file=sys.stderr)
        sys.exit(1)
    selftest_rules()
    print("check-memory selftest: ok")


if sys.argv[2] == "--selftest":
    selftest()
    sys.exit(0)
bad, files = scan(sys.argv[1], read_allow(os.path.join(sys.argv[1], "..", "scripts", "memory-static.allow")), sys.argv[2] == "--strict")
for b in bad:
    print(b)
if bad:
    print(f"check-memory: {len(bad)} site(s)", file=sys.stderr)
    sys.exit(1)
print(f"check-memory: ok, {files} files")
PY
