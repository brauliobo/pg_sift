#!/usr/bin/env python3
"""The memory budgets of the main paths: one table, test/mem-budgets.tsv, and the two files made from it (src/core/membudget_table.h and the table of docs/SIZING.md).

A row is `scope  status  formula  constant  intended  rationale`:
  scope      the name of a SIFT_MEM_SCOPE(name, context) in src/ (an identifier)
  status     closed: the budget is formula + constant, and the tests assert it; open: a defect that another lane is fixing, the budget is the ceiling measured today
  formula    a sum of settings, `maintenance_work_mem`, `2*work_mem`, `hash_mem`, `sift.tomb_log_limit`: the tokens of TOKENS; empty for none
  constant   the fixed overhead, `8MB`, `512kB`, `64B`; for an open row the measured ceiling, and the budget is the constant alone
  intended   an open row: the formula it flips to when the fix lands (CONTRIBUTING.md, "Memory budgets"); empty for a closed one
  rationale  what the memory is

  scripts/gen-membudget.py            write src/core/membudget_table.h and the table of docs/SIZING.md
  scripts/gen-membudget.py --check    fail when either file differs from the table
  scripts/gen-membudget.py --open     the open rows, one a line (the CI report)
  scripts/gen-membudget.py --selftest
"""
import os, re, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
TSV = os.path.join(ROOT, "test", "mem-budgets.tsv")
HEADER = os.path.join(ROOT, "src", "core", "membudget_table.h")
DOC = os.path.join(ROOT, "docs", "SIZING.md")
BEGIN, END = "<!-- membudget:begin -->", "<!-- membudget:end -->"

# token -> (C expression in bytes, the header it needs)
TOKENS = {
    "work_mem": ("((Size) work_mem * 1024)", "miscadmin.h"),
    "maintenance_work_mem": ("((Size) maintenance_work_mem * 1024)", "miscadmin.h"),
    "col_build_budget": ("((Size) sift_colb_budget(1))", "col/build.h"),
    "hash_mem": ("((Size) get_hash_memory_limit())", "executor/nodeHash.h"),
    "sift.tomb_log_limit": ("((Size) sift_tomb_log_limit_kb * 1024)", "core/tomb.h"),
    "sift.table_write_buffer_limit": ("((Size) sift_tbl_write_buffer_limit * 1024)", "table/table.h"),
    "sift.verdict_cache_kb": ("((Size) sift_verdict_cache_kb * 1024)", "agg/agg.h"),
    "agg_nospill": ("((Size) sift_agg_nospill_bytes(NULL, NULL))", "agg/spec/spec.h"),
    "sift.agg_lookup_max_kb": ("((Size) sift_agg_lookup_max_kb * 1024)", "agg/agg.h"),
}
UNITS = {"B": 1, "kB": 1024, "MB": 1024 * 1024, "GB": 1024 * 1024 * 1024}
STATUSES = ("closed", "open")


def bytes_of(text):
    m = re.fullmatch(r"(\d+)\s*(B|kB|MB|GB)", text.strip())
    if not m:
        raise SystemExit(f"{TSV}: constant '{text}' is not N followed by B, kB, MB or GB")
    return int(m.group(1)) * UNITS[m.group(2)]


def terms(formula):
    out = []
    for t in [x.strip() for x in formula.split("+") if x.strip()]:
        m = re.fullmatch(r"(?:(\d+)\*)?([a-z_.]+)", t)
        if not m or m.group(2) not in TOKENS:
            raise SystemExit(f"{TSV}: formula term '{t}' is not [N*]token with a token of scripts/gen-membudget.py")
        out.append((int(m.group(1) or 1), m.group(2)))
    return out


def rows(path=TSV):
    res = []
    for n, line in enumerate(open(path), 1):
        if not line.strip() or line.startswith("#"):
            continue
        f = line.rstrip("\n").split("\t")
        if len(f) != 6:
            raise SystemExit(f"{path}:{n}: {len(f)} columns, 6 expected (scope status formula constant intended rationale)")
        scope, status, formula, constant, intended, why = f
        if not re.fullmatch(r"[a-z][a-z0-9_]*", scope):
            raise SystemExit(f"{path}:{n}: scope '{scope}' is not an identifier")
        if status not in STATUSES:
            raise SystemExit(f"{path}:{n}: status '{status}' is not closed or open")
        if (status == "open") != bool(intended):
            raise SystemExit(f"{path}:{n}: an open row names the formula it flips to (intended), a closed row has none")
        if status == "open" and formula:
            raise SystemExit(f"{path}:{n}: an open row has no formula: its budget is the ceiling measured today (constant)")
        terms(formula)
        if intended:
            terms(intended)
        bytes_of(constant)
        res.append(dict(scope=scope, status=status, formula=formula, constant=constant, intended=intended, why=why))
    names = [r["scope"] for r in res]
    dup = sorted({n for n in names if names.count(n) > 1})
    if dup:
        raise SystemExit(f"{path}: duplicate scopes {dup}")
    if names != sorted(names):
        raise SystemExit(f"{path}: rows are in byte order of the scope name (LC_ALL=C sort), so that two branches adding a scope merge")
    return res


def text(r):
    parts = [(f"{k}*" if k != 1 else "") + t for k, t in terms(r["formula"])] + [r["constant"].replace(" ", "")]
    return " + ".join(parts)


def c_expr(r):
    parts = [(f"{k} * " if k != 1 else "") + TOKENS[t][0] for k, t in terms(r["formula"])] + [f"((Size) {bytes_of(r['constant'])}UL)"]
    return "(" + " + ".join(parts) + ")"


def header(rs):
    inc = sorted({TOKENS[t][1] for r in rs for _, t in terms(r["formula"])})
    out = ["/* generated by scripts/gen-membudget.py from test/mem-budgets.tsv: do not edit. The budget of each SIFT_MEM_SCOPE (core/membudget.h), in bytes; included by core/membudget.c only. */",
           "#ifndef SIFT_MEMBUDGET_TABLE_H", "#define SIFT_MEMBUDGET_TABLE_H", ""]
    out += [f'#include "{i}"' for i in inc] + [""]
    for r in rs:
        out += [f"static Size", f"budget_{r['scope']}(void)", "{", f"\treturn {c_expr(r)};", "}", ""]
    out.append("static const struct")
    out.append("{")
    out += ["\tconst char *name;", "\tSize\t\t(*bytes) (void);", "\tconst char *formula;", "}\t\t\tbudgets[] = {"]
    for r in rs:
        note = " (open: ceiling measured today)" if r["status"] == "open" else ""
        out.append(f'\t{{"{r["scope"]}", budget_{r["scope"]}, "{text(r)}{note}"}},')
    out += ["};", "", "#endif", ""]
    return "\n".join(out)


def table(rs):
    out = [BEGIN, "<!-- generated by scripts/gen-membudget.py from test/mem-budgets.tsv: do not edit -->", "",
           "| scope | status | budget | what it holds |", "|---|---|---|---|"]
    for r in rs:
        b = text(r) if r["status"] == "closed" else f"open: {r['constant']} today, target {r['intended']} + a constant"
        out.append(f"| `{r['scope']}` | {r['status']} | {b} | {r['why']} |")
    out += ["", END]
    return "\n".join(out)


def doc_text(rs):
    cur = open(DOC).read()
    a, b = cur.find(BEGIN), cur.find(END)
    if a < 0 or b < a:
        raise SystemExit(f"{DOC}: the markers {BEGIN} and {END} are missing")
    return cur[:a] + table(rs) + cur[b + len(END):]


def main(argv):
    if argv[:1] == ["--selftest"]:
        return selftest()
    rs = rows()
    if argv[:1] == ["--open"]:
        for r in rs:
            if r["status"] == "open":
                print(f"{r['scope']}\tceiling {r['constant']}\tflips to {r['intended']} + {r['constant']}")
        return 0
    want = {HEADER: header(rs), DOC: doc_text(rs)}
    if argv[:1] == ["--check"]:
        bad = [p for p, t in want.items() if not os.path.exists(p) or open(p).read() != t]
        for p in bad:
            print(f"{os.path.relpath(p, ROOT)} is not what scripts/gen-membudget.py makes from test/mem-budgets.tsv: run it", file=sys.stderr)
        return 1 if bad else 0
    for p, t in want.items():
        open(p, "w").write(t)
    return 0


def selftest():
    import tempfile
    cases = [("a\tclosed\tmaintenance_work_mem\t8MB\t\tx\n", True), ("a\tclosed\tbogus\t8MB\t\tx\n", False),
             ("a\topen\t\t8MB\t\tx\n", False), ("a\topen\t\t8MB\twork_mem\tx\n", True),
             ("b\tclosed\t\t1MB\t\tx\na\tclosed\t\t1MB\t\tx\n", False), ("a\tclosed\t2*work_mem + hash_mem\t64kB\t\tx\n", True)]
    for body, ok in cases:
        with tempfile.NamedTemporaryFile("w", suffix=".tsv", dir=os.environ.get("TMPDIR")) as f:
            f.write(body)
            f.flush()
            try:
                got = bool(rows(f.name))
            except SystemExit:
                got = False
        if got != ok:
            print(f"selftest: {body!r} should {'pass' if ok else 'fail'}", file=sys.stderr)
            return 1
    print("gen-membudget selftest ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
