#!/usr/bin/env python3
"""The store benchmark: ten questions about an online store's orders on PostgreSQL, PostgreSQL with pg_sift, Citus columnar, Elasticsearch and ClickHouse.
Called by steps/store.sh, which starts the containers (one per server, equal CPU and memory limits). stdlib only; `python3 -I`.

    store.py gen   --rows N --data DIR            the seeded CSV every system loads (bench/sql/store/gen.sql)
    store.py load  --rows N --data DIR --out DIR  load it into every engine: load time, size
    store.py run   --rows N --out DIR             warm latency of the ten queries on every engine, result equality, plans
    store.py cold  --rows N --out DIR             latency of the first run after a restart of the server
    store.py updates --rows N --out DIR           updates, deletes, read-after-write, reads under an update load, space (changes the data: runs last)
    store.py merge OUT... -o docs/results/store.json
    store.py render [--results FILE] [--readme FILE] [--benchmarks FILE]    the Markdown tables, rewritten between the bench:store markers

Engines: pg (heap + btree + GIN), sift (heap + sift_col + sift), sift_table (column-format table + sift), es (Elasticsearch, request
cache off), es_cached (the same with the shard request cache on), ch (ClickHouse), citus (Citus on one node, the table in the
`columnar` access method: no UPDATE, DELETE or GIN, so it is not in the update tables). A latency is the wall time of one request from this
process to the server on 127.0.0.1 and back, one client, the response read to its end (psql / HTTP keep-alive connection). The first run of
a query on an engine is not timed: its result is compared with the result of PostgreSQL (the reference) and the run stops on a difference.
"""
import argparse, inspect, json, os, statistics, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import benchlib  # noqa: E402  (steps/benchlib.py: host fingerprint, cell cache, row replacement)
import store_common  # noqa: E402  (steps/store_common.py: LOAD_CODE and MEASURE_CODE are set here)
import store_engine  # noqa: E402  (steps/store_engine.py: UTIL is set here)
from store_common import PARTS, host_fp, make_cache, part_done, part_path  # noqa: E402
from store_data import build_indexes, bulk_docs, cmd_datakey, cmd_gen, cmd_load, copy_chunks, load_ch, load_es, load_pg  # noqa: E402
from store_engine import Client, Http, Utilization, restart  # noqa: E402
from store_lib import CH_AUTH, DEFAULT_ENGINES, UPDATE_ENGINES, container, die, load_queries, log  # noqa: E402
from store_plan import cmd_cache_save, cmd_merge, cmd_noisy, cmd_patch, cmd_plan, cmd_planget, cmd_stable  # noqa: E402
from store_render import cmd_compare, cmd_gap, cmd_render, cmd_table  # noqa: E402
from store_run import cmd_cold, cmd_interference, cmd_run, cold_cell, evict, one_container, run_query, time_cell, time_wave  # noqa: E402
from store_updates import Writer, churn, cmd_updates, cmd_verify, read_after_write, read_latency, update_engine, with_load  # noqa: E402


def cmd_stats(args):
    """what ClickHouse and Elasticsearch read and spend per question, from their own accounting (system.query_log, `took`) and clickhouse-client --time"""
    ch, es = Http('ch', CH_AUTH), Http('es_cached', {'Content-Type': 'application/json'})
    out = dict(rows=args.rows, ch={}, es={})
    for q in load_queries(args.queries):
        sql, tag = q.statement('ch', args.rows), f'gap-{q.id}'
        for _ in range(args.runs):
            ch.request('POST', f'/?database=store&default_format=TSV&log_comment={tag}', sql.encode())
        ch.request('POST', '/', b'SYSTEM FLUSH LOGS')
        cols = ("read_rows, read_bytes, memory_usage, query_duration_ms, peak_threads_usage, ProfileEvents['RealTimeMicroseconds'] AS real_us, "
                "ProfileEvents['UserTimeMicroseconds'] AS user_us, ProfileEvents['SelectedParts'] AS parts, ProfileEvents['SelectedMarks'] AS marks")
        rows = [json.loads(l) for l in ch.request('POST', f"/?default_format=JSONEachRow&output_format_json_quote_64bit_integers=0", (
            f"SELECT {cols} FROM system.query_log WHERE type = 'QueryFinish' AND log_comment = '{tag}' ORDER BY event_time_microseconds DESC LIMIT {args.runs}").encode()).decode().splitlines()]
        cli = subprocess.run(['docker', 'exec', container('ch'), 'clickhouse-client', '--user', 'bench', '--password', 'bench', '--database', 'store', '--time', '--format', 'Null', '--query', sql],
                             capture_output=True, text=True)
        if cli.returncode:
            raise RuntimeError('clickhouse-client: ' + cli.stderr.strip()[:300])
        out['ch'][q.id] = dict(median={k: statistics.median(r[k] for r in rows) for k in rows[0]}, client_time_s=float(cli.stderr.strip().splitlines()[-1]))
        method, path, body, _, _ = q.statement('es_cached', args.rows)
        if '/_doc/' in path:   # a real-time get reports no `took`: the same lookup as a search by id does
            method, path, body = 'POST', '/orders/_search?request_cache=false', json.dumps({'query': {'ids': {'values': [path.rsplit('/', 1)[1]]}}}).encode()
        took = [json.loads(es.request(method, path.replace('request_cache=true', 'request_cache=false'), body))['took'] for _ in range(args.runs)]
        out['es'][q.id] = dict(took_ms=statistics.median(took))
        log(f'{q.id} {q.name}: clickhouse {out["ch"][q.id]["median"]}; elasticsearch took {out["es"][q.id]["took_ms"]} ms')
    with open(args.o, 'w') as f:
        json.dump(out, f, indent=1)
        f.write('\n')
    log(f'written: {args.o}')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)

    def common(p):
        p.add_argument('--rows', type=int)
        p.add_argument('--tier', default='')
        p.add_argument('--cpus', type=int, default=int(os.environ.get('STORE_CPUS', 3)), help='the thread budget N of every engine and the CPU quota of the containers; the result set of a size is `<size>-c<N>`')
        p.add_argument('--data', default=os.environ.get('STORE_DATA', '/tmp/sift-store-data'))
        p.add_argument('--out', default='.')
        p.add_argument('--engines', type=lambda s: s.split(','))
        p.add_argument('--queries', type=lambda s: s.split(','), default=[])
        p.add_argument('--runs', type=int, default=7)
        p.add_argument('--warmups', type=int, default=3, help='untimed runs per query and engine before the timed ones (the first one is the checked result)')
        p.add_argument('--cold-runs', type=int, default=3, help='cold: runs of a question per server whose memory is emptied between them')
        p.add_argument('--restart-runs', type=int, help='cold: runs of a question per server that is restarted between them (Elasticsearch: a restart takes ~20 s); default --cold-runs')
        p.add_argument('--cold-check', type=lambda s: s.split(',') if s else [], default=[], help='cold: questions also timed the other way (emptied against restarted), to check that emptying the memory is as cold; '
                       'off unless named, and its result is kept in the cache once per version of the engine')
        p.add_argument('--load-seconds', type=int, default=int(os.environ.get('STORE_LOAD_SECONDS', 15)), help='updates: how long a query is timed under each update rate')
        p.add_argument('--probes', type=int, default=20, help='updates: single-row changes timed until a query sees them')
        p.add_argument('--rates', type=lambda s: [int(x) for x in s.split(',')], default=[0, 1000, 10000], help='updates: the update rates (rows/s) the query is timed under')
        p.add_argument('--compression', action='store_true', help='load: also the size of each system with its other compression settings')
        p.add_argument('--no-resume', action='store_true', help='measure a part again even when its file holds the key of this command (resume skips it)')
        p.add_argument('--keep-going', action='store_true', help='go on after a result that differs from PostgreSQL\'s (the exit status is still 3)')
        p.add_argument('--untimed', type=lambda s: s.split(',') if s else [], default=[], help='engines that only serve as the reference of the result checks: run, never recorded (the plan sets it)')
        p.add_argument('--max-rerun', type=int, default=6, help='most NOISY cells measured again at the end of a stage')
        p.add_argument('--parallel-engines', dest='parallel', type=int, default=1, help='run: containers timed at the same time, each on disjoint physical cores of its own (docker --cpuset-cpus; equal sets, '
                       'moved round the cores); 1: one engine at a time')
        p.add_argument('--busy-cores', type=int, default=2, help='the utilization report counts the seconds with fewer busy cores than this')
        p.add_argument('--keys', type=lambda s: [x for x in s.split(',') if x], default=[], help='load: container=key of the data volumes (datakey)')
        p.add_argument('--reuse', type=lambda s: [x for x in s.split(',') if x], default=[], help='load: containers whose volume already holds the data of that key')
        p.add_argument('--containers', type=lambda s: s.split(','), default=['pg', 'sift'], help='datakey: the containers to key')
        cache_args(p)

    def cache_args(p):
        p.add_argument('--cache-dir', default=os.environ.get('STORE_CACHE', '/tmp/sift-store-cache'), help='the cache of the cells of the servers that are not under test')
        p.add_argument('--no-cache', action='store_true', help='read nothing from the cache and write nothing to it')
        p.add_argument('--cache-max-age', type=int, default=30, help='days after which a cached cell is measured again')
    for name in ('gen', 'load', 'run', 'cold', 'updates', 'verify', 'plan', 'datakey', 'interference'):
        common(sub.add_parser(name))
    it = sub.choices['interference']
    it.add_argument('--target', default='sift', help='the engine whose requests are timed')
    it.add_argument('--concurrent', type=int, default=3, help='containers that loop the same question at the same time')
    it.add_argument('--samples', type=int, default=41)
    it.add_argument('--threshold', type=float, default=0.05, help='the largest change of a median that still counts as no interference')
    pl = sub.choices['plan']
    pl.add_argument('--parts', type=lambda s: s.split(','), default=['load', 'warm', 'cold', 'updates'], help='the parts of the pass: load, warm, cold, updates')
    pl.add_argument('--points', type=lambda s: [int(x) for x in s.split(',')], help='the thread budgets the questions are timed at')
    pl.add_argument('--update-engines', type=lambda s: s.split(','), default=UPDATE_ENGINES)
    pl.add_argument('--remeasure-all', action='store_true', help='measure every server again whatever the cache holds')
    pl.add_argument('--remeasure', type=lambda s: s.split(','), default=[], help='measure these servers again (pg, citus, es, ch)')
    pl.add_argument('--plan', help='where the plan is written (default OUT/plan.json)')
    pg = sub.add_parser('planget')
    pg.add_argument('plan')
    pg.add_argument('field', choices=['run', 'untimed', 'update', 'cached', 'measured'])
    cs = sub.add_parser('cache-save')
    cs.add_argument('--plan', required=True)
    cs.add_argument('--results', required=True)
    cache_args(cs)
    for name, doc in (('noisy', 'the cells of a part file that are still NOISY'), ('patch', 'replace in a part file the cells a second file measured again')):
        np = sub.add_parser(name, help=doc)
        np.add_argument('file')
        if name == 'patch':
            np.add_argument('fresh')
    st = sub.add_parser('stable', help='do two results files tell the same story?')
    st.add_argument('a')
    st.add_argument('b')
    st.add_argument('--tol', type=float, default=1.0, help='the ratios of pg_sift to each system may differ by a factor of 1 + TOL')
    st.add_argument('--sep', type=float, default=1.5, help='a pair is ordered when its ratio is beyond this in both files')
    m = sub.add_parser('merge')
    m.add_argument('files', nargs='*')
    m.add_argument('-o', required=True)
    m.add_argument('--restart-cold', help='results file whose cold cells restarted the server for every run: take its cold cells of --restart-engines')
    m.add_argument('--restart-engines', type=lambda s: s.split(','), default=[])
    m.add_argument('--plan', action='append', default=[], help='a plan of the pass (repeat per size): its cached groups are read from the cache and merged in (cells of another host are refused)')
    m.add_argument('--keep', action='append', default=[], help='an earlier results file: its sets that this pass did not measure are carried over (skipped with a WARN when it was measured on another host)')
    r = sub.add_parser('render')
    r.add_argument('--results', default=os.path.join(HERE, '..', '..', 'docs', 'results', 'store.json'))
    r.add_argument('--readme')
    r.add_argument('--benchmarks')
    r.add_argument('--compression', default=os.path.join(HERE, '..', '..', 'docs', 'results', 'store-compression.json'))
    r.add_argument('--stdout', action='store_true')
    for name in ('table', 'gap'):
        sub.add_parser(name).add_argument('file', nargs='?', default=os.path.join(HERE, '..', '..', 'docs', 'results', 'store.json'))
    t = sub.add_parser('stats')
    t.add_argument('--rows', type=int, required=True)
    t.add_argument('-o', required=True)
    t.add_argument('--runs', type=int, default=3)
    t.add_argument('--queries', type=lambda s: s.split(','), default=[])
    c = sub.add_parser('compare')
    c.add_argument('a')
    c.add_argument('b')
    args = ap.parse_args()
    store_common.LOAD_CODE = benchlib.key_of([inspect.getsource(f) for f in (load_pg, build_indexes, copy_chunks, load_ch, load_es, bulk_docs)], 12)
    store_common.MEASURE_CODE = benchlib.key_of([store_common.LOAD_CODE] + [inspect.getsource(f) for f in (run_query, time_cell, time_wave, one_container, cold_cell, evict, restart, update_engine, with_load, churn, read_latency,
                                                                                   read_after_write, Client.run, Writer.bulk, Writer.probe_set)], 12)
    if args.cmd in ('gen', 'load', 'run', 'cold', 'updates', 'verify', 'plan', 'datakey', 'interference') and not args.engines:
        args.engines = UPDATE_ENGINES if args.cmd == 'updates' else DEFAULT_ENGINES
    args.cell = 0
    if args.cmd in ('load', 'run', 'cold', 'updates', 'interference'):
        store_engine.UTIL = Utilization(args.busy_cores)
    if getattr(args, 'rows', None) and not getattr(args, 'tier', 'x'):
        args.tier = (f'{args.rows // 1000000}M' if args.rows % 1000000 == 0 else f'{args.rows // 1000}k') + f'-c{args.cpus}'
    if hasattr(args, 'restart_runs') and args.restart_runs is None:
        args.restart_runs = args.cold_runs
    if args.cmd == 'plan':
        args.plan = args.plan or os.path.join(args.out, 'plan.json')
        args.points = args.points or [args.cpus]
        args.update_engines = args.update_engines if 'updates' in args.parts else []
    if args.cmd in ('plan', 'merge', 'cache-save', 'cold'):
        args.fingerprint = host_fp()
    if args.cmd == 'cold':
        args.cache = make_cache(args)
    benchlib.resumable()
    if args.cmd in PARTS and not args.no_resume and part_done(args):
        log(f'{args.cmd}: {part_path(args, PARTS[args.cmd])} holds the key of this command: measured before, kept')
        sys.exit(0)
    try:
        sys.exit({'gen': cmd_gen, 'verify': cmd_verify, 'load': cmd_load, 'run': cmd_run, 'cold': cmd_cold, 'updates': cmd_updates, 'merge': cmd_merge, 'render': cmd_render, 'table': cmd_table,
                  'stats': cmd_stats, 'gap': cmd_gap, 'compare': cmd_compare, 'plan': cmd_plan, 'planget': cmd_planget, 'cache-save': cmd_cache_save, 'noisy': cmd_noisy, 'patch': cmd_patch,
                  'stable': cmd_stable, 'datakey': cmd_datakey, 'interference': cmd_interference}[args.cmd](args))
    except (RuntimeError, benchlib.ForeignHost) as x:
        die(str(x))


if __name__ == '__main__':
    main()
