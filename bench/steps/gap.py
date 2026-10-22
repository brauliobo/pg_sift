#!/usr/bin/env python3
"""Where pg_sift's time goes on the ten store questions (bench/steps/gap.sh): planning, fixed cost, instructions and the top symbols of one backend.

Runs against a private cluster (PGHOST / PGPORT of scripts/pgtest.sh, user postgres) that gap.sh builds and starts under a memory cap. stdlib only.
  load    --rows N        the `orders` table with the sift_col and sift indexes, and the plain PostgreSQL layout in schema `plain`
  profile --rows N -o F   per question: EXPLAIN ANALYZE (planning, execution, SiftAgg counters), the fixed cost, parse + plan with and without the
                          planner hook, perf stat (instructions, cycles) and perf record (top symbols) of one backend
"""
import argparse
import json
import os
import re
import signal
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import store_lib  # noqa: E402  (the queries and their statements)
import store_render  # noqa: E402  (the tables and the stored results)

DB = 'gap'
FLOOR = "SELECT country, count(*) FROM orders WHERE created_at > '2100-01-01' GROUP BY country"   # every unit pruned by its zone map: the cost of a SiftAgg that reads nothing
HOOK = {'plain, no sift library': ('plain', ''), 'plain table, hook loaded': ('plain', '-c session_preload_libraries=pg_sift'),
        'sift table, sift.enable_agg=off': ('public', '-c sift.enable_agg=off'), 'sift table, sift.enable_agg=on': ('public', '')}


def log(msg):
    print(f'[{time.strftime("%H:%M:%S")}] {msg}', flush=True)


def psql(sql, db=DB, options='', args=()):
    env = dict(os.environ, PGOPTIONS=options)
    r = subprocess.run(['psql', '-X', '-q', '-At', '-v', 'ON_ERROR_STOP=1', '-U', 'postgres', '-d', db, *args, '-c', sql], capture_output=True, text=True, env=env)
    if r.returncode:
        raise RuntimeError(f'psql: {r.stderr.strip()[:400]}')
    return r.stdout.strip()


def psql_file(text, db=DB, args=()):
    r = subprocess.run(['psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', '-U', 'postgres', '-d', db, *args, '-f', '-'], input=text, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f'psql: {r.stderr.strip()[:400]}')


def cmd_load(args):
    sec = {n: store_lib.sections(store_lib.read(f'{n}.sql')) for n in ('pg', 'sift')}
    psql(f'DROP DATABASE IF EXISTS {DB} WITH (FORCE)', 'postgres')
    psql(f'CREATE DATABASE {DB}', 'postgres')
    psql_file(store_lib.read('gen.sql'), args=['-v', f'n={args.rows}'])
    psql_file('CREATE SCHEMA plain; SET search_path = plain;\n' + sec['pg']['table'] + '\nINSERT INTO plain.orders SELECT * FROM public.orders_src;\n' + sec['pg']['indexes'])
    psql_file(sec['sift']['table'] + '\nINSERT INTO public.orders SELECT * FROM public.orders_src;\n' + sec['sift']['indexes'])
    psql_file('DROP TABLE orders_src; VACUUM (FREEZE, ANALYZE) public.orders; VACUUM (FREEZE, ANALYZE) plain.orders; CHECKPOINT')
    log(f'{args.rows:,} orders: ' + psql("SELECT pg_size_pretty(pg_total_relation_size('public.orders')) || ' with sift, ' || pg_size_pretty(pg_total_relation_size('plain.orders')) || ' plain'"))
    return 0


def walk(plan):
    yield plan
    for p in plan.get('Plans', []):
        yield from walk(p)


def explain(sql, options='', reps=7):
    """EXPLAIN (ANALYZE) in one backend: the planning time of its first statement (empty caches), then the medians of reps more runs (planning and execution, ms)
    and the counters of the last run's SiftAgg node"""
    s = Session(options)
    try:
        runs = [json.loads(''.join(s.run(f'EXPLAIN (ANALYZE, TIMING, SUMMARY, FORMAT JSON) {sql}', capture=True)))[0] for _ in range(reps + 2)]
        plan = s.run(f'EXPLAIN {sql}', capture=True)
    finally:
        s.close()
    first, last = runs[0], runs[-1]
    agg = next((n for n in walk(last['Plan']) if n.get('Custom Plan Provider') == 'SiftAgg'), None)
    keys = ('Workers Planned', 'Workers Launched', 'Units', 'Units Pruned', 'Rows Scanned', 'Tuples Aggregated', 'Rows Vetted By Heap', 'Groups', 'Heap Pages')
    return dict(planning_first_ms=first['Planning Time'], planning_ms=statistics.median(r['Planning Time'] for r in runs[2:]),   # the second run warms the plan caches
                execution_ms=statistics.median(r['Execution Time'] for r in runs[2:]), top=last['Plan']['Node Type'],
                siftagg={k: agg[k] for k in keys if k in agg} if agg else None, siftagg_ms=agg and agg.get('Actual Total Time'), plan=plan)


def plan_latency(sql, schema, options, reps):
    """parse + plan + EXPLAIN text, in ms: pgbench runs `EXPLAIN (COSTS OFF)` reps times on one connection"""
    with tempfile.NamedTemporaryFile('w', suffix='.sql') as f:
        f.write(f'EXPLAIN (COSTS OFF, SUMMARY OFF) {sql};\n')
        f.flush()
        env = dict(os.environ, PGOPTIONS=f'-c search_path={schema} {options}', PGDATABASE=DB, PGUSER='postgres')
        r = subprocess.run(['pgbench', '-n', '-M', 'simple', '-c', '1', '-t', str(reps), '-f', f.name], capture_output=True, text=True, env=env)
    m = re.search(r'latency average = ([\d.]+) ms', r.stdout)
    if not m:
        raise RuntimeError('pgbench: ' + (r.stderr or r.stdout)[-300:])
    return float(m[1])


def hook_cost(q, n, reps=400):
    out = {}
    for label, (schema, opt) in HOOK.items():
        sql = q.statement('pg' if schema == 'plain' else 'sift', n)
        out[label] = round(min(plan_latency(sql, schema, opt, reps) for _ in range(3)), 3)   # the best of three runs: the least disturbed
    return out


class Session:
    """one psql backend, so that perf can attach to its pid"""
    def __init__(self, options=''):
        self.p = subprocess.Popen(['psql', '-X', '-q', '-At', '-U', 'postgres', '-d', DB], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1,
                                  env=dict(os.environ, PGOPTIONS=options))
        self.pid = int(self.run('SELECT pg_backend_pid()')[0])
        self.run('\\o /dev/null')

    def run(self, sql, capture=False):
        self.p.stdin.write(('\\o\n' if capture else '') + sql + ('' if sql.startswith('\\') else ';') + '\n' + ('\\o /dev/null\n' if capture else '') + '\\echo __DONE__\n')
        self.p.stdin.flush()
        out = []
        while (line := self.p.stdout.readline().rstrip('\n')) != '__DONE__':
            if line == '' and self.p.poll() is not None:
                raise RuntimeError('psql ended')
            out.append(line)
        return out

    def close(self):
        self.p.stdin.close()
        self.p.wait()


def perf_profile(sql, runs, top=14):
    """one backend, no workers, runs times: instructions and cycles (perf stat), the top symbols (perf record)"""
    s = Session('-c max_parallel_workers_per_gather=0')
    try:
        for _ in range(3):
            s.run(sql)
        with tempfile.TemporaryDirectory() as d:
            stat, rec = os.path.join(d, 'stat'), os.path.join(d, 'perf.data')
            ps = subprocess.Popen(['perf', 'stat', '-x,', '-e', 'instructions:u,cycles:u,task-clock', '-p', str(s.pid), '-o', stat], stderr=subprocess.DEVNULL)
            pr = subprocess.Popen(['perf', 'record', '-q', '-e', 'cycles:u', '-F', '4999', '-p', str(s.pid), '-o', rec], stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
            time.sleep(0.5)
            t0 = time.perf_counter()
            for _ in range(runs):
                s.run(sql)
            wall = (time.perf_counter() - t0) * 1e3 / runs
            for p in (ps, pr):
                p.send_signal(signal.SIGINT)
                p.wait()
            counts = {}
            for line in open(stat):
                f = line.split(',')
                if len(f) > 3 and f[0].replace('.', '').isdigit():
                    counts[f[2].split(':')[0]] = float(f[0])
            rep = subprocess.run(['perf', 'report', '-i', rec, '--stdio', '--no-children', '--sort', 'dso,sym', '-n'], capture_output=True, text=True).stdout
    finally:
        s.close()
    syms = [(float(m[1]), m[2].strip(), m[3].strip()) for l in rep.splitlines() if (m := re.match(r'\s*([\d.]+)%\s+\d+\s+(\S+)\s+\[\.\]\s+(.*)', l))]
    return dict(wall_ms=round(wall, 2), instructions=counts.get('instructions', 0) / runs, cycles=counts.get('cycles', 0) / runs, task_ms=counts.get('task-clock', 0) / runs,
                top=[dict(pct=p, dso=d, symbol=y) for p, d, y in syms[:top]])


def cmd_profile(args):
    n = args.rows
    out = dict(rows=n, settings=psql("SELECT string_agg(name || '=' || setting || coalesce(unit, ''), ' ') FROM pg_settings WHERE name IN "
                                     "('shared_buffers', 'work_mem', 'max_parallel_workers_per_gather', 'jit', 'huge_pages')"),
               version=psql('SHOW server_version'), sift=psql("SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'"), queries={})
    try:
        psql('SHOW sift.enable_agg', options='-c search_path=plain')
        raise RuntimeError('the sift library is loaded in a fresh session: the baseline of the hook measurement is not clean')
    except RuntimeError as x:
        if 'unrecognized configuration parameter' not in str(x):
            raise
    out['floor'] = explain(FLOOR)
    out['floor_no_workers'] = explain(FLOOR, '-c max_parallel_workers_per_gather=0')
    log(f'floor: {out["floor"]["execution_ms"]:.2f} ms with workers, {out["floor_no_workers"]["execution_ms"]:.2f} ms without')
    for q in store_lib.load_queries(args.queries):
        sql = q.statement('sift', n)
        r = dict(title=q.title, explain=explain(sql), explain_no_workers=explain(sql, '-c max_parallel_workers_per_gather=0'), planning_by_hook=hook_cost(q, n),
                 perf=perf_profile(sql, args.perf_runs))
        out['queries'][q.id] = r
        e = r['explain']
        log(f'{q.id} {q.name}: plan {e["planning_ms"]:.2f} ms, execute {e["execution_ms"]:.2f} ms ({r["explain_no_workers"]["execution_ms"]:.2f} without workers), '
            f'{r["perf"]["instructions"] / 1e6:.1f} M instructions, hook: {r["planning_by_hook"]}')
    with open(args.o, 'w') as f:
        json.dump(out, f, indent=1)
        f.write('\n')
    log(f'written: {args.o}')
    return 0


# --- docs/BENCH-GAP.md: the generated blocks

def human(v):
    for unit, div in (('GiB', 2**30), ('MiB', 2**20), ('KiB', 2**10)):
        if v >= div:
            return f'{v / div:.1f} {unit}'
    return f'{v:.0f} B'


def top_symbols(perf, k=3):
    return '<br>'.join(f'`{t["symbol"]}` {t["pct"]:.0f}%' for t in perf['top'][:k])


def planning_block(P, R, tier):
    head = ['q', 'planning, warm backend', 'planning, first statement of a backend', 'parse + plan, no sift library', 'parse + plan, hook loaded (plain table)',
            'parse + plan, `sift.enable_agg = off`', 'parse + plan, SiftAgg planned', 'planning / `sift_col` median']
    body = []
    for q, r in P['queries'].items():
        h = r['planning_by_hook']
        med = R.num(tier, 'sift', q, 'warm', 'latency_ms')
        body.append([q, f'{r["explain"]["planning_ms"]:.2f}', f'{r["explain"]["planning_first_ms"]:.2f}', *(f'{h[k]:.3f}' for k in HOOK), f'{r["explain"]["planning_ms"] / med:.1%}' if med else 'n/a'])
    return [f'#### {P["rows"]:,} orders (ms)', ''] + store_render.md_table(head, body) + ['']


def profile_block(P, R, tier, S):
    head = ['q', '`sift_col` median (bench)', 'EXPLAIN execution', 'without workers', 'workers launched', 'units (pruned)', 'rows scanned', 'groups', 'instructions per row',
            'cycles per row', 'top symbols (one backend, no workers)']
    body = []
    for q, r in P['queries'].items():
        a, e = r['explain']['siftagg'] or {}, r['explain']
        rows = a.get('Rows Scanned') or P['rows']
        body.append([q, store_render.ms(R.num(tier, 'sift', q, 'warm', 'latency_ms')), f'{e["execution_ms"]:.2f}', f'{r["explain_no_workers"]["execution_ms"]:.2f}', str(a.get('Workers Launched', 'n/a')),
                     f'{a.get("Units", 0)} ({a.get("Units Pruned", 0)})' if a else 'n/a', f'{a["Rows Scanned"]:,}' if a else 'n/a', f'{a["Groups"]:,}' if a else 'n/a',
                     f'{r["perf"]["instructions"] / rows:.1f}', f'{r["perf"]["cycles"] / rows:.1f}', top_symbols(r['perf'])])
    f, f0 = P['floor'], P['floor_no_workers']
    out = [f'#### {P["rows"]:,} orders (ms)', '', f'A `SiftAgg` that reads nothing (every unit pruned by its zone map) takes {f["execution_ms"]:.2f} ms with {f["siftagg"].get("Workers Launched", 0)} workers launched and '
           f'{f0["execution_ms"]:.2f} ms without: that difference is the cost of starting the workers, and the second number is the executor start-up and finalization.', ''] + store_render.md_table(head, body, ['---:'] * 10 + ['---']) + ['']
    if S:
        head = ['q', 'ClickHouse rows read', 'bytes read', 'memory', 'peak threads', 'duration (query_log)', 'clickhouse-client --time', 'Elasticsearch `took`']
        body = [[q, f'{c["median"]["read_rows"]:,.0f}', human(c['median']['read_bytes']), human(c['median']['memory_usage']), f'{c["median"]["peak_threads_usage"]:.0f}',
                 f'{c["median"]["query_duration_ms"]:.0f} ms', f'{c["client_time_s"] * 1e3:.0f} ms', f'{S["es"][q]["took_ms"]:.0f} ms'] for q, c in S['ch'].items()]
        out += ['What ClickHouse and Elasticsearch spend on the same questions (their own accounting):', ''] + store_render.md_table(head, body) + ['']
    return out


def cmd_render(args):
    import readme
    R = store_render.Results(args.store)
    tiers = {R.meta['tiers'][t]['rows']: t for t in R.tiers}
    profiles = [P for P in (json.load(open(p)) for p in args.profile) if P['rows'] in tiers]   # a profile of a size the store results no longer hold is history, not rendered
    stats = {S['rows']: S for S in (json.load(open(p)) for p in args.stats)}
    readme.replace_block(args.doc, 'gap-table', store_render.gap_table(R))
    readme.replace_block(args.doc, 'gap-planning', sum((planning_block(P, R, tiers[P['rows']]) for P in profiles), []))
    readme.replace_block(args.doc, 'gap-profile', sum((profile_block(P, R, tiers[P['rows']], stats.get(P['rows'])) for P in profiles), []))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('load', 'profile'):
        p = sub.add_parser(name)
        p.add_argument('--rows', type=int, required=True)
    p = sub.choices['profile']
    p.add_argument('-o', required=True)
    p.add_argument('--queries', type=lambda s: s.split(','), default=[])
    p.add_argument('--perf-runs', type=int, default=20)
    p = sub.add_parser('render')
    p.add_argument('--store', default=os.path.join(HERE, '..', '..', 'docs', 'results', 'store.json'))
    p.add_argument('--profile', nargs='+', required=True)
    p.add_argument('--stats', nargs='*', default=[])
    p.add_argument('--doc', default=os.path.join(HERE, '..', '..', 'docs', 'BENCH-GAP.md'))
    args = ap.parse_args()
    try:
        sys.exit({'load': cmd_load, 'profile': cmd_profile, 'render': cmd_render}[args.cmd](args))
    except RuntimeError as x:
        print(f'gap: {x}', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
