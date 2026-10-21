#!/usr/bin/env python3
"""The store benchmark: ten questions about an online store's orders on PostgreSQL, PostgreSQL with pg_sift, Citus columnar, Elasticsearch and ClickHouse.
Called by steps/store.sh, which starts the containers (one per server, equal CPU and memory limits). stdlib only; `python3 -I`.

    store.py gen   --rows N --data DIR            the seeded CSV every system loads (bench/sql/store/gen.sql)
    store.py load  --rows N --data DIR --out DIR  load it into every engine: load time, size
    store.py run   --rows N --out DIR             warm latency of the ten queries on every engine, result equality, plans
    store.py cold  --rows N --out DIR             latency of the first run after a restart of the server
    store.py updates --rows N --out DIR           updates, deletes, read-after-write, reads under an update load, space (changes the data: runs last)
    store.py merge OUT... -o bench/results/store.json
    store.py render [--results FILE] [--readme FILE] [--benchmarks FILE]    the Markdown tables, rewritten between the bench:store markers

Engines: pg (heap + btree + GIN), sift (heap + sift_col + sift), sift_table (column-format table + sift), es (Elasticsearch, request
cache off), es_cached (the same with the shard request cache on), ch (ClickHouse), citus (Citus on one node, the table in the
`columnar` access method: no UPDATE, DELETE or GIN, so it is not in the update tables). A latency is the wall time of one request from this
process to the server on 127.0.0.1 and back, one client, the response read to its end (psql / HTTP keep-alive connection). The first run of
a query on an engine is not timed: its result is compared with the result of PostgreSQL (the reference) and the run stops on a difference.
"""
import argparse, csv, functools, glob, hashlib, inspect, itertools, json, math, os, queue, re, statistics, subprocess, sys, threading, time, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from stats import number, summary  # noqa: E402  (steps/stats.py)
import benchlib  # noqa: E402  (steps/benchlib.py: host fingerprint, cell cache, row replacement)
import store_engine  # noqa: E402  (steps/store_engine.py: UTIL is set here)
from store_engine import BASE, Client, Http, Utilization, Window, ask, docker, es_text, gate, pg_args, psql_file, psql_value, quiet_gate, restart, server_log, wait_idle  # noqa: E402
from store_lib import CHUNK, CH_AUTH, CH_TABLE, COMPRESSION, DDL, DEFAULT_ENGINES, LANES, LOAD_QUERIES, LOAD_QUERY, NAME, NOISY, NULL, PG_ENGINES, START_LOAD, START_PRESSURE, TOL, UPDATE_ENGINES, container, die, is_ch, is_es, load_queries, log, read, sections  # noqa: E402
from store_render import QUERY_TITLES, Results, all_lanes, cmd_compare, cmd_gap, cmd_render, cmd_table  # noqa: E402


# --- the queries


# --- the clients


# --- docker


# --- one engine at a time, idle background work before timing


# --- external noise: what the host did during a timed window that was not the benchmark's own work
# The pressure stall information cannot tell the benchmark's own loads, restarts and neighbouring cells from the host's other users, so it is not used to
# decide anything (it is logged). A window is measured instead: the CPU the host spent, less the CPU of the containers of this run and of this process
# tree, is the work of somebody else (`external_cpu`, cores); the same for the disk of the volume the containers live on (`external_io`, MiB/s).


# --- data


def csv_path(args):
    return os.path.join(args.data, f'orders-{args.rows}.csv')


def stamp(args):
    return f'{args.rows} {hashlib.sha256(read("gen.sql").encode()).hexdigest()[:12]}'


def cmd_gen(args):
    path = csv_path(args)
    os.makedirs(args.data, exist_ok=True)
    if os.path.exists(path) and open(path + '.stamp').read().strip().split()[:2] == stamp(args).split():
        log(f'{path}: already generated ({open(path + ".stamp").read().split()[2][:12]})')
        return 0
    t0 = time.time()
    psql_file('pg', 'DROP DATABASE IF EXISTS gen WITH (FORCE); CREATE DATABASE gen', 'postgres')
    psql_file('pg', read('gen.sql'), 'gen', [f'n={args.rows}'])
    psql_file('pg', f"\\copy orders_src TO '{path}' CSV", 'gen')
    psql_file('pg', 'DROP DATABASE gen WITH (FORCE)', 'postgres')
    digest = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    open(path + '.stamp', 'w').write(f'{stamp(args)} {digest}\n')
    log(f'{path}: {args.rows} orders, {os.path.getsize(path) / 1e6:.0f} MB, generated in {time.time() - t0:.0f} s, sha256 {digest[:16]}')
    return 0


class Rows:
    def __init__(self, tier):
        self.tier, self.rows = tier, []

    def add(self, engine, cls, variant, metric, unit, value=None, st=None):
        r = dict(dataset='store', tier=self.tier, engine=engine, **{'class': cls}, variant=variant, metric=metric, unit=unit)
        r.update(st if st else dict(value=value, min=value, max=value, stderr=None, n=1, values=[value]))
        r['values'] = [round(v, 4) if isinstance(v, float) else v for v in r['values']]
        for k in ('value', 'min', 'max', 'stderr'):
            if isinstance(r[k], float):
                r[k] = round(r[k], 4)
        self.rows.append(r)


def mib(b):
    return round(float(b) / 1048576, 1)


def timed(rows, eid, phase, fn):
    t0 = time.time()
    out = fn()
    rows.add(eid, '-', phase, 'load_s', 's', value=round(time.time() - t0, 1))
    return out


# Citus columnar keeps its stripe, chunk group and chunk metadata in tables of its own schema, shared by the columnar tables of the database (only `orders` here): on disk, so counted
COLUMNAR_META = " + (SELECT coalesce(sum(pg_total_relation_size(oid)), 0) FROM pg_class WHERE relnamespace = 'columnar_internal'::regnamespace AND relkind = 'r')"


def copy_chunks(eid, path):
    """COPY the file in transactions of CHUNK rows, a VACUUM after each (it merges the segments a sift_table closes), through one psql session"""
    p = subprocess.Popen(['psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', *pg_args(eid), '-f', '-'], stdin=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        with open(path) as f:
            while lines := list(itertools.islice(f, CHUNK)):
                p.stdin.write('COPY orders FROM STDIN CSV;\n')
                p.stdin.writelines(lines)
                p.stdin.write('\\.\nVACUUM orders;\n')
        p.stdin.close()
    except BrokenPipeError:   # psql stopped on an error: its message follows
        pass
    err = p.stderr.read()
    if p.wait():
        raise RuntimeError('psql: ' + err.strip()[:500])


def build_indexes(eid, text, rows):
    """each statement of the `indexes` section on its own, timed (`build NAME`, the seconds a user waits for that index); the `compression` option of the lane goes on the sift_col index"""
    total = 0
    for stmt in re.split(r';[ \t]*\n', re.sub(r'(?m)^--.*$', '', text)):
        if not stmt.strip():
            continue
        if eid in COMPRESSION:
            stmt = re.sub(r'(USING sift_col \(.*\))\s*$', rf"\1 WITH (compression = '{COMPRESSION[eid]}')", stmt.strip().rstrip(';'))
        name = m[1] if (m := re.search(r'INDEX (\w+)', stmt)) else 'primary key'
        t0 = time.time()
        psql_file(eid, stmt + ';')
        rows.add(eid, '-', 'build ' + name, 'build_s', 's', value=round(time.time() - t0, 1))
        total += time.time() - t0
    rows.add(eid, '-', 'indexes', 'load_s', 's', value=round(total, 1))


def load_pg(eid, args, rows):
    sec = sections(read(DDL[eid]))
    db = LANES[eid][3]
    def fresh():
        psql_file(eid, f'DROP DATABASE IF EXISTS {db} WITH (FORCE); CREATE DATABASE {db}', 'postgres')
        psql_file(eid, sec['table'])

    fresh()
    try:   # one COPY, one transaction: what a user does first
        timed(rows, eid, 'rows', lambda: psql_file(eid, f"\\copy orders FROM '{csv_path(args)}' CSV"))
    except RuntimeError as x:   # a failure is a result; the rows then go in transactions of CHUNK rows
        rows.add(eid, '-', 'rows', 'load_error', 'text', value=str(x)[:400])
        log(f'{eid}: {x}; loading in transactions of {CHUNK:,} rows')
        fresh()
        timed(rows, eid, 'rows', lambda: copy_chunks(eid, csv_path(args)))
        rows.add(eid, '-', 'rows', 'chunk_rows', 'rows', value=CHUNK)
    build_indexes(eid, sec['indexes'], rows)
    timed(rows, eid, 'vacuum', lambda: psql_file(eid, 'VACUUM (FREEZE, ANALYZE) orders'))
    psql_file(eid, 'CHECKPOINT')
    plus = COLUMNAR_META if eid == 'citus' else ''
    rows.add(eid, '-', '-', 'size_mib', 'MiB', value=mib(psql_value(eid, "SELECT pg_total_relation_size('orders')" + plus)))
    rows.add(eid, '-', 'table', 'size_mib', 'MiB', value=mib(psql_value(eid, "SELECT pg_table_size('orders')" + plus)))
    if eid == 'citus':   # the settings the table was created with, read back from the server (the defaults of columnar.*)
        rows.add(eid, '-', 'orders', 'columnar_options', 'text', value=psql_value(eid, "SELECT 'chunk_group_row_limit=' || chunk_group_row_limit || ', stripe_row_limit=' || stripe_row_limit || ', compression=' || "
                                                                                       "compression || ', compression_level=' || compression_level FROM columnar.options WHERE relation = 'orders'::regclass"))
    if eid == 'sift_table':   # reusable blocks left inside the table file after the load's VACUUM
        rows.add(eid, '-', 'free', 'size_mib', 'MiB', value=mib(int(psql_value(eid, "SELECT free_blocks FROM sift.table_space('orders')")) * 8192))
    if eid == 'sift' or eid in COMPRESSION:   # what the index holds: the options it was built with and, per leaf, the chunks by encoding and their bytes
        rows.add(eid, '-', 'orders_col', 'reloptions', 'text', value=psql_value(eid, "SELECT coalesce(reloptions::text, '(not set)') FROM pg_class WHERE relname = 'orders_col'"))
        rows.add(eid, '-', 'orders_col', 'col_leaves', 'json', value=psql_value(eid, "SELECT json_agg(json_build_object('leaf', path, 'type', type, 'const', const_chunks, 'for', for_chunks, "
                                                                                 "'pack', pack_chunks, 'codec', codec_chunks, 'bytes', chunk_bytes, 'dict', dict_bytes) ORDER BY leaf) FROM sift.col_leaves('orders_col')"))
    for name, size in (l.split('|') for l in psql_value(eid, "SELECT indexrelid::regclass || '|' || pg_relation_size(indexrelid) FROM pg_index WHERE indrelid = 'orders'::regclass ORDER BY 1").split('\n')):
        rows.add(eid, '-', 'index ' + name, 'size_mib', 'MiB', value=mib(size))


def load_ch(args, rows):
    h = Http('ch', CH_AUTH)
    sec = sections(read('ch.sql'))
    h.request('POST', '/', b'DROP TABLE IF EXISTS store.orders')
    for stmt in sec['table'].split(';\n'):
        h.request('POST', '/', stmt.rstrip(';').encode())
    with open(csv_path(args), 'rb') as f:
        timed(rows, 'ch', 'rows', lambda: h.request('POST', '/?' + urllib.parse.urlencode({'query': 'INSERT INTO store.orders FORMAT CSV'}), f))
    timed(rows, 'ch', 'merge', lambda: h.request('POST', '/', sec['after'].rstrip(';').encode()))
    size = h.request('POST', '/', b"SELECT sum(bytes_on_disk) FROM system.parts WHERE database = 'store' AND table = 'orders' AND active").decode()
    rows.add('ch', '-', '-', 'size_mib', 'MiB', value=mib(size))
    h.close()


def bulk_docs(path, batch):
    with open(path, newline='') as f:
        buf = []
        for r in csv.reader(f):
            buf.append('{"index":{"_id":"%s"}}\n%s\n' % (r[0], json.dumps(dict(id=int(r[0]), customer_id=int(r[1]), country=r[2], category=r[3], status=r[4], amount=float(r[5]),
                                                                                quantity=int(r[6]), created_at=r[7], title=r[8]))))
            if len(buf) == batch:
                yield ''.join(buf).encode()
                buf = []
        if buf:
            yield ''.join(buf).encode()


def load_es(args, rows):
    h = Http('es')
    js = {'Content-Type': 'application/json'}
    h.request('DELETE', '/orders?ignore_unavailable=true')
    h.request('PUT', '/orders', read('es.json').replace('"number_of_shards": 1', f'"number_of_shards": {args.cpus}').encode(), js)   # one shard per CPU of the budget the load runs at: one shard is searched by one thread, whatever node.processors says
    q, errors = queue.Queue(maxsize=6), []

    def sender():
        c = Http('es')
        while True:
            body = q.get()
            if body is None:
                return
            try:
                if json.loads(c.request('POST', '/orders/_bulk', body, {'Content-Type': 'application/x-ndjson'}))['errors']:
                    errors.append('bulk reported errors')
            except Exception as x:
                errors.append(str(x))

    def bulk():
        threads = [threading.Thread(target=sender) for _ in range(3)]
        for t in threads:
            t.start()
        for body in bulk_docs(csv_path(args), 20000):
            q.put(body)
        for _ in threads:
            q.put(None)
        for t in threads:
            t.join()
        if errors:
            die('elasticsearch bulk: ' + errors[0])
        h.request('POST', '/orders/_refresh')

    timed(rows, 'es', 'rows', bulk)
    h.request('PUT', '/orders/_settings', json.dumps({'index': {'refresh_interval': '1s'}}).encode(), js)
    timed(rows, 'es', 'merge', lambda: h.request('POST', '/orders/_forcemerge?max_num_segments=1'))
    h.request('POST', '/orders/_flush')
    stats = json.loads(h.request('GET', '/orders/_stats/store'))
    rows.add('es', '-', '-', 'size_mib', 'MiB', value=mib(stats['_all']['primaries']['store']['size_in_bytes']))
    h.close()


def servers(engines):
    """the versions and settings of the servers that take part, as the servers say them"""
    v = {}
    settings = lambda eid: psql_value(eid, "SELECT string_agg(name || '=' || current_setting(name), ', ' ORDER BY name) FROM pg_settings WHERE name IN "
                                           "('shared_buffers', 'work_mem', 'jit', 'effective_cache_size')")
    for eid in [e for e in ('pg', 'sift', 'sift_table') if e in engines]:
        v[eid] = psql_value(eid, 'SHOW server_version')
        v[eid + '_sift'] = psql_value(eid, "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")
        v[eid + '_settings'] = settings(eid)
    if 'citus' in engines:
        v['citus'] = psql_value('citus', 'SHOW server_version')
        v['citus_columnar'] = psql_value('citus', "SELECT extversion FROM pg_extension WHERE extname = 'citus_columnar'")
        v['citus_image'] = docker('inspect', '-f', '{{.Config.Image}}', container('citus')).strip()
        v['citus_settings'] = settings('citus')
    if 'sift' in engines:
        v['sift_libs'] = docker('exec', container('sift'), 'sh', '-c', 'ldd $(find /usr/lib/postgresql -name pg_sift.so | head -1) | grep -Eo "lib(lz4|zstd)[^ ]*" | sort -u | paste -sd" "').strip()
        v['sift_table_compression'] = psql_value('sift', "LOAD 'pg_sift'; SELECT current_setting('sift.table_compression')") or "'' (unset)"
    if any(is_es(e) for e in engines):
        es = json.loads(Http('es').request('GET', '/'))
        v['es'] = es['version']['number']
        v['es_lucene'] = es['version']['lucene_version']
        opts = next(l.split('=', 1)[1] for l in docker('inspect', '-f', '{{range .Config.Env}}{{println .}}{{end}}', container('es')).split('\n') if l.startswith('ES_JAVA_OPTS'))
        v['es_heap'] = ' '.join(a for a in opts.split() if a.startswith('-Xm'))
    if any(is_ch(e) for e in engines):
        ch = Http('ch', CH_AUTH)
        v['ch'] = ch.request('POST', '/', b'SELECT version()').decode().strip()
        v['ch_threads'] = ch.request('POST', '/', b"SELECT value FROM system.settings WHERE name = 'max_threads'").decode().strip()
    return v


PG_THREADS = ('max_parallel_workers_per_gather', 'max_parallel_maintenance_workers', 'max_parallel_workers', 'max_worker_processes', 'parallel_setup_cost', 'parallel_tuple_cost',
              'min_parallel_table_scan_size', 'min_parallel_index_scan_size')


def effective(args, plan=True):
    """the thread settings of every server of the engines, as the servers report them, checked against the budget N (--cpus): PostgreSQL and pg_sift a leader plus N - 1 parallel
    workers, ClickHouse and Elasticsearch N threads / processors, the container a quota of N CPUs and no pinning. Dies on a difference; plan: also how many workers a count(*) plans"""
    n, out, wrong = args.cpus, {}, []

    def check(who, name, got, want):
        if str(got) != str(want):
            wrong.append(f'{who} {name} is {got}, expected {want}')

    for eid in {LANES[e][1]: e for e in reversed(args.engines)}.values():
        c = who = LANES[eid][1]
        inspect = lambda f: docker('inspect', '-f', '{{.HostConfig.' + f + '}}', container(eid)).strip()
        out[c] = dict(quota_cpus=int(inspect('NanoCpus')) / 1e9, cpuset=inspect('CpusetCpus'))
        check(who, 'container quota (--cpus)', out[c]['quota_cpus'], float(n))
        check(who, 'cpuset (the CPUs are not pinned)', out[c]['cpuset'] or 'none', 'none')
        if eid in PG_ENGINES:
            names = ', '.join(f"'{k}'" for k in PG_THREADS)
            got = dict(l.split('=') for l in psql_value(eid, f"SELECT string_agg(name || '=' || current_setting(name), E'\n') FROM pg_settings WHERE name IN ({names})", 'postgres').split('\n'))
            out[c].update(got)
            check(who, 'max_parallel_workers_per_gather', got['max_parallel_workers_per_gather'], n - 1)
            check(who, 'max_parallel_maintenance_workers', got['max_parallel_maintenance_workers'], n - 1)
            if int(got['max_parallel_workers']) < n - 1 or int(got['max_worker_processes']) < int(got['max_parallel_workers']):
                wrong.append(f'{who} max_parallel_workers {got["max_parallel_workers"]} / max_worker_processes {got["max_worker_processes"]} would cap the workers')
            if plan:   # the planner's own answer on a scan of the table: Workers Planned (absent: no Gather, the plan is not parallel)
                m = re.search(r'Workers Planned: (\d+)', psql_value(eid, 'EXPLAIN SELECT count(*) FROM orders'))
                out[c]['count_workers_planned'] = int(m[1]) if m else 0
        elif is_ch(eid):
            q = lambda t, ns: dict(l.split('\t') for l in ask('ch', 'POST', '/', f"SELECT name, value FROM system.{t} WHERE name IN ({ns}) FORMAT TSV".encode()).splitlines())
            out[c].update(q('settings', "'max_threads', 'max_insert_threads', 'max_final_threads'"))
            out[c].update(q('server_settings', "'background_pool_size', 'background_merges_mutations_concurrency_ratio'"))
            for k in ('max_threads', 'max_insert_threads', 'max_final_threads', 'background_pool_size'):
                check(who, k, out[c][k], n)
            check(who, 'background_merges_mutations_concurrency_ratio', out[c]['background_merges_mutations_concurrency_ratio'], 1)
        else:
            node = next(iter(json.loads(ask('es', 'GET', '/_nodes/os?filter_path=**.allocated_processors,**.available_processors'))['nodes'].values()))['os']
            jvm = next(iter(json.loads(ask('es', 'GET', '/_nodes/jvm?filter_path=**.input_arguments'))['nodes'].values()))['jvm']['input_arguments']
            pools = {r['name']: int(r['size']) for r in json.loads(ask('es', 'GET', '/_cat/thread_pool/search,write?format=json&h=name,size'))}
            out[c].update(node, jvm_processor_count=next((a.split('=')[1] for a in jvm if a.startswith('-XX:ActiveProcessorCount')), None), thread_pools=pools)
            check(who, 'allocated_processors (node.processors)', node['allocated_processors'], n)
            check(who, '-XX:ActiveProcessorCount', out[c]['jvm_processor_count'], n)
    if wrong:
        die('the thread budget is not in effect: ' + '; '.join(wrong))
    return out


def size_variants(args, rows):
    """the on-disk size of the systems with their other compression settings, the data copied inside each server: rows `codec NAME`"""
    if 'sift_table' in args.engines:
        for pol in ('none', 'auto', 'lz4', 'zstd:3'):
            t = 'orders_' + pol.split(':')[0]
            psql_file('sift_table', f"DROP TABLE IF EXISTS {t}; CREATE TABLE {t} (LIKE orders) USING sift_table; SET sift.table_compression = '{pol}'; INSERT INTO {t} SELECT * FROM orders; VACUUM {t}")
            rows.add('sift_table', '-', 'codec ' + pol, 'size_mib', 'MiB', value=mib(psql_value('sift_table', f"SELECT pg_table_size('{t}')")))
            rows.add('sift_table', '-', 'codec ' + pol + ' free', 'size_mib', 'MiB', value=mib(int(psql_value('sift_table', f"SELECT free_blocks FROM sift.table_space('{t}')")) * 8192))
            psql_file('sift_table', f'DROP TABLE {t}')
    if 'ch' in args.engines:
        h = Http('ch', CH_AUTH)
        ddl = lambda q: h.request('POST', '/', q.encode())
        cols = ddl("SELECT name FROM system.columns WHERE database = 'store' AND table = 'orders' ORDER BY position").decode().split()
        disk = lambda t: ddl(f"SELECT sum(bytes_on_disk) || ' ' || sum(data_uncompressed_bytes) FROM system.parts WHERE database = 'store' AND table = '{t}' AND active").decode().split()
        rows.add('ch', '-', 'codec default (LZ4)', 'size_mib', 'MiB', value=mib(disk('orders')[0]))
        rows.add('ch', '-', 'codec default (LZ4)', 'uncompressed_mib', 'MiB', value=mib(disk('orders')[1]))
        ddl('DROP TABLE IF EXISTS store.orders_zstd')
        ddl('CREATE TABLE store.orders_zstd AS store.orders')
        for c in cols:
            ddl(f'ALTER TABLE store.orders_zstd MODIFY COLUMN {c} CODEC(ZSTD(3))')
        ddl('INSERT INTO store.orders_zstd SELECT * FROM store.orders')
        ddl('OPTIMIZE TABLE store.orders_zstd FINAL')
        rows.add('ch', '-', 'codec ZSTD(3)', 'size_mib', 'MiB', value=mib(disk('orders_zstd')[0]))
        ddl('DROP TABLE store.orders_zstd')
    if 'es' in args.engines:
        h, js = Http('es'), {'Content-Type': 'application/json'}
        idx = json.loads(read('es.json'))
        idx['settings']['index']['codec'] = 'best_compression'
        h.request('DELETE', '/orders_best?ignore_unavailable=true')
        h.request('PUT', '/orders_best', json.dumps(idx).encode(), js)
        h.request('POST', '/_reindex?wait_for_completion=true&refresh=true', json.dumps({'source': {'index': 'orders'}, 'dest': {'index': 'orders_best'}}).encode(), js)
        h.request('POST', '/orders_best/_forcemerge?max_num_segments=1')
        h.request('POST', '/orders_best/_flush')
        rows.add('es', '-', 'codec best_compression', 'size_mib', 'MiB', value=mib(json.loads(h.request('GET', '/orders_best/_stats/store'))['_all']['primaries']['store']['size_in_bytes']))
        h.request('DELETE', '/orders_best')


def cmd_load(args):
    rows = Rows(args.tier)
    if not os.path.exists(csv_path(args)):
        die(f'{csv_path(args)} not found: run gen first')
    rows.add('-', '-', '-', 'orders', 'rows', value=args.rows)
    effective(args, plan=False)
    quiet_gate('load')   # the one wait for a quiet host of the stage: nothing is waited for between the cells
    keys, reused = dict(kv.split('=') for kv in args.keys), {}
    for c in args.reuse:   # the data of these containers is already in their volume (same key): their load times and sizes are those of the load that put it there, dated
        m = json.load(open(marker_path(args.data, c, keys[c])))
        rows.rows += [dict(r, tier=args.tier) for r in m['rows']]
        reused[c] = m['date']
        log(f'{c}: the loaded data is reused (loaded {m["date"]}, key {keys[c]})')
    for eid in args.engines:
        if eid == 'es_cached' or LANES[eid][1] in args.reuse:
            continue
        log(f'loading {eid}')
        if eid in PG_ENGINES:
            load_pg(eid, args, rows)
        elif eid == 'ch':
            load_ch(args, rows)
        else:
            load_es(args, rows)
        total = sum(r['value'] for r in rows.rows if r['engine'] == eid and r['metric'] == 'load_s')
        rows.add(eid, '-', '-', 'load_s', 's', value=round(total, 1))
        log(f'{eid}: loaded in {total:.0f} s, {next(r["value"] for r in rows.rows if r["engine"] == eid and r["metric"] == "size_mib" and r["variant"] == "-")} MiB')
    if args.compression:
        size_variants(args, rows)
    for c, key in keys.items():   # what is loaded now stays in the volume: a pass that finds the same key there loads nothing
        lanes = [e for e in args.engines if LANES[e][1] == c and e != 'es_cached']
        if c not in reused and lanes:
            benchlib.write_json(marker_path(args.data, c, key), dict(date=benchlib.now_iso(), key=key, rows=[r for r in rows.rows if r['engine'] in lanes]))
    busy = wait_idle(args.engines)
    log(f'the servers are idle after the load' if not busy else f'still busy after the load (the timed batches wait again): {busy}')
    args.threads = effective(args)
    write(args, rows, 'load', {'reused': reused})
    return 0


def marker_path(data, container_, key):
    """the record of a load that finished: its rows and date, next to the CSV; the data volume of the container under the same key holds the data it describes"""
    return os.path.join(data, 'loaded', f'{container_}-{key}.json')


LOAD_CODE = None   # set in main(): the functions that decide what a loaded server holds


def cmd_datakey(args):
    """one line per container: `CONTAINER KEY VOLUME reuse|new`. The key says what its data volume holds: the seeded rows, the image, the table definitions, the load code and settings;
    `reuse`: a volume of that key exists and the load that filled it finished (store.sh keeps the volume under this name and mounts it, and loads nothing)"""
    images = {c: image_ids(c)[1] for c in ('pg', 'citus', 'es', 'ch')}
    images['sift'] = docker('image', 'inspect', '-f', '{{.Id}}', f'{NAME}-pg:latest').strip()   # PostgreSQL with pg_sift built from this checkout
    for c in args.containers:
        lanes = [e for e in args.engines if LANES[e][1] == c and e != 'es_cached']
        files = sorted({DDL[e] for e in lanes if e in DDL} | {{'ch': 'ch.sql', 'es': 'es.json'}.get(c, '')} - {''})
        key = benchlib.key_of(dict(container=c, lanes=lanes, image=images[c], dataset=dataset_id(args), files={f: benchlib.key_of(read(f), 12) for f in files}, code=LOAD_CODE,
                                   server_flags=os.environ.get('STORE_PG_FLAGS', '') if c in ('pg', 'sift', 'citus') else '', shards=args.cpus if c == 'es' else 0, compression=args.compression), 12)
        vol = f'{NAME}-data-{c}-{key}'
        have_volume = subprocess.run(['docker', 'volume', 'inspect', vol], capture_output=True).returncode == 0
        print(c, key, vol, 'reuse' if have_volume and os.path.exists(marker_path(args.data, c, key)) else 'new')
    return 0


PARTS = {'run': 'warm', 'cold': 'cold', 'updates': 'updates'}   # the command and the part file it writes


def part_path(args, part):
    return os.path.join(args.out, f'store-{args.tier}-{part}.json')


def part_key(args, part):
    """what a part file's numbers depend on: the code, the host, the data and every setting of the command (a part with this key is not measured again by `resume`)"""
    return benchlib.key_of(dict(part=part, code=MEASURE_CODE, fp=host_fp()['id'], data=dataset_id(args), rows=args.rows, cpus=args.cpus, engines=args.engines, untimed=args.untimed,
                                queries=args.queries, runs=args.runs, warmups=args.warmups, cold=args.cold_runs, restarts=args.restart_runs, check=args.cold_check, rates=args.rates,
                                load=args.load_seconds, probes=args.probes, parallel=args.parallel), 16)


def part_done(args):
    """whether the part file of this command exists and holds this command's key: its cells were measured before and are kept"""
    path = part_path(args, PARTS[args.cmd])
    try:
        with open(path) as f:
            return json.load(f)['meta'].get('key') == part_key(args, PARTS[args.cmd])
    except (OSError, ValueError):
        return False


def write(args, rows, part, extra=None):
    os.makedirs(args.out, exist_ok=True)
    path = part_path(args, part)
    rows.rows = [r for r in rows.rows if r['engine'] not in args.untimed]   # the reference of the result checks is not a result of this pass
    util = store_engine.UTIL.report() if store_engine.UTIL else None
    if util and part != 'load' and util['wall_s'] >= 60 and util['timed_pct'] < 70:
        log(f'WARN: {part}: timed windows are {util["timed_pct"]}% of the {util["wall_s"]} s of the command (below 70%): the rest was waiting for restarts, cache drops, gates; '
            f'{util["low_busy_pct"]}% of the time fewer than {util["low_busy_below"]} cores were busy')
    meta = dict(key=part_key(args, part), date=time.strftime('%FT%TZ', time.gmtime()), baseline=BASE, utilization=util, parallel=getattr(args, 'parallel', 1), tier=args.tier, rows=args.rows, cpus=args.cpus, threads=args.threads, data_sha256=open(csv_path(args) + '.stamp').read().split()[2], loadavg=START_LOAD, loadavg_end=open('/proc/loadavg').read().split()[:3], pressure=START_PRESSURE, noisy=NOISY, **(extra or {}))
    benchlib.write_text(path, '{"meta": ' + json.dumps(meta) + ',\n "rows": [\n' + ',\n'.join(' ' + json.dumps(r) for r in rows.rows) + '\n]}\n')
    log(f'written: {path}')


# --- the measurements

def rotated(items, i):
    k = i % len(items)
    return items[k:] + items[:k]


def difference(want, got, key, tol):
    """None when the results are equal, else what differs first: strings and integers exactly, numbers within tol (relative); rows sorted by the key columns when the query has them"""
    cut = lambda t: [[NULL if c == '' else c for c in l.split('\t')] for l in t.split('\n') if l != '']
    a, b = cut(want), cut(got)
    if key:
        a, b = sorted(a, key=lambda r: [r[k] for k in key]), sorted(b, key=lambda r: [r[k] for k in key])
    if len(a) != len(b):
        return f'{len(b)} rows, expected {len(a)}'
    for i, (ra, rb) in enumerate(zip(a, b)):
        if len(ra) != len(rb):
            return f'row {i}: {len(rb)} columns, expected {len(ra)}'
        for j, (x, y) in enumerate(zip(ra, rb)):
            nx, ny = number(x), number(y)
            if not (x == y or (nx is not None and ny is not None and abs(nx - ny) <= tol * max(1.0, abs(nx), abs(ny)))):
                return f'row {i} column {j + 1}: {y} expected {x} ({"|".join(ra)[:80]})'
    return None


def max_rel(want, got, key):
    """the largest relative difference between the numbers of two results (rows matched as in `difference`)"""
    cut = lambda t: sorted([l.split('\t') for l in t.split('\n') if l != ''], key=(lambda r: [r[k] for k in key]) if key else None)
    worst = 0.0
    for ra, rb in zip(cut(want), cut(got)):
        for x, y in zip(ra, rb):
            nx, ny = number(x), number(y)
            if nx is not None and ny is not None:
                worst = max(worst, abs(nx - ny) / max(1.0, abs(nx), abs(ny)))
    return worst


def tolerance(eid, st):
    return st[4] if is_es(eid) else TOL


def measure_query(q, clients, args, rows):
    """the cell of one question on every engine, in a window whose external noise is recorded; (the results that differ, why the window was NOISY)"""
    for c in clients.values():
        c.busy = c.cpu = 0.0
    with Window() as w:
        stop = run_query(q, clients, args, rows)
    for e, c in clients.items():   # the cores the container used while this client's requests ran: its CPU over the wall time of those requests (not the window: the other engines are asked in it)
        cores = c.cpu / c.busy if c.busy and e not in args.untimed else None
        quota = args.cpus * 1.05
        if cores is not None and cores > quota:
            log(f'{q.id} {e}: {cores:.2f} cores in the requests, more than the quota of {args.cpus}: not recorded')
        elif cores is not None:
            rows.add(e, q.id, 'warm', 'cpu_cores', 'cores', value=round(cores, 2))
    return stop, w.record(rows, q.id, 'warm', w.judge({container(e) for e in clients}, io=True))


def one_container(lanes, stmts, clients, args, times):
    """the untimed warm-ups, then the timed runs of the lanes of one container, the first lane rotating every run (the lanes of one container share its CPUs, so they never run together)"""
    for _ in range(args.warmups - 1):
        for e in lanes:
            clients[e].run(stmts[e])
    for i in range(args.runs):
        for e in rotated(lanes, i):
            times[e].append(clients[e].run(stmts[e])[0])


def time_cell(live, stmts, clients, args):
    """the timed runs of one question: one engine at a time, the engines interleaved run by run (the method of the published tables), or with --parallel-engines N the containers in
    waves of N at the same time, each on cores of its own (time_wave)"""
    times = {e: [] for e in live}
    boxes = list(dict.fromkeys(container(e) for e in live))
    if args.parallel <= 1 or len(boxes) < 2:
        for _ in range(args.warmups - 1):
            for e in live:
                clients[e].run(stmts[e])
        for i in range(args.runs):
            for e in rotated(live, i):
                times[e].append(clients[e].run(stmts[e])[0])
        return times
    cores = physical_cores()
    n = min(args.parallel, benchlib.max_parallel(cores, args.cpus))
    boxes = rotated(boxes, args.cell)   # which container is in which wave moves from question to question
    for w in range(0, len(boxes), n):
        time_wave(boxes[w:w + n], live, stmts, clients, args, times, cores, args.cell + w)
    return times


@functools.lru_cache(maxsize=None)
def physical_cores():
    return benchlib.physical_cores()


def pin(box, cores):
    """docker --cpuset-cpus: the container runs on these CPUs only; None frees it again (all CPUs of the host)"""
    docker('update', '--cpuset-cpus', ','.join(map(str, cores)) if cores else f'0-{os.cpu_count() - 1}', box)


def time_wave(boxes, live, stmts, clients, args, times, cores, rotation):
    """the containers of a wave run their timed requests at the same time, each on `--cpus` physical cores of its own (equal sets, moved round the cores from wave to wave so that no engine
    always gets the same ones; the other container threads of the host are not pinned). The cores are freed again afterwards"""
    sets, errors = benchlib.cpuset_plan(cores, args.cpus, len(boxes), rotation), []

    def work(box):
        try:
            one_container([e for e in live if container(e) == box], stmts, clients, args, times)
        except Exception as x:
            errors.append(x)

    try:
        for box, s in zip(boxes, sets):
            pin(box, s)
        threads = [threading.Thread(target=work, args=(box,)) for box in boxes]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    finally:
        for box in boxes:
            pin(box, None)
    if errors:
        raise errors[0]


def run_query(q, clients, args, rows):
    stmts = {e: q.statement(e, args.rows, e == 'es_cached') for e in clients}
    out, bad = {}, {}
    for e, c in clients.items():   # the first run of each engine: its result is checked, the plan is read (an untimed engine is only the reference: no rows of it)
        try:
            out[e] = c.run(stmts[e], True)[1]
            plan = c.explain(stmts[e])
            if plan is not None and e not in args.untimed:
                rows.add(e, q.id, '-', 'plan', 'node', value=plan)
        except Exception as x:
            bad[e] = str(x)
            log(f'{q.id} {e}: {x}')
    ref = out.get('pg', next(iter(out.values()), None))
    stop = []
    for e in out:
        if e == 'pg' or ref is None:
            verdict = 'reference' if e == 'pg' else 'unchecked'
        else:
            d = difference(ref, out[e], q.key, tolerance(e, stmts[e]))
            verdict = 'ok' if d is None else 'DIFF: ' + d
            if d is None:
                rows.add(e, q.id, '-', 'rel_diff', 'fraction', value=float(f'{max_rel(ref, out[e], q.key):.3g}'))
            if d is not None:
                stop.append(f'{q.id} {q.title}: {e} differs from PostgreSQL: {d}')
        if e not in args.untimed:
            rows.add(e, q.id, '-', 'result', 'check', value=verdict)
    for e, msg in bad.items():
        rows.add(e, q.id, '-', 'result', 'check', value='error: ' + msg)
    live = [e for e in clients if e in out and e not in args.untimed]
    times = time_cell(live, stmts, clients, args)
    for e in live:
        rows.add(e, q.id, 'warm', 'latency_ms', 'ms', st=summary(times[e]))
    log(f'{q.id} {q.name}: ' + ' '.join(f'{e} {sorted(times[e])[len(times[e]) // 2]:.3g}' for e in live))
    return stop


def cmd_run(args):
    rows = Rows(args.tier)
    queries = load_queries(args.queries)
    args.threads = effective(args)
    clients = {e: Client(e) for e in args.engines}
    problems = []
    log(f'{len(queries)} queries, engines {" ".join(args.engines)}, {args.runs} runs after {args.warmups} warm-up(s), thread budget {args.cpus}')
    gate('run', args.engines)
    noisy, again = {}, []
    try:
        for i, q in enumerate(queries):
            args.cell = i
            stop, why = measure_query(q, clients, args, rows)
            problems += stop
            if why:
                noisy[q.id] = why
            if problems and not args.keep_going:
                break
        for q in [q for q in queries if q.id in noisy and not problems][:args.max_rerun]:   # a NOISY cell is measured again once, at the end of the stage, not waited on
            log(f'{q.id}: NOISY ({noisy[q.id]}), measured again')
            rows.rows = [r for r in rows.rows if r['class'] != q.id]
            args.cell = queries.index(q) + 1   # another wave order than the first time
            stop, why = measure_query(q, clients, args, rows)
            problems += stop
            again.append(q.id)
            if why:
                NOISY.append(f'{q.id}: {why}')
    finally:
        for c in clients.values():
            c.close()
        write(args, rows, 'warm', {'servers': servers(args.engines), 'runs': args.runs, 'warmups': args.warmups, 'rerun': again})
    for p in problems:
        print('DIFFERENT RESULT: ' + p, file=sys.stderr)
    return 3 if problems else 0


COLD_HOW = {'es': 'restart'}   # emptying the memory of Elasticsearch is not as cold as a restart, so its cold cells restart the container


def evict(e):
    """what a restart empties, without one: the memory the server keeps its data and caches in. The operating system's page cache stays, as after a restart (dropping it needs root)"""
    if e in PG_ENGINES:   # every shared buffer, from the maintenance database (a new backend starts each cell: no relation, catalog or plan caches)
        psql_value('pg' if e == 'pg' else e, 'SELECT pg_buffercache_evict_all()', 'postgres')
    elif is_ch(e):        # the mark, uncompressed-block, primary-key and skipping-index caches; the parts stay attached, as after a restart they are loaded again
        for what in ('MARK CACHE', 'UNCOMPRESSED CACHE', 'PRIMARY INDEX CACHE', 'INDEX MARK CACHE', 'INDEX UNCOMPRESSED CACHE', 'QUERY CACHE'):
            ask('ch', 'POST', '/', f'SYSTEM DROP {what}'.encode())
    else:                 # the node's caches, and the shards' readers (terms dictionaries, doc value and points structures) closed and opened again
        ask('es', 'POST', '/orders/_cache/clear')
        ask('es', 'POST', '/orders/_close')
        ask('es', 'POST', '/orders/_open?wait_for_active_shards=all')
        ask('es', 'GET', '/_cluster/health/orders?wait_for_status=green&timeout=60s')


def cold_cell(e, q, stmt, args, how='evict'):
    """the first run of a question after the server's memory was emptied (`evict`: its caches and buffers, `--cold-runs` times; `restart`: the container, `--restart-runs` times:
    a restart takes seconds, so it has a count of its own), each on a new connection: (the times, the windows of the runs added up); no times when the engine fails"""
    ts, w = [], Window()
    for _ in range(args.restart_runs if how == 'restart' else args.cold_runs):
        c = None
        try:
            with Window() as x:   # the emptying and the first run: the request alone is a few milliseconds, too short for the host's counters
                restart(e) if how == 'restart' else evict(e)
                c = Client(e)
                ts.append(c.run(stmt)[0])
            w.add(x)
        except RuntimeError as err:   # an error is a result, as in the warm run
            log(f'cold {e} {q.id}: {err}')
            return None, w
        finally:
            if c:
                c.close()
    return ts, w


def med(xs):
    return sorted(xs)[len(xs) // 2]


def cold_check(e, q, stmt, how, args, rows, cache):
    """the check that emptying the memory is the cold of a restart: the same question the other way. It is not run unless asked for (--cold-check), and its result is kept in the cache once
    per version of the engine (the image the container runs): (milliseconds of the other way, the date it was measured when it was read from the cache)"""
    other = 'restart' if how == 'evict' else 'evict'
    inputs = dict(engine=e, image=docker('inspect', '-f', '{{.Image}}', container(e)).strip(), rows=args.rows, cpus=args.cpus, question=q.id, how=other, runs=args.restart_runs if other == 'restart' else args.cold_runs)
    hit, why = cache.lookup('check', e, inputs) if cache else (None, '')
    if hit:
        rows.rows += hit['rows']
        return med([r for r in hit['rows'] if r['metric'] == 'latency_ms'][0]['values']), hit['date']
    ts, _ = cold_cell(e, q, stmt, args, other)
    if not ts:
        return None, None
    mine = Rows(args.tier)
    mine.add(e, q.id, 'cold-alt', 'latency_ms', 'ms', st=summary(ts))
    rows.rows += mine.rows
    if cache:
        cache.put('check', e, inputs, dict(rows=mine.rows))
    return med(ts), None


def cmd_cold(args):
    rows = Rows(args.tier)
    queries = load_queries(args.queries)
    engines = [e for e in args.engines if e != 'es_cached' and e not in args.untimed]
    stmts = {(q.id, e): q.statement(e, args.rows) for q in queries for e in engines}
    how = {e: COLD_HOW.get(e, 'evict') for e in engines}
    effective(args)
    for e in engines:   # one restart of each server, then every cell empties its memory without one (a server whose cells restart it needs no restart before)
        if how[e] != 'restart':
            restart(e)
    for e in engines:
        if e in PG_ENGINES:
            psql_value(e, 'CREATE EXTENSION IF NOT EXISTS pg_buffercache', 'postgres')
    gate('cold', args.engines)
    cells = [(e, q) for e in engines for q in queries]
    got, again = {}, []
    for e in engines:
        rows.add(e, '-', 'cold', 'cold_how', 'text', value=how[e])
    for e, q in cells:
        got[e, q.id] = cold_cell(e, q, stmts[q.id, e], args, how[e])
        if got[e, q.id][0]:
            log(f'cold {e} {q.id} {q.name}: {med(got[e, q.id][0]):.3g} ms')
    for e, q in cells:   # a NOISY cell is measured again once, at the end of the stage, not waited on (at most --max-rerun of them: a restart cell takes a minute)
        why = got[e, q.id][1].judge({container(e)})
        if got[e, q.id][0] and why and len(again) < args.max_rerun:
            log(f'cold {e} {q.id}: NOISY ({why}), measured again')
            got[e, q.id] = cold_cell(e, q, stmts[q.id, e], args, how[e])
            again.append(f'{e} {q.id}')
    for e, q in cells:
        ts, w = got[e, q.id]
        if not ts:
            continue
        why = w.record(rows, q.id, f'cold-{e}', w.judge({container(e)}))
        if why:
            NOISY.append(f'cold {e} {q.id}: {why}')
        rows.add(e, q.id, 'cold', 'latency_ms', 'ms', st=summary(ts))
    checked = {}
    for q in [q for q in queries if q.id in args.cold_check]:
        for e in engines:
            alt, date = cold_check(e, q, stmts[q.id, e], how[e], args, rows, args.cache)
            if alt is not None:
                checked[f'{e} {q.id}'] = date or 'measured'
                log(f'cold check {e} {q.id}: {alt:.3g} ms ({"restart" if how[e] == "evict" else "evict"}{", from the cache " + date if date else ""}), {med(got[e, q.id][0]):.3g} ms ({how[e]})')
    args.threads = effective(args)   # after the restarts: the settings are the container's, not the process's
    write(args, rows, 'cold', {'cold_runs': args.cold_runs, 'restart_runs': args.restart_runs, 'rerun': again, 'cold_check': args.cold_check, 'cold_check_dates': checked})
    return 0


def cmd_interference(args):
    """Does running engines at the same time change what one of them measures? The requests of the target are timed (1) unpinned and alone, the method of the published tables, (2) pinned to
    `--cpus` physical cores of its own and alone, and (3) pinned while `--concurrent` other containers loop the same question on disjoint cores of their own. Per question the median and p95
    of each, and the change of (3) against (2): what the neighbours' memory bandwidth and L3 traffic cost"""
    target, queries = args.target, load_queries(args.queries)
    others = [e for e in args.engines if container(e) != container(target) and e != 'es_cached'][:args.concurrent]
    boxes, cores = [container(target)] + [container(e) for e in others], physical_cores()
    clients = {e: Client(e) for e in [target] + others}
    effective(args)
    gate('interference', [target] + others)
    out = []

    def sample(stmt):
        for _ in range(args.warmups):
            clients[target].run(stmt)
        return [clients[target].run(stmt)[0] for _ in range(args.samples)]

    stats = lambda xs: dict(median=statistics.median(xs), p95=percentile(xs, 95), n=len(xs))
    for qi, q in enumerate(queries):
        stmts = {e: q.statement(e, args.rows) for e in clients}
        for e in clients:
            clients[e].run(stmts[e], True)
        for b in boxes:
            pin(b, None)
        free = stats(sample(stmts[target]))
        for b, s in zip(boxes, benchlib.cpuset_plan(cores, args.cpus, len(boxes), qi)):   # the target's cores move from question to question
            pin(b, s)
        stop = threading.Event()
        loops = [threading.Thread(target=lambda e=e: [clients[e].run(stmts[e]) for _ in iter(stop.is_set, True)]) for e in others]
        try:
            alone = stats(sample(stmts[target]))
            for t in loops:
                t.start()
            time.sleep(0.5)
            together = stats(sample(stmts[target]))
        finally:
            stop.set()
            for t in loops:
                if t.is_alive():
                    t.join()
            for b in boxes:
                pin(b, None)
        out.append(dict(question=q.id, title=q.title, unpinned=free, pinned=alone, concurrent=together, median_change=together['median'] / alone['median'] - 1, p95_change=together['p95'] / alone['p95'] - 1,
                        pinning_change=alone['median'] / free['median'] - 1))
        log(f'{q.id} {q.name}: {target} unpinned {free["median"]:.3g} ms, pinned {alone["median"]:.3g} ms, with {len(others)} others {together["median"]:.3g} ms '
            f'(median {out[-1]["median_change"]:+.1%}, p95 {out[-1]["p95_change"]:+.1%})')
    worst = max(abs(r['median_change']) for r in out)
    log(f'{target} with {" ".join(others)} running at the same time on cores of their own: the median changed by at most {worst:.1%}, the p95 by at most {max(abs(r["p95_change"]) for r in out):.1%}')
    benchlib.write_json(os.path.join(args.out, f'interference-{target}-{args.tier}.json'), dict(target=target, others=others, cpus=args.cpus, rows=args.rows, samples=args.samples, cells=out,
                                                                                            date=benchlib.now_iso(), host=host_fp(), threshold=args.threshold, ok=worst <= args.threshold))
    return 0 if worst <= args.threshold else 1


# --- updates, deletes and what they cost the readers (the `updates` command)

UPDATE_SET = "status = 'refunded', amount = amount - 1"
PHASES = [('update 1%', 'update', 'id % 100 = 0', lambda i: i % 100 == 0), ('update 10%', 'update', 'id % 10 = 1', lambda i: i % 10 == 1),
          ('delete 5%', 'delete', 'id % 20 = 3', lambda i: i % 20 == 3)]
PROBE_ID = lambda k: 20 * k + 5          # untouched by the phases (id % 20 = 5): its status becomes 'probe'
DIGEST = {'pg': "SELECT status, count(*), sum(amount) FROM orders GROUP BY status ORDER BY 1",
          'ch': "SELECT status, count(), sum(amount) FROM store.orders GROUP BY status ORDER BY 1"}


class Writer:
    """one connection of an engine that changes rows: the update path its documentation recommends for the job"""
    def __init__(self, eid):
        self.eid, self.c = eid, Client(eid)
        self.t = CH_TABLE.get(eid)

    def close(self):
        self.c.close()

    def bulk(self, kind, ids):
        """one batch of rows by primary key"""
        lst = ','.join(map(str, ids))
        if self.eid in PG_ENGINES:
            self.c.psql.run(f"UPDATE orders SET {UPDATE_SET} WHERE id IN ({lst})" if kind == 'update' else f"DELETE FROM orders WHERE id IN ({lst})")
        elif self.eid == 'ch':
            self.c.http.request('POST', '/?database=store', (f"UPDATE {self.t} SET {UPDATE_SET} WHERE id IN ({lst})" if kind == 'update' else f"DELETE FROM {self.t} WHERE id IN ({lst})").encode())
        elif self.eid == 'ch_rmt':
            self.c.http.request('POST', '/?database=store', self.rmt_insert(f"id IN ({lst})", "'refunded'", 'amount - 1', 0) if kind == 'update' else self.rmt_insert(f"id IN ({lst})", 'status', 'amount', 1))
        elif self.eid == 'es':
            action = lambda i: ('{"update":{"_id":"%d"}}\n{"script":{"source":"ctx._source.status=\'refunded\'; ctx._source.amount-=1"}}\n' if kind == 'update' else '{"delete":{"_id":"%d"}}\n') % i
            r = json.loads(self.c.http.request('POST', '/orders/_bulk', ''.join(action(i) for i in ids).encode(), {'Content-Type': 'application/x-ndjson'}))
            if r['errors']:
                raise RuntimeError('bulk: ' + json.dumps(next(i for i in r['items'] if list(i.values())[0].get('error')))[:300])
        else:
            raise RuntimeError(f'{self.eid}: rows are changed by predicate')

    def rmt_insert(self, where, status, amount, deleted):
        """a new version of the rows, inserted: ReplacingMergeTree replaces the old one when the parts merge, FINAL hides it until then"""
        return (f"INSERT INTO {self.t} SELECT id, customer_id, country, category, {status}, {amount}, quantity, created_at, title, toUnixTimestamp64Micro(now64(6)), {deleted} "
                f"FROM {self.t} WHERE {where}").encode()

    def predicate(self, kind, pred):
        """ClickHouse mutation: one ALTER for all the rows of the predicate, waited for"""
        sql = f"ALTER TABLE {self.t} UPDATE {UPDATE_SET} WHERE {pred} SETTINGS mutations_sync = 1" if kind == 'update' else f"ALTER TABLE {self.t} DELETE WHERE {pred} SETTINGS mutations_sync = 1"
        self.c.http.request('POST', '/?database=store', sql.encode())

    def digest(self):
        if self.eid in PG_ENGINES:
            return '\n'.join(self.c.psql.run(DIGEST['pg'], True)) + '\n'
        if is_ch(self.eid):
            return self.c.http.request('POST', '/?database=store&default_format=TSV', DIGEST['ch'].replace('store.orders', self.t + (' FINAL' if self.eid == 'ch_rmt' else '')).encode()).decode()
        self.c.http.request('POST', '/orders/_refresh')
        body = {"size": 0, "aggs": {"s": {"terms": {"field": "status", "size": 20, "order": {"_key": "asc"}}, "aggs": {"a": {"sum": {"field": "amount"}}}}}}
        return es_text(self.c.http.request('POST', '/orders/_search?request_cache=false', json.dumps(body).encode()), 'aggregations.s.buckets -> key doc_count a.value')

    def probe_set(self, i, mode):
        """set one row's status to 'probe' the way the engine does a single-row change; returns when the engine acknowledged"""
        if self.eid in PG_ENGINES:
            self.c.psql.run(f"UPDATE orders SET status = 'probe' WHERE id = {i}")
        elif self.eid == 'es':
            self.c.http.request('POST', f'/orders/_update/{i}' + ('?refresh=wait_for' if mode == 'wait_for' else ''), b'{"doc": {"status": "probe"}}')
        elif self.eid == 'ch':
            self.c.http.request('POST', '/?database=store', f"UPDATE {self.t} SET status = 'probe' WHERE id = {i}".encode())
        elif self.eid == 'ch_rmt':
            self.c.http.request('POST', '/?database=store', self.rmt_insert(f'id = {i}', "'probe'", 'amount', 0))
        else:
            self.c.http.request('POST', '/?database=store', f"ALTER TABLE {self.t} UPDATE status = 'probe' WHERE id = {i} SETTINGS mutations_sync = 0".encode())

    def probe_count(self):
        if self.eid in PG_ENGINES:
            return int(self.c.psql.run("SELECT count(*) FROM orders WHERE status = 'probe'", True)[0])
        if is_ch(self.eid):
            return int(self.c.http.request('POST', '/?database=store&default_format=TSV', f"SELECT count() FROM {self.t}{' FINAL' if self.eid == 'ch_rmt' else ''} WHERE status = 'probe'".encode()))
        body = {"size": 0, "track_total_hits": True, "query": {"term": {"status": "probe"}}}
        return json.loads(self.c.http.request('POST', '/orders/_search?request_cache=false', json.dumps(body).encode()))['hits']['total']['value']

    def maintain(self):
        if self.eid in PG_ENGINES:
            self.c.psql.run('VACUUM (ANALYZE) orders')
        elif is_ch(self.eid):
            self.c.http.request('POST', '/?database=store', f'OPTIMIZE TABLE {self.t} FINAL'.encode())
        else:
            self.c.http.request('POST', '/orders/_forcemerge?max_num_segments=1')
            self.c.http.request('POST', '/orders/_flush')

    def free_mib(self):
        """sift_table: the free blocks inside the table file (None for the others)"""
        if self.eid != 'sift_table':
            return None
        return mib(int(self.c.psql.run("SELECT free_blocks FROM sift.table_space('orders')", True)[0]) * 8192)

    def size_mib(self):
        if self.eid in PG_ENGINES:
            self.c.psql.run('CHECKPOINT')
            return mib(self.c.psql.run("SELECT pg_total_relation_size('orders')", True)[0])
        if is_ch(self.eid):
            t = self.t.split('.')[1]
            return mib(self.c.http.request('POST', '/', f"SELECT sum(bytes_on_disk) FROM system.parts WHERE database = 'store' AND table = '{t}' AND active".encode()).decode())
        self.c.http.request('POST', '/orders/_refresh')
        return mib(json.loads(self.c.http.request('GET', '/orders/_stats/store'))['_all']['primaries']['store']['size_in_bytes'])


def prepare_copies(engines):
    """the tables the other ClickHouse update paths change: copies of the loaded one (the lightweight UPDATE engine changes the original)"""
    h = Http('ch', CH_AUTH)
    sql = []
    if 'ch_mut' in engines:
        sql += ['DROP TABLE IF EXISTS store.orders_mut', 'CREATE TABLE store.orders_mut AS store.orders', 'INSERT INTO store.orders_mut SELECT * FROM store.orders', 'OPTIMIZE TABLE store.orders_mut FINAL']
    if 'ch_rmt' in engines:
        sql += ['DROP TABLE IF EXISTS store.orders_rmt'] + sections(read('ch.sql'))['rmt'].rstrip(';').split(';\n') + [
            'INSERT INTO store.orders_rmt SELECT *, 0, 0 FROM store.orders', 'OPTIMIZE TABLE store.orders_rmt FINAL']
    for s in sql:
        h.request('POST', '/', s.encode())
    h.close()


def percentile(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, math.ceil(p / 100 * len(xs)) - 1)]


def read_latency(w, q, args, rows, variant):
    c, st = w.c, q.statement(w.eid, args.rows)
    for _ in range(args.warmups):
        c.run(st)
    rows.add(w.eid, q.id, variant, 'latency_ms', 'ms', st=summary([c.run(st)[0] for _ in range(args.runs)]))


def churn(eid, n, rate, seconds, stop, out):
    """the update load: batches of 100 rows (ids drawn from the live rows, amount + 1) paced to `rate` rows a second, as fast as the engine takes them when it cannot keep up"""
    import random
    rnd, w, batch = random.Random(rate), Writer(eid), 100
    done = errors = 0
    first = None
    t0 = time.perf_counter()
    try:
        while not stop.is_set():
            ids = []
            while len(ids) < batch:
                i = rnd.randrange(1, n + 1)
                if i % 20 != 3:
                    ids.append(i)
            try:
                lst = ','.join(map(str, ids))
                if eid in PG_ENGINES:
                    w.c.psql.run(f"UPDATE orders SET amount = amount + 1 WHERE id IN ({lst})")
                elif eid == 'ch':
                    w.c.http.request('POST', '/?database=store', f"UPDATE store.orders SET amount = amount + 1 WHERE id IN ({lst})".encode())
                elif eid == 'ch_rmt':
                    w.c.http.request('POST', '/?database=store', w.rmt_insert(f'id IN ({lst})', 'status', 'amount + 1', 0))
                else:
                    r = json.loads(w.c.http.request('POST', '/orders/_bulk', ''.join('{"update":{"_id":"%d"}}\n{"script":{"source":"ctx._source.amount+=1"}}\n' % i for i in ids).encode(),
                                                    {'Content-Type': 'application/x-ndjson'}))
                    if r['errors']:
                        raise RuntimeError('bulk errors')
                done += batch
            except Exception as x:
                errors += 1
                first = first or str(x)[:200]
                time.sleep(0.05)
            lag = t0 + (done / rate) - time.perf_counter() if rate else 0
            if lag > 0:
                stop.wait(lag)
    finally:
        out.update(done=done, errors=errors, seconds=time.perf_counter() - t0, first_error=first)
        w.close()


def with_load(w, q, args, rows, rate):
    import threading as th
    stop, out = th.Event(), {}
    t = th.Thread(target=churn, args=(w.eid, args.rows, rate, args.load_seconds, stop, out), daemon=True) if rate else None
    if t:
        t.start()
        time.sleep(1)
    st, lat, fail = q.statement(w.eid, args.rows), [], None
    t0 = time.perf_counter()
    try:
        while time.perf_counter() - t0 < args.load_seconds:
            lat.append(w.c.run(st)[0])
    except Exception as x:   # a reader that fails is a result: kept, the engine's other levels still run
        fail = str(x)[:300]
    finally:
        if t:
            stop.set()
            t.join()
    v = f'load {rate}/s'
    if fail:
        rows.add(w.eid, q.id, v, 'read_error', 'text', value=fail)
        rows.add(w.eid, q.id, v, 'server_log', 'text', value=server_log(w.eid))
        log(f'{w.eid} load {rate}/s: the query failed after {len(lat)} runs: {fail}')
    if not lat:
        return
    rows.add(w.eid, q.id, v, 'latency_ms', 'ms', value=percentile(lat, 50))
    rows.add(w.eid, q.id, v, 'latency_p95_ms', 'ms', value=percentile(lat, 95))
    rows.add(w.eid, q.id, v, 'queries', 'queries', value=len(lat))
    if t:
        rows.add(w.eid, '-', v, 'updates_rows_s', 'rows/s', value=round(out['done'] / out['seconds']))
        rows.add(w.eid, '-', v, 'update_errors', 'batches', value=out['errors'])
        if out['first_error']:
            rows.add(w.eid, '-', v, 'update_error', 'text', value=out['first_error'])
    log(f'{w.eid} load {rate}/s: query p50 {percentile(lat, 50):.3g} ms p95 {percentile(lat, 95):.3g} ms' + (f', {out["done"] / out["seconds"]:.0f} rows/s, {out["errors"]} failed batches' if t else ''))


def read_after_write(w, args, rows):
    modes = {'pg': ['commit'], 'sift': ['commit'], 'sift_table': ['commit'], 'es': ['default', 'wait_for'], 'ch': ['lightweight'], 'ch_mut': ['mutation'], 'ch_rmt': ['insert']}[w.eid]
    k0 = 0
    for mode in modes:
        ack, vis = [], []
        for _ in range(args.probes):
            k0 += 1
            t0 = time.perf_counter()
            w.probe_set(PROBE_ID(k0), mode)
            t1 = time.perf_counter()
            while w.probe_count() < k0:
                time.sleep(0.002)
            vis.append((time.perf_counter() - t1) * 1e3)
            ack.append((t1 - t0) * 1e3)
        rows.add(w.eid, '-', mode, 'ack_ms', 'ms', st=summary(ack))
        rows.add(w.eid, '-', mode, 'visible_ms', 'ms', st=summary(vis))
        log(f'{w.eid} {mode}: acknowledged in {sorted(ack)[len(ack) // 2]:.3g} ms, visible to a query {sorted(vis)[len(vis) // 2]:.3g} ms later')


def update_engine(eid, args, rows, ref):
    w = Writer(eid)
    reference = eid in args.untimed   # PostgreSQL when its numbers are read from the cache: it changes the rows (the state the others are compared with) and times nothing
    qs = {q.id: q for q in load_queries(LOAD_QUERIES)}
    n = args.rows
    for q in [] if reference else qs.values():
        read_latency(w, q, args, rows, 'baseline')
    point = lambda v: (rows.add(eid, '-', v, 'size_mib', 'MiB', value=w.size_mib()), w.free_mib() is not None and rows.add(eid, '-', v, 'free_mib', 'MiB', value=w.free_mib()))
    if not reference:
        point('before')
    for name, kind, pred, test in PHASES:
        ids = [i for i in range(1, n + 1) if test(i)]
        t0 = time.perf_counter()
        if eid == 'ch_mut':
            w.predicate(kind, pred)
        else:
            for k in range(0, len(ids), 1000):
                w.bulk(kind, ids[k:k + 1000])
        dt = time.perf_counter() - t0
        rows.add(eid, '-', name, 'phase_s', 's', value=round(dt, 2))
        rows.add(eid, '-', name, 'phase_rows_s', 'rows/s', value=round(len(ids) / dt))
        d = w.digest()
        if eid == 'pg':
            ref[name] = d
            verdict = 'reference'
        else:
            diff = difference(ref[name], d, [0], TOL)
            verdict = 'ok' if diff is None else 'DIFF: ' + diff
        rows.add(eid, 'digest', name, 'result', 'check', value=verdict)
        log(f'{eid} {name}: {len(ids)} rows in {dt:.2f} s ({len(ids) / dt:.0f} rows/s), state {verdict}')
    if reference:
        w.close()
        return
    read_after_write(w, args, rows)
    if eid != 'ch_mut':
        for rate in args.rates:
            with_load(w, qs[LOAD_QUERY], args, rows, rate)
    point('after churn')
    for q in qs.values():
        read_latency(w, q, args, rows, 'after churn')
    t0 = time.perf_counter()
    w.maintain()
    rows.add(eid, '-', 'maintenance', 'maintain_s', 's', value=round(time.perf_counter() - t0, 1))
    point('after maintenance')
    for q in qs.values():
        read_latency(w, q, args, rows, 'after maintenance')
    w.close()


def cmd_updates(args):
    rows, ref = Rows(args.tier), {}
    problems = []
    args.threads = effective(args)
    for c in {LANES[e][1] for e in args.engines}:   # the rows are about to change: what these containers hold is no longer what a load of the same key leaves
        for f in glob.glob(marker_path(args.data, c, '*')):
            os.remove(f)
    gate('updates', args.engines)
    prepare_copies(args.engines)
    try:
        for eid in [e for e in UPDATE_ENGINES if e in args.engines]:
            log(f'updates on {eid}')
            try:
                with Window() as w:
                    update_engine(eid, args, rows, ref)
                why = w.record(rows, '-', f'updates-{eid}', w.judge({container(eid)}, io=False))   # its own writes are the disk traffic; the CPU of the others is judged
                if why:
                    NOISY.append(f'updates {eid}: {why}')
            except Exception as x:   # recorded, and the other engines still run
                rows.add(eid, '-', 'run', 'error', 'text', value=str(x)[:300])
                rows.add(eid, '-', 'run', 'server_log', 'text', value=server_log(eid))
                log(f'{eid}: {x}')
    finally:
        write(args, rows, 'updates', {'load_seconds': args.load_seconds, 'probes': args.probes})
    problems = [r for r in rows.rows if r['metric'] == 'result' and str(r['value']).startswith('DIFF')]
    for r in problems:
        print(f'DIFFERENT STATE: {r["engine"]} {r["variant"]}: {r["value"]}', file=sys.stderr)
    return 3 if problems else 0


def cmd_verify(args):
    """the thread budget of every server, before anything is loaded or timed (store.sh, after the containers start or change budget)"""
    th = effective(args, plan=False)
    log(f'thread budget {args.cpus} in effect: ' + json.dumps(th))
    return 0


def host_fp():
    return benchlib.host_fingerprint(docker_version=docker('version', '-f', '{{.Server.Version}}').strip())


def host():
    """the fingerprint of this host (steps/benchlib.py) and the budgets of the run"""
    return dict(host_fp(), cpus=os.environ.get('STORE_CPUS', '?'), cpus_list=os.environ.get('STORE_CPUS_LIST', '?'), memory=os.environ.get('STORE_MEMORY', '?'))


# --- the cache of the cells of the engines that are not under test (steps/benchlib.py has the rules)

def make_cache(args):
    return None if args.no_cache else benchlib.Cache(args.cache_dir, args.fingerprint, args.cache_max_age)


def image_ids(group):
    """(tag, image id) of the image of a server; pulled when it is not there, so that the key does not change with the first pull"""
    tag = os.environ[{'pg': 'STORE_PG_IMAGE', 'citus': 'STORE_CITUS_IMAGE', 'es': 'STORE_ES_IMAGE', 'ch': 'STORE_CH_IMAGE'}[group]]
    if subprocess.run(['docker', 'image', 'inspect', tag], capture_output=True).returncode:
        docker('pull', '-q', tag)
    return tag, docker('image', 'inspect', '-f', '{{.Id}}', tag).strip()


def dataset_id(args):
    """the seeded data is identified by what generates it: the rows, the generator and the image of the server it runs on"""
    return dict(rows=args.rows, generator=benchlib.key_of(read('gen.sql'), 12), image=image_ids('pg')[1])


MEASURE_CODE = None   # set in main(): the functions whose behaviour a cached number depends on


def group_inputs(group, lanes, args):
    """everything a cached cell of the group depends on: the key of its entry is the hash of this. Not the pg_sift lanes (they are measured every pass)"""
    tag, iid = image_ids(group)
    qs = load_queries(args.queries)
    method = dict(runs=args.runs, warmups=args.warmups, cold_runs=args.cold_runs, restart_runs=args.restart_runs, cold_check=args.cold_check, parts=args.parts, points=args.points, load_cpus=args.cpus, parallel=args.parallel)
    if 'updates' in args.parts:
        method['updates'] = dict(load_seconds=args.load_seconds, probes=args.probes, rates=args.rates, phases=[p[0] for p in PHASES], set=UPDATE_SET)
    files = sorted({DDL[e] for e in lanes if e in DDL} | {{'ch': 'ch.sql', 'es': 'es.json'}.get(group, '')} - {''})
    return dict(group=group, lanes=lanes, image=tag, image_id=iid, dataset=dataset_id(args), memory=os.environ.get('STORE_MEMORY', ''),
                server_flags=os.environ.get('STORE_PG_FLAGS', '') if group in ('pg', 'citus') else '', statements={e: benchlib.key_of([q.statement(e, args.rows, e == 'es_cached') for q in qs], 12) for e in lanes},
                files={f: benchlib.key_of(read(f), 12) for f in files}, code=MEASURE_CODE, method=method)


def cmd_plan(args):
    """which lanes this pass measures and which it reads from the cache (--remeasure-all: none), written to plan.json; the lanes that are measured include PostgreSQL as the (untimed)
    reference of the result checks. Prints the decision of every server"""
    cache, hits, groups = make_cache(args), {}, {}
    every = list(dict.fromkeys(args.engines + args.update_engines))
    for g in dict.fromkeys(benchlib.group_of(e) for e in every):
        lanes = [e for e in every if benchlib.group_of(e) == g]
        why = ('pg_sift: measured every pass' if not benchlib.cacheable(g) else 'the cache is off (--remeasure-all)' if args.remeasure_all or not cache else 'named in --remeasure' if g in args.remeasure
               else 'compression variants: sizes are measured' if args.compression else '')
        inputs = group_inputs(g, lanes, args) if benchlib.cacheable(g) else {}
        entry = None
        if not why:
            entry, why = cache.lookup('cells', g, inputs)
        groups[g] = dict(lanes=lanes, status='cached' if entry else 'measure', inputs=inputs, path=cache.path('cells', g, inputs) if entry else None)
        if entry:
            hits[g] = entry
            log(f'plan {g}: read from the cache, measured {entry["date"]}, key {entry["key"][:8]}, host {entry["fp"]}')
        else:
            log(f'plan {g}: measured in this pass ({why})')
    run, untimed = benchlib.plan_engines(args.engines, hits)
    upd, _ = benchlib.plan_engines(args.update_engines, hits)
    benchlib.write_json(args.plan, dict(groups=groups, run=run, untimed=untimed, update=upd, fingerprint=args.fingerprint))
    log(f'plan: lanes {" ".join(run) or "none"}' + (f' (reference only: {" ".join(untimed)})' if untimed else '') + f'; updates {" ".join(upd) or "none"}; written {args.plan}')
    return 0


def cmd_planget(args):
    """one field of plan.json as a comma separated list (run, untimed, update, cached, measured)"""
    p = json.load(open(args.plan))
    print(','.join({'cached': [g for g, v in p['groups'].items() if v['status'] == 'cached'], 'measured': [g for g, v in p['groups'].items() if v['status'] == 'measure']}.get(args.field) or p.get(args.field, [])))
    return 0


def sub_meta(group, meta):
    """the part of a result set's servers and threads that describes one group's servers"""
    return ({k: v for k, v in meta.get('servers', {}).items() if k == group or k.startswith(group + '_')}, {c: v for c, v in meta.get('threads', {}).items() if c == LANES[group][1]})


def cmd_cache_save(args):
    """put the cells of the cacheable servers this pass measured into the cache (not those with a window that was still NOISY after the second measurement)"""
    plan, merged, cache = json.load(open(args.plan)), json.load(open(args.results)), make_cache(args)
    for g, v in plan['groups'].items():
        if v['status'] != 'measure' or not cache or not benchlib.cacheable(g):
            continue
        rows = [r for r in merged['rows'] if benchlib.group_of(r['engine']) == g]
        if not rows:
            continue
        if any(r['metric'] == 'noisy' for r in rows):
            log(f'cache {g}: not kept, a window was still NOISY after the second measurement')
            continue
        tiers = {}
        for t in sorted({r['tier'] for r in rows}):
            tm = merged['meta']['tiers'][t]
            servers, threads = sub_meta(g, tm)
            tiers[t] = dict(meta={k: x for k, x in tm.items() if k not in ('parts', 'servers', 'threads', 'cached')}, parts=tm['parts'], servers=servers, threads=threads)
        log(f'cache {g}: kept {cache.put("cells", g, v["inputs"], dict(rows=rows, tiers=tiers))}')
    return 0


def restart_cold(args, metas, rows):
    """cold cells of some engines taken from another results file, whose cells restarted the server for every run: for the engines where emptying the memory is not as cold
    (the check of `cold`), the numbers of this run's emptied cells stay as the check (`cold-alt`) of the questions it was made for"""
    d = json.load(open(args.restart_cold))
    engines = args.restart_engines
    for tier in {r['tier'] for r in d['rows'] if r['variant'] == 'cold'} & {t for t, m in metas.items() if 'cold' in m['parts']}:   # the sets this run has cold cells for
        part = d['meta']['tiers'][tier]['parts']['cold']
        src = [r for r in d['rows'] if r['tier'] == tier and r['engine'] in engines and r['variant'] == 'cold' and r['metric'] == 'latency_ms']
        keep = []
        for r in rows:
            if r['tier'] == tier and r['engine'] in engines and r['variant'] == 'cold' and r['metric'] == 'latency_ms':
                if r['class'] in metas[tier].get('cold_check', []):
                    r = dict(r, variant='cold-alt')
                else:
                    continue
            if r['tier'] == tier and r['engine'] == '-' and r['variant'].startswith('cold-') and r['variant'][5:] in engines:
                continue
            if r['tier'] == tier and r['metric'] == 'cold_how' and r['engine'] in engines:
                continue
            keep.append(r)
        rows = keep + src + [dict(dataset='store', tier=tier, engine=e, **{'class': '-'}, variant='cold', metric='cold_how', unit='text', value='restart', min='restart', max='restart', stderr=None, n=1, values=['restart']) for e in engines]
        metas[tier]['restart_cold'] = dict(engines=engines, date=part['date'], runs=d['meta']['tiers'][tier]['cold_runs'], noisy=[n for n in part['noisy'] if n.split()[1] in engines])
    return rows


def cmd_merge(args):
    """the files of the parts of a pass (store-<set>-<part>.json), the entries of the cache its plan read, and the sets of an earlier results file (--keep) that this pass did not
    measure again, into one results file. Cells of another host are never mixed in"""
    metas, rows, fp = {}, [], args.fingerprint
    for path in args.files:
        d = json.load(open(path))
        tier = d['meta']['tier']
        metas.setdefault(tier, {}).update({k: v for k, v in d['meta'].items() if k not in ('date', 'loadavg', 'loadavg_end', 'pressure', 'noisy', 'baseline', 'rerun', 'utilization')})
        metas[tier].setdefault('parts', {})[os.path.basename(path).split('-')[-1][:-5]] = dict(date=d['meta']['date'], loadavg=d['meta']['loadavg'], loadavg_end=d['meta'].get('loadavg_end'), pressure=d['meta'].get('pressure'), noisy=d['meta'].get('noisy', []), baseline=d['meta'].get('baseline'), rerun=d['meta'].get('rerun', []), utilization=d['meta'].get('utilization'))
        metas[tier].pop('loadavg_end', None)
        rows += d['rows']
    entries = [json.load(open(v['path'])) for plan in args.plan for v in json.load(open(plan))['groups'].values() if v['status'] == 'cached']
    benchlib.check_homogeneous(entries, fp)
    for e in entries:   # what was measured in this pass wins; a column read from the cache says when and under which key it was measured
        rows += e['rows']
        for tier, t in e['tiers'].items():
            m = metas.setdefault(tier, dict(t['meta'], parts={}))
            for k in ('servers', 'threads'):
                m[k] = {**t[k], **m.get(k, {})}
            m.setdefault('cached', {})[e['name']] = dict(date=e['date'], key=e['key'], fp=e['fp'], parts={p: x['date'] for p, x in t['parts'].items()})
    for keep in args.keep:
        old = json.load(open(keep))
        if not benchlib.same_host(old['meta'].get('host', {}), fp):
            log(f'WARN {keep} was measured on another host ({old["meta"].get("host", {}).get("cpu", "?")}, {old["meta"].get("host", {}).get("kernel", "?")}): its sets are not carried over')
            continue
        for tier, t in old['meta']['tiers'].items():
            if tier not in metas and not any(t['rows'] == m['rows'] and t.get('cpus') == m.get('cpus') for m in metas.values()):
                metas[tier] = t
                rows += [r for r in old['rows'] if r['tier'] == tier]
    if args.restart_cold:
        rows = restart_cold(args, metas, rows)
    with open(args.o, 'w') as f:
        f.write('{"meta": ' + json.dumps(dict(tiers=metas, host=host())) + ',\n "rows": [\n' + ',\n'.join(' ' + json.dumps(r) for r in rows) + '\n]}\n')
    log(f'written: {args.o}')
    return 0


def noisy_cells(meta):
    """the cells a part file reports as still NOISY: ('warm', question) / ('cold', engine, question) / ('updates', engine), from the `noisy` list of its meta"""
    out = []
    for s in meta.get('noisy', []):
        m = re.match(r'(?:(cold) (\w+) (\d+)|(updates) (\w+)|(\d+)): ', s)
        if m:
            out.append(('cold', m[2], m[3]) if m[1] else ('updates', m[5]) if m[4] else ('warm', m[6]))
    return out


def cmd_noisy(args):
    """the cells of a part file that are still NOISY, one per line: `warm QUESTION`, `cold ENGINE QUESTION`, `updates ENGINE` (bench/bench final measures them again)"""
    for c in noisy_cells(json.load(open(args.file))['meta']):
        print(' '.join(c))
    return 0


def cmd_patch(args):
    """replace in a part file the cells a second file measured again (same sets, engines and questions), and drop them from its list of NOISY cells"""
    old, new = json.load(open(args.file)), json.load(open(args.fresh))

    def measured_again(s):   # a NOISY entry of the old file whose cell the new file holds
        c = noisy_cells({'noisy': [s]})
        return bool(c) and any(r['class'] == c[0][1] if c[0][0] == 'warm' else (r['engine'], r['class']) == c[0][1:] for r in new['rows'] if c[0][0] != 'updates')

    old['rows'] = benchlib.replace_rows(old['rows'], new['rows'])
    old['meta']['noisy'] = [s for s in old['meta'].get('noisy', []) if not measured_again(s)] + new['meta'].get('noisy', [])
    old['meta']['rerun'] = sorted(set(old['meta'].get('rerun', [])) | set(new['meta'].get('rerun', [])) | {'patched'})
    benchlib.write_json(args.file, old)
    log(f'patched {args.file} with {args.fresh}')
    return 0


HEADLINE = [('sift', 'pg'), ('sift', 'citus'), ('sift', 'es'), ('sift', 'ch'), ('sift_table', 'pg')]   # the claims of the README: pg_sift against each system


def warm_by_cpus(path):
    """{cpus: {(engine, question): warm median}} of a results file"""
    R = Results(path)
    return {R.cpus(t): {(e, q): R.num(t, e, q, 'warm', 'latency_ms') for e in all_lanes(R, t) if e != 'es_cached' for q in R.queries(t)} for t in R.all_tiers}


def cmd_stable(args):
    """do two results files (two sizes) tell the same story? Per thread budget both have: the ratios of pg_sift to each system on every question must stay within a factor of
    1 + --tol, and no pair separated by more than --sep in both may change order. Exit status 1 when not (the ladder of presets is checked with this)"""
    a, b, bad = warm_by_cpus(args.a), warm_by_cpus(args.b), []
    for c in sorted(set(a) & set(b)):
        r = benchlib.stable({k: v for k, v in a[c].items() if v}, {k: v for k, v in b[c].items() if v}, args.tol, args.sep, HEADLINE)
        print(f'\n{c} CPUs: {"stable" if r["ok"] else "NOT stable"} (ratios within {1 + args.tol:g}x, order flips beyond {args.sep:g}x counted)')
        for q, i in r['queries'].items():
            print(f'  {q} {QUERY_TITLES.get(q, "")[:40]:40} pairs {i["pairs"]} flips {i["flips"]} worst drift {i["worst"][0]:.2f}x ({i["worst"][1]}/{i["worst"][2]})')
        bad += r['violations']
    return 1 if bad or not set(a) & set(b) else 0


# --- the Markdown tables


# --- the tables of the update benchmark


# --- the markers of README.md and docs/BENCHMARKS.md

# --- compression


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
    r.add_argument('--results', default=os.path.join(HERE, '..', '..', 'bench', 'results', 'store.json'))
    r.add_argument('--readme')
    r.add_argument('--benchmarks')
    r.add_argument('--compression', default=os.path.join(HERE, '..', '..', 'bench', 'results', 'store-compression.json'))
    r.add_argument('--stdout', action='store_true')
    for name in ('table', 'gap'):
        sub.add_parser(name).add_argument('file', nargs='?', default=os.path.join(HERE, '..', '..', 'bench', 'results', 'store.json'))
    t = sub.add_parser('stats')
    t.add_argument('--rows', type=int, required=True)
    t.add_argument('-o', required=True)
    t.add_argument('--runs', type=int, default=3)
    t.add_argument('--queries', type=lambda s: s.split(','), default=[])
    c = sub.add_parser('compare')
    c.add_argument('a')
    c.add_argument('b')
    args = ap.parse_args()
    global MEASURE_CODE, LOAD_CODE
    LOAD_CODE = benchlib.key_of([inspect.getsource(f) for f in (load_pg, build_indexes, copy_chunks, load_ch, load_es, bulk_docs)], 12)
    MEASURE_CODE = benchlib.key_of([LOAD_CODE] + [inspect.getsource(f) for f in (run_query, time_cell, time_wave, one_container, cold_cell, evict, restart, update_engine, with_load, churn, read_latency,
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
