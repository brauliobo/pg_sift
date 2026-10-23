#!/usr/bin/env python3
"""Focused scaling suite: time against size at 4-5 points on small tables, the log-log slope asserted against a target.

  bench/scaling/scaling.py list
  bench/scaling/scaling.py run [--only units,groups,...] [--quick] [--sizes units=64,128,256] [--reps 5] [--out FILE] [--keep]

Uses the cluster of PGHOST/PGPORT/PGDATABASE (any with pg_sift installed); every case lives in the schema scal_<case>, dropped at the end.
Per size and engine: one warm-up and --reps runs of EXPLAIN (ANALYZE, TIMING OFF), median Planning + Execution Time.
Engines: sift (btrees absent; sequential scans off so that the sift plan is the one measured) and native (sift GUCs off; only for cases with native=True).
One CSV line per point, then one SLOPE line per (case, query, engine); exit status 1 if a sift line is FAIL."""
import argparse, importlib.util, glob, os, re, subprocess, sys, time

H = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, H)
from model import Case, Query, verdict          # noqa: E402

ENGINES = {
    'sift': ['enable_seqscan=off', 'sift.enable_col_scan=on'],             # col_scan: the index scan of sift_col (point lookups, IN lists, ORDER BY .. LIMIT)
    'native': ['sift.enable_agg=off', 'sift.enable_topn=off', 'sift.enable_scan=off', 'sift.enable_count=off', 'sift.enable_col_scan=off'],
}
COMMON = ["work_mem='4GB'", 'max_parallel_workers_per_gather=0', "statement_timeout=0"]      # large work_mem; one worker so that time is work


def psql(sql, check=True):
    r = subprocess.run(['psql', '-X', '-q', '-At', '-v', f'ON_ERROR_STOP={int(check)}'], input=sql, capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f'psql: {r.stderr[:1500]}')
    return r.stdout + (r.stderr if not check else '')


def drop_schema(name):
    """one transaction per table: a single DROP SCHEMA over thousands of partitions runs out of lock slots (max_locks_per_transaction)"""
    psql(f"select format('drop table %s', c.oid::regclass) from pg_class c join pg_namespace n on n.oid = c.relnamespace where n.nspname = 'scal_{name}' "
         f"and c.relkind in ('r', 'p') order by c.relispartition desc \\gexec\ndrop schema if exists scal_{name} cascade;")


def load_cases():
    cases = {}
    for f in sorted(glob.glob(os.path.join(H, 'cases', '*.py'))):
        spec = importlib.util.spec_from_file_location('cases_' + os.path.basename(f)[:-3], f)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        for c in mod.CASES:
            cases[c.name] = c
    return cases


def med(x):
    x = sorted(x)
    return x[len(x) // 2]


def explain(sql, sets, reps):
    pre = ''.join(f'set {s};\n' for s in COMMON + sets)
    body = ''.join(f"explain (analyze, timing off, summary on, costs off) {sql};\nselect '#END';\n" for _ in range(reps + 1))
    out = psql(pre + body, check=False).split('#END')[:-1]
    if len(out) != reps + 1 or any('Execution Time' not in o for o in out):
        return None, '\n'.join(out)[-600:]
    f = lambda c, k: float(re.search(k + r': ([0-9.]+) ms', c).group(1))
    return ([f(c, 'Planning Time') for c in out[1:]], [f(c, 'Execution Time') for c in out[1:]], out[-1]), None


def node(plan):
    if 'SiftAgg' in plan:
        return 'SiftAgg'
    m = re.search(r'(?:Index(?: Only)? Scan|Bitmap Heap Scan|Seq Scan|Custom Scan \((\w+)\))[^\n]*', plan)
    return (m.group(1) or m.group(0).split(' on ')[0].split(' using ')[0]).replace(' ', '_') if m else 'other'


def run(args):
    cases = load_cases()
    only = args.only.split(',') if args.only else list(cases)
    sizes_override = {k: [int(v) for v in s.split(',')] for k, s in (a.split('=') for a in args.sizes)}
    rows, failed = {}, False
    print('case,query,engine,size,plan_ms,exec_ms,units,pruned,node', flush=True)
    for name in only:
        try:
            failed |= run_case(name, cases[name], args, rows, sizes_override)
        except RuntimeError as e:
            print(f'{name},,,,,,,,ERROR', flush=True)
            print(f'# {name}: {e}', file=sys.stderr)
            failed = True
            try:
                drop_schema(name)
            except RuntimeError:
                pass
    summary(rows, failed)


nosift = set()


def run_case(name, c, args, rows, sizes_override):
    failed = False
    sizes = sizes_override.get(name) or (c.sizes[:c.quick] if args.quick else c.sizes)
    for size in sizes:
        if size == sizes[0] or c.per_size:
            t0 = time.time()
            drop_schema(name)
            psql(f'create schema scal_{name};')
            psql(f'set search_path = scal_{name}, public; {c.setup(size if c.per_size else max(sizes))}')
            print(f'# {name}: built for {size if c.per_size else max(sizes)} {c.unit} in {time.time() - t0:.0f}s', file=sys.stderr, flush=True)
        psql(f'set search_path = scal_{name}, public; {c.at(size)}')
        if c.check and size == sizes[0]:
            res = {e: psql('\n'.join(f'set {x};' for x in COMMON + [f'search_path = scal_{name}, public'] + ENGINES[e]) + f'\n{c.check(size)}', check=False).strip() for e in ENGINES}
            same = len(set(res.values())) == 1 and 'ERR' not in res['sift']
            print(f'CHECK {name}@{size} {"ok" if same else "MISMATCH " + str(res)}', flush=True)
            failed |= not same
        for q in c.queries:
            for eng in ENGINES if c.native else ['sift']:
                r, err = explain(q.sql(size), [f'search_path = scal_{name}, public'] + ENGINES[eng], args.reps)
                if r is None:
                    print(f'{name},{q.name},{eng},{size},,,,,ERROR', flush=True)
                    print(f'# {name}/{q.name}/{eng}@{size}: {err}', file=sys.stderr)
                    if eng == 'sift' and q.target:
                        failed = True
                    continue
                pl, ex, plan = r
                g = lambda k: m.group(1) if (m := re.search(k + r': (\d+)', plan)) else ''
                p, e = med(pl), med(ex)
                if eng == 'sift' and q.needs_sift and 'sift' not in plan.lower() and 'SiftAgg' not in plan:
                    print(f'# {name}/{q.name}@{size}: the plan does not use sift ({node(plan)}): the time below is not the index', file=sys.stderr)
                    nosift.add((name, q.name))
                    failed = True
                print(f'{name},{q.name},{eng},{size},{p:.3f},{e:.3f},{g("Units")},{g("Units Pruned")},{node(plan)}', flush=True)
                rows.setdefault((name, q.name, eng, q.target), []).append((size, p + e))
    if not args.keep:
        drop_schema(name)
    return failed


def summary(rows, failed):
    for (name, qn, eng, target), pts in rows.items():
        if len(pts) < 2:
            continue
        slope, last, v = verdict(pts, target)
        v = 'NOSIFT' if eng == 'sift' and (name, qn) in nosift else v
        extra = pts[-1][1] * (64 ** max(last, 0))
        print(f'SLOPE {name}/{qn} {eng} slope={slope:.2f} last={last:.2f} target={target or "-"} {v} x64={extra:.1f}ms', flush=True)
        failed |= eng == 'sift' and v == 'FAIL'
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('list')
    r = sub.add_parser('run')
    r.add_argument('--only'); r.add_argument('--quick', action='store_true'); r.add_argument('--keep', action='store_true')
    r.add_argument('--sizes', action='append', default=[]); r.add_argument('--reps', type=int, default=5)
    a = ap.parse_args()
    if a.cmd == 'list':
        for n, c in load_cases().items():
            print(n, c.sizes, [q.name for q in c.queries])
    else:
        run(a)
