"""The orders benchmark's update load: the writer, reads under a load, read-after-write, space (steps/store.py updates, verify)."""
import glob
import json
import os
import sys
import time
from store_common import difference, marker_path, percentile, write
from store_data import Rows, effective, mib
from stats import summary
from store_engine import Client, Http, Window, es_text, gate, server_log
from store_lib import CH_AUTH, CH_TABLE, LANES, LOAD_QUERIES, LOAD_QUERY, MODES, NOISY, PG_ENGINES, TOL, UPDATE_ENGINES, container, is_ch, load_queries, log, read, sections


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

    def lsn(self):
        return self.c.psql.run('SELECT pg_current_wal_lsn()', True)[0] if self.eid in PG_ENGINES else None

    def wal_mib(self, since):
        """the WAL written since `since` (a value of lsn()), MiB; None for the systems that are not PostgreSQL"""
        return None if since is None else mib(int(self.c.psql.run(f"SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '{since}')", True)[0]))

    def free_mib(self):
        """sift_table: the free blocks inside the table file (None for the others)"""
        if self.eid not in ('sift_table', 'sift_table_maint'):
            return None
        return mib(int(self.c.psql.run("SELECT free_blocks FROM sift.table_space('orders')", True)[0]) * 8192)

    def table_parts(self):
        """PostgreSQL servers: (MiB of the table, {index: MiB}) read as the load reads them (pg_table_size, pg_relation_size per index); None for the others"""
        if self.eid not in PG_ENGINES:
            return None
        run = lambda sql: self.c.psql.run(sql, True)
        return (mib(run("SELECT pg_table_size('orders')")[0]),
                {n: mib(s) for n, s in (l.split('|') for l in run("SELECT indexrelid::regclass || '|' || pg_relation_size(indexrelid) FROM pg_index WHERE indrelid = 'orders'::regclass ORDER BY 1"))})

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
    modes = {'es': ['default', 'wait_for']}.get(w.eid) or [MODES[w.eid]]
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
    def point(v):
        rows.add(eid, '-', v, 'size_mib', 'MiB', value=w.size_mib())
        free, parts = w.free_mib(), w.table_parts()
        if free is not None:
            rows.add(eid, '-', v, 'free_mib', 'MiB', value=free)
        if parts:
            rows.add(eid, '-', v, 'table_mib', 'MiB', value=parts[0])
            for name, size in parts[1].items():
                rows.add(eid, name, v, 'index_mib', 'MiB', value=size)

    if not reference:
        point('before')
    for name, kind, pred, test in PHASES:
        ids = [i for i in range(1, n + 1) if test(i)]
        lsn = w.lsn()
        t0 = time.perf_counter()
        if eid == 'ch_mut':
            w.predicate(kind, pred)
        else:
            for k in range(0, len(ids), 1000):
                w.bulk(kind, ids[k:k + 1000])
        dt = time.perf_counter() - t0
        rows.add(eid, '-', name, 'phase_s', 's', value=round(dt, 2))
        rows.add(eid, '-', name, 'phase_rows_s', 'rows/s', value=round(len(ids) / dt))
        if lsn:
            rows.add(eid, '-', name, 'wal_mib', 'MiB', value=w.wal_mib(lsn))
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
    lsn = w.lsn()
    t0 = time.perf_counter()
    w.maintain()
    rows.add(eid, '-', 'maintenance', 'maintain_s', 's', value=round(time.perf_counter() - t0, 1))
    if lsn:
        rows.add(eid, '-', 'maintenance', 'wal_mib', 'MiB', value=w.wal_mib(lsn))
    point('after maintenance')
    for q in qs.values():
        read_latency(w, q, args, rows, 'after maintenance')
        if eid in PG_ENGINES:   # what the scan did (units answered from the pre-aggregates, rows scanned and vetted) is kept next to the milliseconds
            rows.add(eid, q.id, 'after maintenance', 'explain', 'text', value=w.c.run('EXPLAIN (ANALYZE, VERBOSE, BUFFERS) ' + q.statement(eid, args.rows), True)[1].strip())
    if eid in PG_ENGINES and eid.startswith('sift') and eid != 'sift_table' and eid != 'sift_table_maint':
        rows.add(eid, '-', 'after maintenance', 'col_segments', 'text', value=w.c.run("SELECT format('%s gen=%s rows=%s units=%s dirty=%s dead_rows=%s format=%s bytes=%s preagg_bytes=%s', kind, gen, rows, units, dirty, dead_rows, format, bytes, preagg_bytes) FROM sift.col_segments('orders_col') ORDER BY gen", True)[1].strip())
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
