"""Library of bench/examples/run-example: registry, example files, engines, load, oracle, queries, writes, concurrency, coverage (README.md)."""
import csv
import hashlib
import json
import os
import re
import statistics
import subprocess
import sys
import threading
import time
import itertools
from collections import Counter, namedtuple

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                       # bench/examples
STEPS = os.path.join(os.path.dirname(ROOT), 'steps')
sys.path[:0] = [STEPS, os.path.join(STEPS, 'records')]
import iolib  # noqa: E402  (the IO of a statement)
import pgq  # noqa: E402
import collect  # noqa: E402  (instructions of the process tree)

SCALES = {'S': 100_000, 'M': 1_000_000, 'L': 10_000_000}
KNOBS = dict(skew='1', hot='0', null_pct='0', card='20', corr='random', seed='7')
ORACLE_OFF = ['SET enable_indexscan = off', 'SET enable_indexonlyscan = off', 'SET enable_bitmapscan = off', 'SET max_parallel_workers_per_gather = 0']
SIFT_OFF = ['SET sift.enable_agg = off', 'SET sift.enable_col_scan = off']
Link = namedtuple('Link', 'child_table child_col parent_table parent_col relation reads')
Query = namedtuple('Query', 'id kind join ordered empty_ok sql alt oracle plan found')
Batch = namedtuple('Batch', 'id pattern rows concurrent maintenance sift_only rounds sql')


def trivial(rows):
    """an oracle that proves little: no rows, or one row of zeros and blanks (a filter that matched nothing)"""
    return not rows or (len(rows) == 1 and all(f in ('0', '') for f in rows[0].split('|')))


def tsv(path):
    with open(path) as f:
        return [r for r in csv.DictReader((l for l in f if not l.startswith('#') and l.strip()), delimiter='\t')]


def split_blocks(text, key):
    """[(header dict, body)] of a file whose blocks start with `-- KEY: name` followed by `-- k: v` lines"""
    out, cur = [], None
    for line in text.splitlines():
        m = re.match(r'--\s*(\w+):\s*(.*)$', line)
        if m and m.group(1) == key:
            cur = ({key: m.group(2).strip()}, [])
            out.append(cur)
        elif cur is not None:
            if m and not cur[1]:
                cur[0][m.group(1)] = m.group(2).strip()
            else:
                cur[1].append(line)
    return [(h, '\n'.join(b).strip()) for h, b in out]


class Example:
    def __init__(self, id):
        self.id, self.dir = id, os.path.join(ROOT, 'examples', id)
        row = next((r for r in tsv(os.path.join(ROOT, 'registry.tsv')) if r['id'] == id), None)
        if row is None:
            raise SystemExit('run-example: %s is not in registry.tsv' % id)
        self.meta = row
        self.queries, self.batches, self.links = self._filtered_queries(), self._batches(), self._links()

    def file(self, name):
        p = os.path.join(self.dir, name)
        return p if os.path.exists(p) else None

    def text(self, name):
        p = self.file(name)
        return open(p).read() if p else ''

    def _filtered_queries(self):
        only = [x for x in os.environ.get('EX_ONLY', '').split(',') if x]
        return [q for q in self._queries() if not only or q.id in only]

    def _queries(self):
        return [Query(h['query'], h['kind'], h['join'], h.get('ordered', 'no') == 'yes', h.get('empty', 'no') == 'ok', b.rstrip(';').strip(), h.get('sift'), h.get('oracle'), h.get('plan'), h.get('found'))
                for h, b in split_blocks(self.text('queries.sql'), 'query')]

    def _batches(self):
        return [Batch(h['batch'], h['pattern'], h.get('rows', 'nb'), h.get('concurrent', 'yes') == 'yes', h.get('maintenance', 'no') == 'yes', h.get('sift_only', 'no') == 'yes', int(h.get('rounds', 0)), b)
                for h, b in split_blocks(self.text('writes.sql'), 'batch')]

    def _links(self):
        p = self.file('links.tsv')
        return [Link(**{k: r[k] for k in Link._fields}) for r in tsv(p)] if p else []

    def digest(self):
        h = hashlib.sha256(json.dumps(self.meta, sort_keys=True).encode())
        for n in sorted(os.listdir(self.dir)):
            h.update(n.encode() + open(os.path.join(self.dir, n), 'rb').read())
        return h.hexdigest()[:12]

    def knobs(self, nf, overrides):
        k = dict(KNOBS)
        for c in ('skew', 'hot', 'null_pct', 'card', 'corr'):
            if self.meta.get(c):
                k[c] = self.meta[c]
        k.update(overrides)
        div, mn = int(self.meta['nd_div']), int(self.meta['nd_min'])
        nd = max(mn, nf // div) if div else mn
        return dict(k, nf=nf, nd=nd, nb=max(nf // 100, 10), s=int(k['seed']) * 1000)


# --- sessions

class Sess(pgq.Backend):
    """one psql backend; run() -> (ms as psql timed it, rows, error, warning), the shape iolib and collect expect"""

    def __init__(self, db, knobs=(), settings=()):
        env = dict(os.environ, LC_ALL='C', PGOPTIONS='-c statement_timeout=900s')
        super().__init__(['psql', '-X', '-qAt', '-F', '|', '-d', db], env, setup=['\\set VERBOSITY terse'], strict=False)
        for k, v in dict(knobs).items():
            self.raw("\\set %s '%s'" % (k, v))
        for st in settings:
            self.must(st)

    def raw(self, line):
        pgq.Backend.run(self, line)

    def run(self, sql):
        lines = pgq.Backend.run(self, '\\timing on\n%s;\n\\timing off' % sql.rstrip().rstrip(';'), capture=True)
        ms = next((float(m.group(1)) for l in reversed(lines) if (m := re.match(r'Time: ([0-9.]+) ms', l))), None)
        err = next((l[6:].strip() for l in lines if l.startswith('ERROR:')), '')
        rows = [l for l in lines if not l.startswith(('Time:', 'ERROR:', 'WARNING:', 'NOTICE:', 'DETAIL:', 'HINT:', 'CONTEXT:'))]
        return ms, rows, err, next((l for l in lines if l.startswith('WARNING:')), '')

    def must(self, sql):
        ms, rows, err, _ = self.run(sql)
        if err:
            raise RuntimeError('%s :: %s' % (err, sql[:100]))
        return rows

    def one(self, sql):
        return self.must(sql)[0]

    def result(self, sql):
        return self.must(sql)


def psql_file(db, path, knobs, extra=''):
    """a file of DDL or generator SQL through psql -f with the knobs as psql variables; any error stops the run"""
    cmd = ['psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', '-d', db, '-f', path] + [a for k, v in knobs.items() for a in ('-v', '%s=%s' % (k, v))]
    p = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, LC_ALL='C'))
    if p.returncode:
        raise SystemExit('run-example: %s failed: %s' % (os.path.basename(path), p.stderr.strip()[:600]))


def psql_sql(db, sql, knobs=None):
    tmp = os.path.join(os.environ.get('EX_TMP', '/dev/shm/ex'), 'stmt-%d.sql' % os.getpid())
    open(tmp, 'w').write(sql)
    try:
        psql_file(db, tmp, knobs or {})
    finally:
        os.unlink(tmp)


def admin(sql):
    p = subprocess.run(['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-d', 'postgres', '-c', sql], capture_output=True, text=True)
    if p.returncode:
        raise SystemExit('run-example: %s' % p.stderr.strip())
    return p.stdout.strip()


# --- engines and the connected-design plug-in interface

Engine = namedtuple('Engine', 'name sift structures design')


def engine_of(name):
    """native: btrees only, no extension. sift-unconnected: sift_col indexes as today, pushdown on. sift-off: the same structures, pushdown and
    sift scans off (what a write pays without a read benefit, and a second oracle). connected:<design>: a plug-in (designs/README.md)"""
    if name == 'native':
        return Engine(name, False, False, None)
    if name in ('sift-unconnected', 'sift-off'):
        return Engine(name, True, True, None)
    if name.startswith('connected:'):
        return Engine(name, True, True, load_design(name.split(':', 1)[1]))
    raise SystemExit('run-example: unknown engine %s (native, sift-unconnected, sift-off, connected:<design>)' % name)


def load_design(name):
    sys.path.insert(0, os.path.join(ROOT, 'designs'))
    import importlib
    try:
        return importlib.import_module(name.replace('-', '_'))
    except ImportError:
        raise SystemExit('run-example: no design %s (bench/examples/designs/%s.py)' % (name, name.replace('-', '_')))


def structure_sql(ex, eng):
    """the DDL lists of an engine after the data is loaded: [(step name, file or sql text)]"""
    steps = [('btree', ex.file('btree.sql'))]
    if eng.sift:
        steps.append(('sift', ex.file('sift.sql')))
    if eng.design:
        steps.append(('connected', eng.design.ddl(ex)))
    return [(n, s) for n, s in steps if s]


def session_settings(eng, oracle=False):
    s = []
    if eng.sift:
        s.append("LOAD 'pg_sift'")
    if oracle:
        s += ORACLE_OFF + (SIFT_OFF if eng.sift else [])
    elif eng.name == 'sift-off':
        s += SIFT_OFF
    elif eng.design:
        s += list(eng.design.session_sql())
    if not oracle:
        s += [x for x in os.environ.get('EX_SETTINGS', '').split(';') if x.strip()]
    return s


# --- load

class Run:
    """one (example, scale, engine) database: load, then the phases"""

    def __init__(self, ex, scale, eng, overrides, out, keep=False, quick=False):
        self.ex, self.scale, self.eng, self.out, self.keep = ex, scale, eng, out, keep
        self.nf = SCALES.get(scale) or int(scale)
        self.k = ex.knobs(self.nf, overrides)
        self.k['table_am'] = overrides.get('table_am', 'sift_table' if eng.sift else 'heap')
        self.db = re.sub(r'[^a-z0-9]', '_', ('ex_%s_%s_%s' % (ex.id, scale, eng.name)).lower())[:60]
        self.round = itertools.count(1)
        self.lock = threading.Lock()
        self.loadrows, self.queryrows, self.writerows, self.concrows, self.refused = [], [], [], [], []

    def next_round(self):
        with self.lock:
            return next(self.round)

    def session(self, oracle=False, settings=()):
        return Sess(self.db, self.k, session_settings(self.eng, oracle) + list(settings))

    def load(self):
        ex, eng = self.ex, self.eng
        admin('DROP DATABASE IF EXISTS %s WITH (FORCE)' % self.db)
        admin('CREATE DATABASE %s' % self.db)
        if eng.sift:
            psql_sql(self.db, 'CREATE EXTENSION pg_sift')
        psql_file(self.db, os.path.join(ROOT, 'lib', 'gx.sql'), {})
        psql_sql(self.db, "CREATE TABLE gx.knob AS SELECT :nf::bigint AS nf, :nd::int AS nd, :nb::int AS nb, :s::int AS s, :skew::float8 AS skew, "
                          ":hot::float8 AS hot, :null_pct::float8 AS null_pct, :card::int AS card, :'corr'::text AS corr", self.k)
        psql_file(self.db, ex.file('schema.sql'), self.k)
        t = time.perf_counter()
        psql_file(self.db, ex.file('gen.sql'), self.k)
        self.step('gen', t)
        for name, sql in structure_sql(ex, eng):
            t = time.perf_counter()
            if os.path.exists(sql):
                psql_file(self.db, sql, self.k)
            else:
                psql_sql(self.db, sql, self.k)
            self.step(name, t)
        t = time.perf_counter()
        psql_sql(self.db, 'VACUUM (ANALYZE)')
        self.step('vacuum_analyze', t)
        s = self.session()
        self.tables = s.result("SELECT c.relname FROM pg_class c WHERE c.relnamespace = 'public'::regnamespace AND c.relkind IN ('r', 'm') ORDER BY 1")
        self.facts = {t: int(s.one('SELECT count(*) FROM public.%s' % t)) for t in self.tables}
        for r in s.result("SELECT relname, relpages, relallvisible FROM pg_class WHERE relnamespace = 'public'::regnamespace AND relkind = 'r' AND relpages > 0"):
            name, pages, vis = r.split('|')
            if pages != vis:
                print('run-example: warning: %s relallvisible %s of %s pages' % (name, vis, pages), file=sys.stderr)
        self.loadrows.append(dict(step='rows', ms='', detail=json.dumps(self.facts)))
        for r in self.index_sizes(s):
            self.loadrows.append(dict(step='index', ms='', detail=r))
        if self.eng.name.startswith('connected'):
            before = dict(r.split('|') for r in s.result("SELECT format('%s|%s', child || fk, fill) FROM sift.col_connections"))
            for child in s.result('SELECT DISTINCT child FROM sift.col_connections'):
                s.must("SELECT sift.col_maintain('%s'::regclass)" % child)
            for r in s.result("SELECT format('%s|%s|%s|%s|%s|%s', child, fk, parent, declaration, stamp, fill) FROM sift.col_connections ORDER BY 1"):
                child, fk, parent, decl, stamp, fill = r.split('|')
                keys = int(s.one('SELECT count(*) FROM %s' % re.search(r'-> (\w+)\(', decl).group(1)))
                self.loadrows.append(dict(step='connection', ms='', detail='%s|%d|%s|%s' % (r, keys, s.one('SHOW maintenance_work_mem'), before[child + fk])))
                self.refused += [decl] * (fill != 'filled')
        s.close()

    def step(self, name, t0):
        self.loadrows.append(dict(step=name, ms='%.0f' % ((time.perf_counter() - t0) * 1000), detail=''))

    def index_sizes(self, s):
        return s.result("SELECT format('%s|%s|%s|%s', c.relname, i.relname, am.amname, pg_relation_size(i.oid)) FROM pg_index x "
                        "JOIN pg_class c ON c.oid = x.indrelid JOIN pg_class i ON i.oid = x.indexrelid JOIN pg_am am ON am.oid = i.relam "
                        "WHERE c.relnamespace = 'public'::regnamespace ORDER BY 1")

    def drop(self):
        if not self.keep:
            admin('DROP DATABASE IF EXISTS %s WITH (FORCE)' % self.db)

    # --- queries

    @staticmethod
    def osql(q):
        """the statement the oracle runs: the `-- oracle:` rewrite, or the main text with EX_EXACT_ORACLE (small sizes: the rewrite cannot weaken the check)"""
        return q.sql if os.environ.get('EX_EXACT_ORACLE') else q.oracle or q.sql

    def esql(self, q):
        """the statement the engine runs: sift engines with the sift planner on use the `-- sift:` form when a query has one (a BM25 predicate), the others and the oracle the main text"""
        return q.alt if q.alt and self.eng.sift and self.eng.name != 'sift-off' else q.sql

    @staticmethod
    def found_proofs(s, sql):
        """the joins answered by the found flag of a connection (EXPLAIN ANALYZE `Lookup: .. N found proofs`)"""
        text = ' '.join(s.result('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' + sql))
        return sum(int(n) for n in re.findall(r'(\d+) found proofs', text))

    def plan(self, s, sql):
        rows = s.result('EXPLAIN (COSTS OFF, FORMAT JSON) ' + sql)
        nodes = []

        def walk(n):
            nodes.append(n['Node Type'] + (':' + n['Custom Plan Provider'] if n.get('Custom Plan Provider') else ''))
            for c in n.get('Plans', []):
                walk(c)
        walk(json.loads('\n'.join(rows))[0]['Plan'])
        uniq = list(dict.fromkeys(nodes))
        return '>'.join(uniq[:8]), any('Sift' in n for n in nodes)

    @staticmethod
    def hint(a, b):
        """the first rows only one side has: what a mismatch looks like"""
        sa, sb = set(a), set(b)
        return 'engine-only %s oracle-only %s (%d vs %d rows)' % (sorted(sa - sb)[:2], sorted(sb - sa)[:2], len(a), len(b))

    @staticmethod
    def same(a, b, ordered):
        return (a == b) if ordered else sorted(a) == sorted(b)

    def check_queries(self, es, os_, tag):
        """every query on the engine session against the oracle session (plain heap plans, sift off): [(query id, ok, nrows)]"""
        res = []
        for q in self.ex.queries:
            got, want = es.result(self.esql(q)), os_.result(self.osql(q))
            res.append((q.id, self.same(got, want, q.ordered), len(want)))
        return res

    def run_queries(self, instr=True, reps=3):
        es, os_ = self.session(), self.session(oracle=True)
        pid = collect.backend_pid(es)
        post = collect.postmaster_pid(pid)
        for q in self.ex.queries:
            want = os_.result(self.osql(q))
            sql = self.esql(q)
            plan, sift_used = self.plan(es, sql)
            proofs = self.found_proofs(es, sql) if q.found and self.eng.name.startswith('connected') else None
            cold = iolib.measure(es, sql, tables=self.tables)
            warm = [iolib.measure(es, sql) for _ in range(reps)]
            got = es.result(sql)
            m = collect.count_instructions(es, sql, pid, post)[0] if instr else None
            self.queryrows.append(dict(
                query=q.id, kind=q.kind, join=q.join, ms=round(statistics.median(w['exec_ms'] for w in warm), 3),
                instr_m='' if m is None else round(m, 2), cold_hit=cold['hit'], cold_read=cold['read'], cold_ms=round(cold['exec_ms'], 3),
                warm_hit=warm[-1]['hit'], warm_read=warm[-1]['read'], plan=plan, sift_used=int(sift_used),
                rows_equal=int(self.same(got, want, q.ordered)), found_ok=int(proofs is None or proofs > 0), plan_ok=int(not (q.plan and self.eng.sift and self.eng.name != 'sift-off') or q.plan in plan), nrows=len(want), trivial=int(trivial(want) and not q.empty_ok)))
        es.close()
        os_.close()

    # --- writes

    def sizes(self, s):
        r = s.result("SELECT coalesce(sum(pg_relation_size(c.oid)) FILTER (WHERE c.relkind = 'r'), 0), "
                     "coalesce(sum(pg_relation_size(c.oid)) FILTER (WHERE c.relkind = 'i' AND am.amname = 'btree'), 0), "
                     "coalesce(sum(pg_relation_size(c.oid)) FILTER (WHERE c.relkind = 'i' AND am.amname <> 'btree'), 0) "
                     "FROM pg_class c LEFT JOIN pg_am am ON am.oid = c.relam WHERE c.relnamespace = 'public'::regnamespace")[0]
        return [int(x) for x in r.split('|')]

    def eval_rows(self, expr):
        return int(eval(expr, {}, {k: int(v) for k, v in self.k.items() if re.fullmatch(r'\d+', str(v))}))

    def batches(self):
        return [b for b in self.ex.batches if self.eng.sift or not b.sift_only]

    def run_batch(self, ws, b, rnd):
        ws.raw('\\set round %d' % rnd)
        before = self.sizes(ws)
        wal0 = int(ws.one("SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '0/0')::bigint"))
        t = time.perf_counter()
        ms, rows, err, _ = ws.run(b.sql)
        wall = (time.perf_counter() - t) * 1000
        after = self.sizes(ws)
        wal = int(ws.one("SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '0/0')::bigint")) - wal0
        return err, wall, wal, [a - b_ for a, b_ in zip(after, before)]

    def run_writes(self):
        ws, es, os_ = self.session(), self.session(), self.session(oracle=True)
        res = self.check_queries(es, os_, 'initial')
        self.writerows.append(self.wrow('initial', 'check', 0, 0, 0, [0, 0, 0], res, ''))
        for b in self.pick_batches():
            rnd = self.next_round()
            err, wall, wal, d = self.run_batch(ws, b, rnd)
            rows = self.eval_rows(b.rows)
            res = self.check_queries(es, os_, b.id)
            self.writerows.append(self.wrow(b.id, b.pattern, rows, wall, wal, d, res, err))
        for s in (ws, es, os_):
            s.close()

    def pick_batches(self):
        """all batches, or with EX_PATTERNS the first batch of each named pattern (a scale run measures a sample of the write side)"""
        pats = [x for x in os.environ.get('EX_PATTERNS', '').split(',') if x]
        if not pats:
            return self.batches()
        return [next(b for b in self.batches() if b.pattern == p) for p in pats if any(b.pattern == p for b in self.batches())]

    def wrow(self, bid, pattern, rows, wall, wal, d, res, err):
        bad = [q for q, ok, _ in res if not ok]
        per = lambda x: round(x / rows, 2) if rows else ''  # noqa: E731
        return dict(batch=bid, pattern=pattern, rows=rows, ms=round(wall, 1), wal_bytes=wal, wal_per_row=per(wal), heap_bytes=d[0], btree_bytes=d[1],
                    other_index_bytes=d[2], index_bytes_per_row=per(d[1] + d[2]), oracle_ok=int(not bad and not err), mismatches=','.join(bad), error=err[:80])

    # --- concurrency

    def run_concurrency(self, seconds=20, readers=3, writers=2, skip=(), txnmix=False):
        stop = threading.Event()
        stats = Counter()
        detail = []
        slock = threading.Lock()
        qs = self.ex.queries
        wb = [b for b in self.batches() if b.concurrent and not b.maintenance and b.id not in skip]
        mb = [b for b in self.batches() if b.maintenance and b.id not in skip]

        def bump(k, n=1):
            with slock:
                stats[k] += n

        def reader(i):
            e, o = self.session(), self.session(oracle=True)
            while not stop.is_set():
                for q in qs:
                    e.must('BEGIN ISOLATION LEVEL REPEATABLE READ')
                    got = e.result(self.esql(q))
                    for st in ORACLE_OFF + (SIFT_OFF if self.eng.sift else []):
                        e.must(st.replace('SET ', 'SET LOCAL '))
                    want = e.result(self.osql(q))
                    e.must('COMMIT')
                    bump('reads')
                    if not self.same(got, want, q.ordered):
                        bump('mismatch')
                        detail.append('reader %s: %s: %s' % (i, q.id, self.hint(got, want)))
                    if stop.is_set():
                        break
            e.close()
            o.close()

        def txn_reader():
            """a transaction that inserts rows (the first concurrent batch with an INSERT), then reads every query with its own rows
            on the engine plan and on the oracle plan; it rolls back and commits in turn"""
            b = next((b for b in wb if re.search(r'(?im)^\s*INSERT\s+INTO', b.sql)), None)
            e = self.session()
            for n in itertools.count(1):
                if b is None or stop.is_set():
                    break
                e.must('BEGIN ISOLATION LEVEL REPEATABLE READ')
                e.raw('\\set round %d' % self.next_round())
                err = e.run(b.sql)[2]
                if err:
                    e.must('ROLLBACK')
                    bump('deadlock_retry' if 'deadlock' in err else 'write_error')
                    continue
                got = [e.result(self.esql(q)) for q in qs]
                for st in ORACLE_OFF + (SIFT_OFF if self.eng.sift else []):
                    e.must(st.replace('SET ', 'SET LOCAL '))
                for q, g in zip(qs, got):
                    bump('txn_reads')
                    want = e.result(self.osql(q))
                    if not self.same(g, want, q.ordered):
                        bump('mismatch')
                        detail.append('txn reader: %s with the transaction\'s own rows: %s' % (q.id, self.hint(g, want)))
                e.must('ROLLBACK' if n % 2 else 'COMMIT')
            e.close()

        def snapshot_reader():
            """REPEATABLE READ rounds: one snapshot re-reads every query three times while the writers run, then commits; a longer
            snapshot would hold DROP INDEX CONCURRENTLY (it waits for every older snapshot) for the rest of the run"""
            e = self.session()
            while not stop.is_set():
                e.must('BEGIN ISOLATION LEVEL REPEATABLE READ')
                base = {q.id: e.result(self.esql(q)) for q in qs}
                for _ in range(3):
                    for q in qs:
                        again = e.result(self.esql(q))
                        bump('snapshot_reads')
                        if not self.same(again, base[q.id], q.ordered):
                            bump('snapshot_changed')
                            detail.append('snapshot: %s changed inside one snapshot: %s' % (q.id, self.hint(again, base[q.id])))
                    time.sleep(0.5)
                e.must('COMMIT')
            e.close()

        def writer(i):
            w = self.session()
            for b in itertools.cycle(wb):
                if stop.is_set() or not wb:
                    break
                for attempt in range(5):
                    err, *_ = self.run_batch(w, b, self.next_round())
                    if 'deadlock' in err or 'could not serialize' in err:
                        bump('deadlock_retry')
                        continue
                    break
                if err and 'deadlock' not in err:
                    bump('write_error')
                    bump('dup_key', 'duplicate key' in err)
                    detail.append('writer %s: %s: %s' % (i, b.id, err[:80]))
                bump('writes')
            w.close()

        def maintenance():
            m = self.session()
            while mb and not stop.wait(max(1, seconds / 20)):
                for b in mb:
                    err, *_ = self.run_batch(m, b, self.next_round())
                    bump('deadlock_retry' if 'deadlock' in err else 'maintenance_error' if err else 'maintenance')
                    bump('ran ' + b.id, int(not err))
                    if err and 'deadlock' not in err:
                        detail.append('maintenance %s: %s' % (b.id, err[:80]))
            m.close()

        threads = [threading.Thread(target=reader, args=(i,)) for i in range(readers)]
        threads += [threading.Thread(target=snapshot_reader), threading.Thread(target=maintenance)]
        threads += [threading.Thread(target=writer, args=(i,)) for i in range(writers)]
        threads += [threading.Thread(target=txn_reader)] * txnmix
        for t in threads:
            t.start()
        time.sleep(seconds)
        stop.set()
        for t in threads:
            t.join(300)
        e, o = self.session(), self.session(oracle=True)
        final = self.check_queries(e, o, 'final')
        e.close()
        o.close()
        bad = [q for q, ok, _ in final if not ok]
        short = [b.id for b in mb if seconds >= 60 and stats['ran ' + b.id] < b.rounds]
        if short:
            detail.append('maintenance batches %s ran fewer than their `-- rounds:`' % ','.join(short))
        self.concrows.append(dict(seconds=seconds, readers=readers, writers=writers, **{k: stats[k] for k in (
            'reads', 'txn_reads', 'mismatch', 'snapshot_reads', 'snapshot_changed', 'writes', 'deadlock_retry', 'write_error', 'dup_key', 'maintenance', 'maintenance_error')},
            final_mismatches=','.join(bad), ok=int(not (bad or stats['mismatch'] or stats['snapshot_changed'] or stats['write_error'] or stats['maintenance_error'] or short)),
            detail=' ; '.join(dict.fromkeys(detail))[:600]))


# --- output

def write_tsv(path, rows, lead):
    if not rows:
        return
    cols = list(lead) + [c for c in rows[0] if c not in lead]
    with open(path, 'w') as f:
        f.write('\t'.join(cols) + '\n')
        for r in rows:
            f.write('\t'.join(str(dict(lead, **r).get(c, '')) for c in cols) + '\n')


# --- readiness and coverage

def ready_ids():
    p = os.path.join(ROOT, 'ready.tsv')
    return {r['id']: r for r in tsv(p)} if os.path.exists(p) else {}


def mark_ready(ex, run_id, engines):
    p = os.path.join(ROOT, 'ready.tsv')
    rows = ready_ids()
    rows[ex.id] = dict(id=ex.id, digest=ex.digest(), run_id=run_id, engines=','.join(engines), date=time.strftime('%Y-%m-%d'))
    with open(p, 'w') as f:
        f.write('# id\tdigest\trun_id\tengines\tdate  (written by run-example --validate; a changed example needs it again)\n')
        f.write('id\tdigest\trun_id\tengines\tdate\n')
        for r in sorted(rows.values(), key=lambda r: r['id']):
            f.write('\t'.join(r[c] for c in ('id', 'digest', 'run_id', 'engines', 'date')) + '\n')


def is_ready(ex, rd):
    return ex.id in rd and rd[ex.id]['digest'] == ex.digest()


def tags_of(ex):
    m = ex.meta
    qs = ex.queries
    t = {'join_type': {q.join for q in qs}, 'query_kind': {q.kind for q in qs}}
    for d, col in (('cardinality', 'cardinality'), ('size_relation', 'size_relation'), ('key_type', 'key_type'), ('write_pattern', 'write_pattern'), ('clustering', 'clustering')):
        t[d] = {v for v in re.split(r'[,\s]+', m.get(col, '')) if v}
    t['clustering'] |= {m.get('corr') or 'random'}
    sk = float(m.get('skew') or 1)
    t['skew'] = {'zipf' if sk > 1 else 'uniform'} | ({'hot_key'} if float(m.get('hot') or 0) > 0 else set())
    t['write_pattern'] |= {b.pattern for b in ex.batches}
    t['scale'] = {'S', 'M', 'L'}
    t['storage'] = {m.get('storage') or 'heap'}
    return t


def vocab():
    out = {}
    for r in tsv(os.path.join(ROOT, 'vocab.tsv')):
        out.setdefault(r['dimension'], {})[r['value']] = r['meaning']
    return out


def example_ids():
    return sorted(r['id'] for r in tsv(os.path.join(ROOT, 'registry.tsv')))


def check_registry():
    """problems: a tag value outside the vocabulary, a missing file, a query without a kind, an example without writes"""
    voc, problems = vocab(), []
    for i in example_ids():
        ex = Example(i)
        for f in ('schema.sql', 'gen.sql', 'btree.sql', 'sift.sql', 'queries.sql', 'writes.sql', 'links.tsv', 'connected.sql'):
            if not ex.file(f):
                problems.append('%s: missing %s' % (i, f))
        if not ex.file('queries.sql'):
            continue
        for d, vals in tags_of(ex).items():
            for v in vals:
                if v not in voc.get(d, {}):
                    problems.append('%s: %s=%s is not in vocab.tsv' % (i, d, v))
        ids = [q.id for q in ex.queries]
        if len(ids) != len(set(ids)) or len(ids) < 3:
            problems.append('%s: needs 3 or more distinct query ids, has %s' % (i, ids))
        if len(ex.batches) < 3:
            problems.append('%s: needs 3 or more write batches' % i)
    return problems


def coverage(only_ready=False):
    """markdown: for each vocabulary dimension the examples that carry each value; a value with no example is a gap"""
    voc, rd, lines = vocab(), ready_ids(), []
    exs = [Example(i) for i in example_ids()]
    exs = [e for e in exs if is_ready(e, rd)] if only_ready else exs
    tags = {e.id: tags_of(e) for e in exs}
    lines.append('%d examples%s\n' % (len(exs), ' (ready)' if only_ready else ' (registered)'))
    gaps, thin = [], []
    for d, values in voc.items():
        lines += ['### %s' % d, '', '| value | examples | count |', '|---|---|---|']
        for v in values:
            ids = [i for i, t in tags.items() if v in t.get(d, ())]
            lines.append('| %s | %s | %d |' % (v, ', '.join(ids[:6]) + (' ...' if len(ids) > 6 else ''), len(ids)))
            if not ids:
                gaps.append('%s=%s' % (d, v))
            elif len(ids) < 3:
                thin.append('%s=%s(%d)' % (d, v, len(ids)))
        lines.append('')
    lines.append('gaps (no example): %s' % (', '.join(gaps) or 'none'))
    lines.append('thin (fewer than 3 examples): %s' % (', '.join(thin) or 'none'))
    return '\n'.join(lines)
