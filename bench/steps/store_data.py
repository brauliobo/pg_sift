"""The orders benchmark's data: the generated CSV, the load into every engine, the keys of the data volumes (steps/store.py gen, load, datakey)."""
import benchlib
import csv
import hashlib
import itertools
import json
import os
import queue
import re
import subprocess
import threading
import time
import urllib.parse
from store_common import csv_path, dataset_id, image_ids, marker_path, write
import store_common
from stats import summary
from store_engine import Http, ask, docker, pg_args, psql_file, psql_value, quiet_gate, wait_idle
from store_lib import CHUNK, CH_AUTH, COMPRESSION, DDL, LANES, MAINT, NAME, PG_ENGINES, container, die, is_ch, is_es, log, read, sections


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

    def cost(self, c, engine, cls, variant, stmt):
        """the CPU of the timed requests of the client (c.cpus) and the MB one more request of `stmt` reads (or the reason the engine cannot say)"""
        self.add(engine, cls, variant, 'cpu_ms', 'ms', st=summary(c.cpus))
        mb = c.read_mb(stmt)
        self.add(engine, cls, variant, 'read_mb', 'MB', **({'value': mb} if isinstance(mb, str) else {'st': summary([mb])}))


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
        psql_file(eid, f'DROP DATABASE IF EXISTS {db} WITH (FORCE); CREATE DATABASE {db}' + ''.join(f'; ALTER DATABASE {db} SET {g} = on' for g in MAINT.get(eid, ('', ()))[1]), 'postgres')
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
    if eid in ('sift_table', 'sift_table_maint'):   # reusable blocks left inside the table file after the load's VACUUM
        rows.add(eid, '-', 'free', 'size_mib', 'MiB', value=mib(int(psql_value(eid, "SELECT free_blocks FROM sift.table_space('orders')")) * 8192))
    if eid in ('sift', 'sift_maint') or eid in COMPRESSION:   # what the index holds: the options it was built with and, per leaf, the chunks by encoding and their bytes
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
    for eid in [e for e in ('pg', 'sift', 'sift_table', 'sift_maint', 'sift_table_maint') if e in engines]:
        v[eid] = psql_value(eid, 'SHOW server_version')
        v[eid + '_sift'] = psql_value(eid, "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")
        v[eid + '_settings'] = settings(eid)
        if eid != 'pg':   # the maintenance settings of the lane, read back from the server in its database
            v[eid + '_maintenance'] = psql_value(eid, "SELECT string_agg(replace(name, 'sift.', '') || ' = ' || setting, ', ' ORDER BY name) FROM pg_settings "
                                                      "WHERE name IN ('sift.async_maintenance', 'sift.col_auto_merge', 'sift.table_auto_defrag')")
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


def cmd_datakey(args):
    """one line per container: `CONTAINER KEY VOLUME reuse|new`. The key says what its data volume holds: the seeded rows, the image, the table definitions, the load code and settings;
    `reuse`: a volume of that key exists and the load that filled it finished (store.sh keeps the volume under this name and mounts it, and loads nothing)"""
    images = {c: image_ids(c)[1] for c in ('pg', 'citus', 'es', 'ch')}
    images['sift'] = docker('image', 'inspect', '-f', '{{.Id}}', f'{NAME}-pg:latest').strip()   # PostgreSQL with pg_sift built from this checkout
    for c in args.containers:
        lanes = [e for e in args.engines if LANES[e][1] == c and e != 'es_cached']
        files = sorted({DDL[e] for e in lanes if e in DDL} | {{'ch': 'ch.sql', 'es': 'es.json'}.get(c, '')} - {''})
        key = benchlib.key_of(dict(container=c, lanes=lanes, image=images[c], dataset=dataset_id(args), files={f: benchlib.key_of(read(f), 12) for f in files}, code=store_common.LOAD_CODE,
                                   server_flags=os.environ.get('STORE_PG_FLAGS', '') if c in ('pg', 'sift', 'citus') else '', shards=args.cpus if c == 'es' else 0, compression=args.compression), 12)
        vol = f'{NAME}-data-{c}-{key}'
        have_volume = subprocess.run(['docker', 'volume', 'inspect', vol], capture_output=True).returncode == 0
        print(c, key, vol, 'reuse' if have_volume and os.path.exists(marker_path(args.data, c, key)) else 'new')
    return 0
