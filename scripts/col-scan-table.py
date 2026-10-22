#!/usr/bin/env python3
"""col-scan-table.py [--write docs/COL-SCAN.md]: the "which btree can be dropped" table of docs/COL-SCAN.md, from docs/results/col-scan.tsv and
col-scan-space.tsv. Without --write it prints the table; with it, it replaces the text between the two table markers of the file.
Times are the planner's pick, execution time in ms (the first query of a new backend, and the median of the last three of 24 runs of one session); sift_col is
the index with the operator classes and set_columns. Regenerate the data with bench/col_scan.py."""
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "..", "docs", "results")
START, END = "<!-- col-scan-table:start -->", "<!-- col-scan-table:end -->"

# (what the btree serves, query keys, verdict by the ratio at 1,000,000 rows)
CLASSES = [
    ("point lookup of a clustered key (`id = c`), rows", ["pt_id_rows"]),
    ("point lookup of a random int (`rnd = c`), rows", ["pt_rnd_rows"]),
    ("covering lookup (`SELECT id, rnd WHERE rnd = c`)", ["pt_rnd_cover"]),
    ("`IN` of 10 / 100 values, rows", ["in10_rows", "in100_rows"]),
    ("narrow range of a clustered key, rows", ["rng_narrow_rows"]),
    ("range of 0.1% of a random int, rows / covering", ["rng_rnd_narrow_rows", "rng_rnd_narrow_cover"]),
    ("equality on one column and a range on another, rows", ["mc_eq_rng_rows"]),
    ("unordered `LIMIT 10`", ["lim_grp10"]),
    ("`count(*)` of an `IN` of 100 values", ["in100_count"]),
    ("`count(*)` of a range of 20%", ["rng_wide_count"]),
    ("`DISTINCT`, `GROUP BY`", ["distinct_grp"]),
    ("text / uuid equality, rows", ["txt_eq_rows", "uuid_eq_rows"]),
    ("`LIKE 'abc%'`, rows", ["txt_like_rows"]),
    ("`ORDER BY .. LIMIT 10` / `LIMIT 1000`", ["ord10", "ord1000"]),
    ("`min(rnd), max(rnd)`", ["minmax"]),
]


def load(column="exec_median_ms"):
    t = {}
    with open(os.path.join(RES, "col-scan.tsv")) as f:
        for r in csv.DictReader(f, delimiter="\t"):
            if r[column]:
                t[(r["table_rows"], r["query_key"], r["index"], r["variant"])] = float(r[column])
    return t


def pick(t, n, key, index):
    """the planner's pick on the index; the set_columns index where it was measured, else the classes' index"""
    for idx in ("sift_col classes + set_columns", "sift_col classes"):
        v = t.get((n, key, idx, "planner")) if index == "sift" else None
        if v is not None:
            return v
    return t.get((n, key, "btree", "planner")) if index == "btree" else None


def ms(v):
    return "%.2f" % v if v >= 0.1 else "%.3f" % v


def verdict(ratios):
    worst = max(ratios)
    if worst < 1:
        return "drop the btree (faster)"
    if worst <= 2.5:
        return "drop the btree"
    if worst <= 10:
        return "drop it if %.0f times the time is fine" % worst
    return "keep the btree (%.0f times slower)" % worst


def table(column):
    t = load(column)
    out = ["| a btree that serves | sift_col / btree, 1,000,000 rows (ms) | 100,000 rows (ms) | ratio | verdict |", "|---|---|---|---|---|"]
    for label, keys in CLASSES:
        c1, c2, rs = [], [], []
        for k in keys:
            b1, s1 = pick(t, "1000000", k, "btree"), pick(t, "1000000", k, "sift")
            b2, s2 = pick(t, "100000", k, "btree"), pick(t, "100000", k, "sift")
            if None in (b1, s1):
                continue
            c1.append("%s / %s" % (ms(s1), ms(b1)))
            c2.append("%s / %s" % (ms(s2), ms(b2)) if None not in (b2, s2) else "")
            rs.append(s1 / b1)
        if not rs:
            continue
        out.append("| %s | %s | %s | %s | %s |" % (label, ", ".join(c1), ", ".join(c2), ", ".join(("%.1fx" % r) if r >= 0.1 else "%.2fx" % r for r in rs), verdict(rs)))
    out.append("| `UNIQUE`, `PRIMARY KEY`, `EXCLUDE`, the target of a foreign key | | | | keep the btree (no unique check, docs/COL-SCAN.md) |")
    return "\n".join(out)


def space():
    with open(os.path.join(RES, "col-scan-space.tsv")) as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    b = float(rows[0]["index_bytes"])
    return "\n".join(
        ["| 1,000,000 rows, 7 columns | index bytes | bytes a row | of the btrees |", "|---|---|---|---|"]
        + ["| %s | %s | %s | %.2fx |" % (r["index"], format(int(r["index_bytes"]), ","), r["bytes_per_row"], float(r["index_bytes"]) / b) for r in rows]
    )


def main():
    text = ("**A client that connects for each query**: the first query of a new backend (median of three sessions), which parses the dictionaries of its string\n"
            "columns and fills its caches.\n\n" + table("first_ms") + "\n\n"
            "**A backend that has run the query before**: the median of the last three of 24 runs in one session (a pooled connection).\n\n" + table("exec_median_ms") + "\n\n" + space())
    if "--write" not in sys.argv:
        print(text)
        return
    path = sys.argv[sys.argv.index("--write") + 1]
    s = open(path).read()
    a, b = s.index(START) + len(START), s.index(END)
    open(path, "w").write(s[:a] + "\n" + text + "\n" + s[b:])


main()
