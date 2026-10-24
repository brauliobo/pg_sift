#!/usr/bin/env python3
"""Which btree a sift_col index can replace (docs/COL-SCAN.md): the same queries on tables items_N under a btree per query column and under one sift_col index.

  bench/col_scan.py load DB ROWS                         create items_ROWS (id ascending, rnd random, grp 100 values, ts ascending, s_short 8 characters, u uuid, amount)
  bench/col_scan.py run  DB ROWS KIND [--only=a,b] >> col-scan.tsv   time the queries with only the indexes of KIND
  bench/col_scan.py space DB ROWS > col-scan-space.tsv   the size of every kind of index

KIND: btree (one per query column), classes (one sift_col index with the operator classes), set (the same with key_columns = $SET_COLUMNS, default 'id, rnd, s_short, u'),
point (the same with point_columns = $POINT_COLUMNS, default 'id, rnd, grp, s_short, u'), point_tid (the same with point_tid_columns: the directory lists heap addresses and the lookup reads no unit).
PG* variables pick the server. Each query and plan runs RUNS = 24 times in one session: the median of the last three is exec_median_ms (EXPLAIN (ANALYZE, TIMING OFF) Execution
Time), a backend that has run the query before (and has asked a dictionary often enough for the lookup structures built beside it, col/dict.c: 8
equalities, 16 prefix tests). first_ms is the first run of a new backend (it parses the dictionaries of the string columns it reads, fills its caches):
the median of the first runs of three sessions, the way a client that connects for each query sees it.
Plans: planner (sift.enable_col_scan on for sift_col, the planner's pick), and for a forced plan: an index scan, a bitmap scan; `scan off` is the
heap, or the btree, as PostgreSQL plans it without the column scan. No parallel workers.
"""
import json, os, random, statistics, subprocess, sys

ENV = dict(os.environ, LC_ALL="C")
NAMES = {"btree": "btree", "classes": "sift_col classes", "set": "sift_col classes + key_columns", "point": "sift_col classes + point_columns",
         "point_tid": "sift_col classes + point_tid_columns"}
OPS = ("id sift.col_int4_ops, rnd sift.col_int4_ops, grp sift.col_int4_ops, ts sift.col_timestamptz_ops, "
       "s_short sift.col_text_ops, u sift.col_uuid_ops, amount sift.col_numeric_ops")
SET_COLUMNS = os.environ.get("SET_COLUMNS", "id, rnd, s_short, u")
POINT_COLUMNS = os.environ.get("POINT_COLUMNS", "id, rnd, grp, s_short, u")
INDEXES = {
    "btree": ["CREATE INDEX b_id ON T (id)", "CREATE INDEX b_rnd ON T (rnd) INCLUDE (id)", "CREATE INDEX b_grp_ts ON T (grp, ts)",
              "CREATE INDEX b_s ON T (s_short)", "CREATE INDEX b_u ON T (u)"],
    "classes": [f"CREATE INDEX c_scan ON T USING sift_col ({OPS})"],
    "set": [f"CREATE INDEX c_set ON T USING sift_col ({OPS}) WITH (key_columns = '{SET_COLUMNS}')"],
    "point": [f"CREATE INDEX c_point ON T USING sift_col ({OPS}) WITH (point_columns = '{POINT_COLUMNS}')"],
    "point_tid": [f"CREATE INDEX c_point_tid ON T USING sift_col ({OPS}) WITH (point_tid_columns = '{POINT_COLUMNS}')"],
}
DROP = ("b_id", "b_rnd", "b_grp_ts", "b_s", "b_u", "b_amount", "c_scan", "c_set", "c_point", "c_point_tid")
SIFT = ["SET sift.enable_col_scan = on"]
PLANS = {
    "btree": [("planner", []), ("forced index scan", ["SET enable_seqscan = off", "SET enable_bitmapscan = off"]),
              ("forced bitmap scan", ["SET enable_seqscan = off", "SET enable_indexscan = off", "SET enable_indexonlyscan = off"])],
    "sift": [("scan off (sift.enable_col_scan=off)", []), ("planner", SIFT),
             ("forced bitmap scan", SIFT + ["SET sift.enable_agg = off", "SET enable_seqscan = off", "SET enable_indexscan = off", "SET enable_indexonlyscan = off"]),
             ("forced index scan", SIFT + ["SET sift.enable_agg = off", "SET enable_seqscan = off", "SET enable_bitmapscan = off"])],
}
RUNS = 24
TSV = ("table_rows", "query_key", "index", "variant", "plan", "exec_median_ms", "planning_median_ms", "first_ms")


def psql(*sql):
    args = ["psql", "-X", "-qAt", "-v", "ON_ERROR_STOP=1"]
    for s in sql:
        args += ["-c", s]
    return subprocess.run(args + ["-d", sys.argv[2]], env=ENV, capture_output=True, text=True, check=True).stdout


def load(n):
    psql("SET statement_timeout = 0", "CREATE EXTENSION IF NOT EXISTS pg_sift", f"DROP TABLE IF EXISTS items_{n}", f"""CREATE TABLE items_{n} (
  id int NOT NULL, rnd int, grp int, ts timestamptz, ts_rnd timestamptz, u uuid, s_short text, s_long text, s_lc text, amount numeric(12,2), payload text)""",
         "SELECT setseed(0.42)", f"""INSERT INTO items_{n} SELECT g, (random()*{n}*10)::int, (random()*99)::int,
  timestamptz '2024-01-01' + g * interval '30 seconds', timestamptz '2024-01-01' + (random()*{n}*30) * interval '1 second',
  md5(g::text)::uuid, left(md5(g::text), 8), repeat(md5((g*7)::text), 7) || md5(g::text)::text,
  'cat_' || (random()*19)::int, (random()*100000)::numeric(12,2), repeat('x', 100) FROM generate_series(1, {n}) g""",
         f"VACUUM (ANALYZE, FREEZE) items_{n}")


def queries(n, t):
    ids = sorted(random.Random(7).sample(range(1, n + 1), 100))
    mid, narrow, wide, rmax = n // 2, max(1, n // 1000), n // 5, n * 10
    one = lambda col: psql(f"SELECT {col} FROM {t} WHERE id = {mid}").strip()
    rnd_v, u_v, s_v, ts_lo = one("rnd"), one("u"), one("s_short"), one("ts")
    ts_hi = f"'{ts_lo}'::timestamptz + interval '{narrow * 30 * 100} seconds'"
    return {
        "pt_id_rows": f"SELECT * FROM {t} WHERE id = {mid}",
        "pt_id_count": f"SELECT count(*) FROM {t} WHERE id = {mid}",
        "pt_rnd_rows": f"SELECT * FROM {t} WHERE rnd = {rnd_v}",
        "pt_rnd_cover": f"SELECT id, rnd FROM {t} WHERE rnd = {rnd_v}",
        "in10_rows": f"SELECT * FROM {t} WHERE id IN ({','.join(map(str, ids[:10]))})",
        "in100_rows": f"SELECT * FROM {t} WHERE id IN ({','.join(map(str, ids))})",
        "in100_count": f"SELECT count(*) FROM {t} WHERE id IN ({','.join(map(str, ids))})",
        "rng_narrow_rows": f"SELECT * FROM {t} WHERE id BETWEEN {mid} AND {mid + narrow}",
        "rng_narrow_count": f"SELECT count(*) FROM {t} WHERE id BETWEEN {mid} AND {mid + narrow}",
        "rng_wide_rows": f"SELECT * FROM {t} WHERE id BETWEEN {mid} AND {mid + wide}",
        "rng_wide_count": f"SELECT count(*) FROM {t} WHERE id BETWEEN {mid} AND {mid + wide}",
        "rng_rnd_narrow_rows": f"SELECT * FROM {t} WHERE rnd BETWEEN {rmax // 2} AND {rmax // 2 + rmax // 1000}",
        "rng_rnd_narrow_cover": f"SELECT id FROM {t} WHERE rnd BETWEEN {rmax // 2} AND {rmax // 2 + rmax // 1000}",
        "rng_rnd_wide_count": f"SELECT count(*) FROM {t} WHERE rnd BETWEEN {rmax // 2} AND {rmax // 2 + rmax // 5}",
        "ord10": f"SELECT id, rnd FROM {t} ORDER BY rnd LIMIT 10",
        "ord10_star": f"SELECT * FROM {t} ORDER BY rnd LIMIT 10",
        "ord1000": f"SELECT id, rnd FROM {t} ORDER BY rnd LIMIT 1000",
        "ord_desc10": f"SELECT id, rnd FROM {t} ORDER BY rnd DESC LIMIT 10",
        "ord_filter10": f"SELECT id, ts FROM {t} WHERE grp = 7 ORDER BY ts LIMIT 10",
        "mc_eq_rng_count": f"SELECT count(*) FROM {t} WHERE grp = 7 AND ts BETWEEN '{ts_lo}' AND {ts_hi}",
        "mc_eq_rng_rows": f"SELECT * FROM {t} WHERE grp = 7 AND ts BETWEEN '{ts_lo}' AND {ts_hi}",
        "mc_eq_only_count": f"SELECT count(*) FROM {t} WHERE grp = 7",
        "mc_eq_only_rows": f"SELECT * FROM {t} WHERE grp = 7",
        "mc_rng_only_rows": f"SELECT * FROM {t} WHERE ts BETWEEN '{ts_lo}' AND '{ts_lo}'::timestamptz + interval '{narrow * 30} seconds'",
        "minmax": f"SELECT min(rnd), max(rnd) FROM {t}",
        "txt_eq_rows": f"SELECT * FROM {t} WHERE s_short = '{s_v}'",
        "txt_prefix_count": f"SELECT count(*) FROM {t} WHERE s_short LIKE '{s_v[:3]}%'",
        "uuid_eq_rows": f"SELECT * FROM {t} WHERE u = '{u_v}'",
        "lim_grp10": f"SELECT * FROM {t} WHERE grp = 7 LIMIT 10",
        "lim_rng10": f"SELECT id, ts FROM {t} WHERE id > {mid} LIMIT 10",
        "txt_like_rows": f"SELECT * FROM {t} WHERE s_short LIKE '{s_v[:5]}%'",
        "txt_like_count": f"SELECT count(*) FROM {t} WHERE s_short LIKE '{s_v[:4]}%'",
        "is_null_count": f"SELECT count(*) FROM {t} WHERE rnd IS NULL",
        "distinct_grp": f"SELECT DISTINCT grp FROM {t}",
        "groupby_grp": f"SELECT grp, count(*), sum(amount) FROM {t} GROUP BY grp",
        "nl_join_100": f"SELECT count(*), sum(t.rnd) FROM (SELECT g AS id FROM generate_series({mid}, {mid + 99}) g) d JOIN {t} t ON t.id = d.id",
    }


def explain(sql, settings, runs=RUNS):
    """runs runs in one session: (first, median of the last three, their planning median, plan nodes)"""
    out = psql("SET statement_timeout = '60s'", *settings, *["EXPLAIN (ANALYZE, TIMING OFF, FORMAT JSON) " + sql] * runs)
    docs, at, dec = [], 0, json.JSONDecoder()
    while at < len(out):
        if out[at].isspace():
            at += 1
            continue
        doc, at = dec.raw_decode(out, at)
        docs.append(doc[0])

    def nodes(p):
        yield p
        for c in p.get("Plans", []):
            yield from nodes(c)

    names = []
    for p in nodes(docs[-1]["Plan"]):
        nt = p["Node Type"]
        if nt == "Custom Scan":
            nt = p.get("Custom Plan Provider", "Custom")
        if nt not in ("Limit", "Aggregate", "Hash", "Result", "Gather", "Materialize", "Function Scan"):
            names.append(nt.replace(" Scan", "").replace("Bitmap Heap", "BmpHeap").replace("Bitmap Index", "BmpIdx").replace(" ", ""))
    return (docs[0]["Execution Time"], statistics.median(d["Execution Time"] for d in docs[-3:]),
            statistics.median(d["Planning Time"] for d in docs[-3:]), "/".join(names[:3]))


def run(n, kind):
    only = [a[7:] for a in sys.argv if a.startswith("--only=")]
    only = only[0].split(",") if only else []
    tab = f"items_{n}"
    psql(*[f"DROP INDEX IF EXISTS {i}" for i in DROP])
    for d in INDEXES[kind]:
        psql(d.replace(" T ", f" {tab} "))
    psql(f"VACUUM (ANALYZE) {tab}")
    plans = PLANS["btree" if kind == "btree" else "sift"]
    print("\t".join(TSV))
    for key, sql in queries(n, tab).items():
        if only and not any(key.startswith(o) for o in only):
            continue
        for variant, settings in plans:
            try:
                first, med, plan_ms, plan = explain(sql, settings)
                first = statistics.median([first] + [explain(sql, settings, 1)[0] for _ in range(2)])
            except subprocess.CalledProcessError as e:
                print(f"{n}\t{key}\t{NAMES[kind]}\t{variant}\tERR\t\t\t{e.stderr.strip()[:60]}", flush=True)
                continue
            print(f"{n}\t{key}\t{NAMES[kind]}\t{variant}\t{plan}\t{med:.3f}\t{plan_ms:.3f}\t{first:.3f}", flush=True)


def space(n):
    tab = f"items_{n}"
    print("table_rows\tcolumns\tindex\tindex_bytes\tbytes_per_row")
    for kind, label, cols in (("btree", "btree (6 indexes)", "a btree on each of id, rnd, (grp, ts), s_short, u, amount"),
                              ("classes", "sift_col classes", "id, rnd, grp, ts, s_short, u, amount: one index"),
                              ("set", "sift_col classes + key_columns", f"the same with key_columns = '{SET_COLUMNS}'"),
                              ("point", "sift_col classes + point_columns", f"the same with point_columns = '{POINT_COLUMNS}'"),
                              ("point_tid", "sift_col classes + point_tid_columns", f"the same with point_tid_columns = '{POINT_COLUMNS}'")):
        psql(*[f"DROP INDEX IF EXISTS {i}" for i in DROP])
        for d in INDEXES[kind] + (["CREATE INDEX b_amount ON T (amount)"] if kind == "btree" else []):
            psql(d.replace(" T ", f" {tab} "))
        size = int(psql(f"SELECT sum(pg_relation_size(indexrelid)) FROM pg_index WHERE indrelid = '{tab}'::regclass AND NOT indisprimary").strip())
        print(f"{n}\t{cols}\t{label}\t{size}\t{size / n:.2f}")


cmd, n = sys.argv[1], int(sys.argv[3])
{"load": lambda: load(n), "run": lambda: run(n, sys.argv[4]), "space": lambda: space(n)}[cmd]()
