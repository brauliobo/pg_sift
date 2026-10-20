#!/usr/bin/env python3
"""`col @@> ANY (array)`: the scan of the OR (SiftScan, SiftCount, the score) against the bitmap scan, by array size and shape.

  bench/any_array.py sets  DB TABLE COL            band LO HI NAME ...    store the arrays (table arrsets) from the words of a document-frequency band
  bench/any_array.py run   DB TABLE COL RUNS [NAME,...] > any-array.tsv   time every array in every mode; PG* variables pick the server

`sets` takes the words of TABLE.COL whose document frequency lies in [LO, HI] (to_tsvector('simple'), 4-12 letters), orders them by a hash of the word
and the band's NAME (the same words every run), and stores the first 2, 5, 20 and 100 of them (fewer when the band has fewer). Each
`band LO HI NAME` is one flavour. `run` needs a server with pg_sift and the sets in DB; it prints one row per array, shape and mode.

Modes: bitmap (sift.enable_scan and enable_count off: the plan before the scan took the array), bitmap_serial (the same without parallel workers),
scan (the scan forced: bitmap and sequential scans off), scan_par (the same with sift.parallel_scan), default (the planner's own choice).
Shapes: count, list (all the matches), limit10 and limit1000 (the first rows, no order), top10 and top1000 (ORDER BY sift.score(ctid) DESC; the
bitmap has no score), and the OR written in the query text (`*_ortext`) for the same arrays. Each query runs RUNS + 1 times in one psql session
(the first is dropped) and the planning time of EXPLAIN is taken the same way. Timings are the client's wall clock (psql \\timing), the result sent to /dev/null.
"""
import os, re, statistics, subprocess, sys

env = dict(os.environ, LC_ALL='C')
SIZES = (2, 5, 20, 100)
MODES = {
    'bitmap': "SET sift.enable_scan=off; SET sift.enable_count=off; SET enable_seqscan=off;",
    'bitmap_serial': "SET sift.enable_scan=off; SET sift.enable_count=off; SET enable_seqscan=off; SET max_parallel_workers_per_gather=0;",
    'scan': "SET enable_bitmapscan=off; SET enable_seqscan=off;",
    'scan_par': "SET enable_bitmapscan=off; SET enable_seqscan=off; SET sift.parallel_scan=on;",
    'default': "",
}


def psql(db, sql):
    return subprocess.run(['psql', '-X', '-q', '-d', db, '-v', 'ON_ERROR_STOP=1'], input=sql, capture_output=True, text=True, env=env, check=True).stdout


def make_sets(db, table, col, bands):
    sql = ["DROP TABLE IF EXISTS arrsets;", "CREATE TABLE arrsets (flavour text, size int, arr text[]);",
           f"CREATE TEMP TABLE w AS SELECT word, ndoc FROM ts_stat($$SELECT to_tsvector('simple', {col}) FROM {table}$$) WHERE word ~ '^[a-z]{{4,12}}$';"]
    for lo, hi, name in bands:
        for size in SIZES:
            sql.append(f"INSERT INTO arrsets SELECT '{name}', {size}, array_agg(word ORDER BY h) FROM "
                       f"(SELECT word, hashtext(word || '{name}') h FROM w WHERE ndoc BETWEEN {lo} AND {hi} ORDER BY h LIMIT {size}) x;")
    psql(db, '\n'.join(sql))


def sets(db, flavours):
    out = psql(db, "\\pset tuples_only on\n\\pset format unaligned\nSELECT flavour || '|' || size || '|' || cardinality(arr) || '|' || arr::text FROM arrsets ORDER BY flavour, size;")
    for line in out.strip().split('\n'):
        f, s, n, a = line.split('|', 3)
        if not flavours or f in flavours:
            yield f, int(s), int(n), a


def shapes(table, col, arr):
    lit = "'" + arr.replace("'", "''") + "'::text[]"
    w = f"{col} @@> ANY ({lit})"
    wt = f"{col} @@> '" + ' OR '.join(arr.strip('{}').split(',')).replace("'", "''") + "'"
    every = ('bitmap', 'bitmap_serial', 'scan', 'scan_par', 'default')
    scored = ('scan', 'scan_par', 'default')
    return {
        'count': (f"SELECT count(*) FROM {table} WHERE {w}", every),
        'list': (f"SELECT id FROM {table} WHERE {w}", every),
        'limit10': (f"SELECT id FROM {table} WHERE {w} LIMIT 10", every),
        'limit1000': (f"SELECT id FROM {table} WHERE {w} LIMIT 1000", every),
        'top10': (f"SELECT id, sift.score(ctid) FROM {table} WHERE {w} ORDER BY 2 DESC, 1 LIMIT 10", scored),
        'top1000': (f"SELECT id, sift.score(ctid) FROM {table} WHERE {w} ORDER BY 2 DESC, 1 LIMIT 1000", scored),
        'top10_ortext': (f"SELECT id, sift.score(ctid) FROM {table} WHERE {wt} ORDER BY 2 DESC, 1 LIMIT 10", ('default',)),
        'count_ortext': (f"SELECT count(*) FROM {table} WHERE {wt}", ('default',)),
        'list_ortext': (f"SELECT id FROM {table} WHERE {wt}", ('default', 'bitmap')),
    }


def timed(db, mode, query, runs):
    out = psql(db, f"{MODES[mode]}\n\\o /dev/null\n\\timing on\n" + (query + ";\n") * (runs + 1))
    ts = [float(m) for m in re.findall(r'^Time: ([0-9.]+) ms', out, re.M)]
    assert len(ts) == runs + 1, (query, out[:300])
    return ts[1:]


def planning(db, mode, query, runs):
    out = psql(db, f"{MODES[mode]}\n\\pset format unaligned\n\\pset tuples_only on\n" + f"EXPLAIN (SUMMARY ON, COSTS OFF) {query};\n" * (runs + 1))
    ts = [float(m) for m in re.findall(r'^Planning Time: ([0-9.]+) ms', out, re.M)]
    assert len(ts) == runs + 1, out[:300]
    return ts[1:]


def plan_node(db, mode, query):
    out = psql(db, f"{MODES[mode]}\n\\pset format unaligned\n\\pset tuples_only on\nEXPLAIN (COSTS OFF) {query};\n")
    for pat, name in (('SiftCount', 'SiftCount'), ('SiftScan', 'SiftScan'), ('Bitmap Heap Scan', 'Bitmap'), ('Seq Scan', 'SeqScan')):
        if pat in out:
            return name
    return '?'


def run(db, table, col, runs, flavours):
    print('\t'.join(['db', 'flavour', 'size', 'elements', 'shape', 'mode', 'plan', 'exec_median_ms', 'exec_min_ms', 'exec_max_ms', 'plan_median_ms', 'plan_max_ms', 'rows']))
    for flavour, size, n, arr in sets(db, flavours):
        for shape, (q, modes) in shapes(table, col, arr).items():
            for mode in modes:
                ts = timed(db, mode, q, runs)
                ps = planning(db, mode, q, runs)
                count_sql = q if shape.startswith('count') else f"SELECT count(*) FROM ({q}) x"
                rows = psql(db, f"{MODES[mode]}\n\\pset tuples_only on\n{count_sql};\n").strip()
                print('\t'.join(map(str, [db, flavour, size, n, shape, mode, plan_node(db, mode, q), f'{statistics.median(ts):.3f}', f'{min(ts):.3f}', f'{max(ts):.3f}',
                                          f'{statistics.median(ps):.3f}', f'{max(ps):.3f}', rows])), flush=True)


if __name__ == '__main__':
    if len(sys.argv) > 5 and sys.argv[1] == 'sets' and sys.argv[5] == 'band':
        db, table, col = sys.argv[2:5]
        rest = sys.argv[5:]
        bands = [(int(rest[i + 1]), int(rest[i + 2]), rest[i + 3]) for i in range(0, len(rest), 4)]
        make_sets(db, table, col, bands)
    elif len(sys.argv) > 5 and sys.argv[1] == 'run':
        run(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]), sys.argv[6].split(',') if len(sys.argv) > 6 else None)
    else:
        sys.exit(__doc__)
