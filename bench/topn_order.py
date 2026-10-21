#!/usr/bin/env python3
"""ORDER BY key LIMIT n over a sift_col index: the top-N scan (SiftAgg) against the sort plan, by how the key lies in the table.

  bench/topn_order.py load DB ROWS         create table items (ROWS rows, multiple of 12000) and its sift_col index
  bench/topn_order.py run  DB RUNS > topn-order.tsv   time every query in both plans; PG* variables pick the server

Keys: k_clu (follows the table), k_band12k / k_band3k / k_band300 (blocks of that many consecutive rows, each block a band of values, the
blocks in a scrambled order: the zones of a unit cover a few bands, many bands, nearly the whole range), k_rand (no order), k_mix
(500 values, one row in 11 NULL: ties and NULLs). Every query ends with the tie-break `id`, so both plans return the same rows.
Plans: topn (the planner's own choice with the sort plan priced out: sift.enable_agg on) and sort (sift.enable_agg off, no index scan:
the rows are read and sorted). Each query runs RUNS + 1 times in one session (the first is dropped); the time is the Execution Time of
EXPLAIN (ANALYZE, TIMING OFF), the units and superblocks are those it reports. Single backend, no parallel workers, warm cache.
"""
import os, re, statistics, subprocess, sys

env = dict(os.environ, LC_ALL='C')
KEYS = ('k_clu', 'k_band12k', 'k_band3k', 'k_band300', 'k_rand', 'k_mix')
LIMITS = (10, 1000)
FILTERS = (('none', ''), ('kind', "WHERE kind = 'cat_b'"))
PLANS = {
    'topn': "SET sift.enable_agg = on; SET enable_seqscan = off; SET enable_indexscan = off; SET enable_bitmapscan = off;",
    'sort': "SET sift.enable_agg = off;",
}


def psql(db, sql):
    return subprocess.run(['psql', '-X', '-q', '-d', db, '-v', 'ON_ERROR_STOP=1', '-At'], input=sql, capture_output=True, text=True, env=env, check=True).stdout


def band(rows, block, name):
    nb = rows // block
    return (f"(((i / {block}) * 7919 + 13) % {nb}) * (1000000 / {nb}) + (hashint4(i + {block}) & 2147483647) % (1000000 / {nb}) AS {name}")


def load(db, rows):
    assert rows % 12000 == 0
    psql(db, f"""
CREATE EXTENSION IF NOT EXISTS pg_sift;
DROP TABLE IF EXISTS items;
CREATE TABLE items (id serial PRIMARY KEY, kind text, k_clu int, k_band12k int, k_band3k int, k_band300 int, k_rand int, k_mix int, val float8, pad text DEFAULT repeat('x', 100));
INSERT INTO items (kind, k_clu, k_band12k, k_band3k, k_band300, k_rand, k_mix, val)
SELECT (ARRAY['cat_a','cat_b','cat_c','cat_d'])[1 + i % 4], i, {band(rows, 12000, 'a')}, {band(rows, 3000, 'b')}, {band(rows, 300, 'c')},
       (hashint4(i + 1) & 2147483647) % 1000000, CASE WHEN i % 11 = 0 THEN NULL ELSE (hashint4(i + 2) & 2147483647) % 500 END, (hashint4(i + 3) & 2147483647) / 7.0
FROM generate_series(1, {rows}) i;
CREATE INDEX items_col ON items USING sift_col (id, kind, k_clu, k_band12k, k_band3k, k_band300, k_rand, k_mix, val);
VACUUM (ANALYZE) items;""")


def run(db, runs):
    print('\t'.join(('table_rows', 'key', 'order', 'limit', 'filter', 'plan', 'exec_median_ms', 'exec_min_ms', 'exec_max_ms', 'units', 'skipped_sbs', 'rows_scanned')))
    rows = int(psql(db, "SELECT count(*) FROM items").strip())
    for key in KEYS:
        for order in ('ASC', 'DESC'):
            for limit in LIMITS:
                for fname, where in FILTERS:
                    q = f"SELECT id, {key} FROM items {where} ORDER BY {key} {order}, id LIMIT {limit}"
                    for plan, setup in PLANS.items():
                        script = setup + '\n' + '\n'.join([f"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, BUFFERS OFF) {q};"] * (runs + 1))
                        out = psql(db, "SET max_parallel_workers_per_gather = 0;\n" + script)
                        times = [float(m) for m in re.findall(r'Execution Time: ([0-9.]+) ms', out)][1:]
                        units = re.findall(r'^ *Units: (\d+)', out, re.M)
                        skipped = re.findall(r'Superblocks Skipped By Top-N: (\d+)', out)
                        scanned = re.findall(r'Rows Scanned: (\d+)', out)
                        if plan == 'topn' and 'Top-N:' not in out:
                            plan = 'topn_not_used'
                        print('\t'.join((str(rows), key, order, str(limit), fname, plan, f'{statistics.median(times):.3f}', f'{min(times):.3f}', f'{max(times):.3f}',
                                         units[-1] if units else '', skipped[-1] if skipped else '', scanned[-1] if scanned else '')), flush=True)


if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == 'load':
        load(sys.argv[2], int(sys.argv[3]))
    elif len(sys.argv) >= 4 and sys.argv[1] == 'run':
        run(sys.argv[2], int(sys.argv[3]))
    else:
        print(__doc__)
        sys.exit(1)
