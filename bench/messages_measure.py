#!/usr/bin/env python3
"""The gain and the cost of the decisions that a NOTICE of docs/MESSAGES.md announces, in absolute numbers of one backend, for the cells of docs/messages.tsv.

    PGHOST=DIR PGPORT=N python3 -I bench/messages_measure.py [--only CASE,..] [--out docs/messages-measured.tsv]

A server with the testing build (sift.test_limits) must run: scripts/pgtest.sh start. Each case builds a table of 100,000 rows or fewer and runs one statement in the
states a message tells apart (what it announces against the option that avoids it, and a b-tree where one answers the statement). A row of the output is one state of
one statement: instructions (perf stat -e instructions:u on the backend, the whole statement, warm), the shared blocks of the plan (hit + read, EXPLAIN (ANALYZE, BUFFERS)),
the bytes of the index, the WAL bytes of a write, the rows. The figures are absolute: a ratio of two small numbers means nothing.
"""
import argparse
import re
import signal
import subprocess
import sys
import tempfile
import time
import os  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'steps'))
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)

CASES = {}


def case(f):
    CASES[f.__name__] = f
    return f


class Session:
    """one psql process; perf counts the instructions of its backend"""

    def __init__(self):
        self.p = subprocess.Popen(['psql', '-X', '-At', 'postgres'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.pid = int(self.run('SELECT pg_backend_pid()')[0])

    def run(self, sql):
        self.p.stdin.write(sql.rstrip().rstrip(';') + ";\nSELECT '@@DONE@@';\n")
        self.p.stdin.flush()
        out = []
        while True:
            line = self.p.stdout.readline()
            if line == '' or line.strip() == '@@DONE@@':
                return [x for x in out if x != '']
            out.append(line.rstrip('\n'))

    def counted(self, sql):
        with tempfile.NamedTemporaryFile('r', suffix='.perf') as f:
            perf = subprocess.Popen(['perf', 'stat', '-e', 'instructions:u', '-x', ',', '-p', str(self.pid), '-o', f.name], stderr=subprocess.DEVNULL)
            time.sleep(0.3)
            rows = self.run(sql)
            perf.send_signal(signal.SIGINT)
            perf.wait()
            m = re.search(r'^(\d+),,instructions', f.read(), re.M)
        return rows, int(m.group(1)) if m else None

    def blocks(self, sql):
        out = ' '.join(self.run('EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF) ' + sql))
        return sum(int(v) for k, v in re.findall(r'(hit|read)=(\d+)', out.split('Buffers:')[1].split('Planning')[0]) ) if 'Buffers:' in out else 0

    def plan(self, sql):
        return ' / '.join(x.strip() for x in self.run('EXPLAIN (COSTS OFF) ' + sql) if 'Scan' in x or 'Sift' in x or 'Aggregate' in x)[:90]

    def size(self, index):
        return int(self.run("SELECT pg_relation_size('%s')" % index)[0])

    def lsn(self):
        return int(self.run("SELECT pg_current_wal_lsn() - '0/0'::pg_lsn")[0])


class Out:
    def __init__(self, s, name, table):
        self.s, self.name, self.table, self.rows = s, name, table, []

    def stmt(self, state, sql, index=None):
        """one statement in one state: warm run, then the counted one; a state that names a b-tree runs without the aggregate scan and without a sequential scan"""
        s = self.s
        bt = state.startswith('b-tree') or state.startswith('gin')
        s.run('SET sift.enable_agg = ' + ('off' if bt else 'on'))
        s.run('SET enable_seqscan = ' + ('off' if bt else 'on'))
        if bt:
            s.run('VACUUM ANALYZE ' + self.table)
        s.run(sql)
        r, ins = s.counted(sql)
        blocks = s.blocks(sql) if sql.lstrip().lower().startswith('select') else ''
        self.rows.append((self.name, state, sql if len(sql) < 120 else sql[:117] + '...', (r[0][:30] if len(r) == 1 else '%d rows' % len(r)), ins, blocks, (sum(s.size(i) for i in index.split(',')) if index else ''), s.plan(sql) if sql.lstrip().lower().startswith('select') else ''))


PRE = ["SET client_min_messages = warning", "SET max_parallel_workers_per_gather = 0", "SET max_parallel_maintenance_workers = 0", "SET jit = off", "CREATE EXTENSION IF NOT EXISTS pg_sift"]
OPTS = "point_tid_columns = 'off', order_array_columns = 'off'"


def caps(s):
    for q in PRE + ["DROP TABLE IF EXISTS caps CASCADE", "CREATE TABLE caps (id int, few int, many int, mid int, wide bigint) WITH (autovacuum_enabled = off)",
                    "INSERT INTO caps SELECT g, g % 7, (g * 7919) % 50000, ((g * 7919) % 400) * 3, ((g * 7919) % 3000) * 3 FROM generate_series(1, 100000) g", "VACUUM (FREEZE, ANALYZE) caps"]:
        s.run(q)


def drop(s, *idx):
    for i in idx:
        s.run('DROP INDEX IF EXISTS ' + i)


@case
def cube_dim_no_histogram(s):
    """cube-dim-no-histogram: a dimension over the cap of groups of a unit has no histogram there; GROUP BY it scans the units"""
    o = Out(s, 'cube_dim_no_histogram', 'caps')
    caps(s)
    for col, state in (('few', 'histogram (7 groups)'), ('many', 'no histogram (50,000 groups a unit, cap 256)')):
        q = "SELECT %s, count(*) FROM caps GROUP BY %s" % (col, col)
        drop(s, 'm_c', 'm_bt')
        s.run("CREATE INDEX m_c ON caps USING sift_col (id, %s) WITH (preagg = 'cube(%s: id)', key_columns = 'off', %s)" % (col, col, OPTS))
        o.stmt('sift, ' + state, q, 'm_c')
        drop(s, 'm_c')
        s.run('CREATE INDEX m_bt ON caps (%s)' % col)
        o.stmt('b-tree on %s' % col, q, 'm_bt')
        drop(s, 'm_bt')
    return o.rows


@case
def keys_columns(s):
    """keys-bloom-filter, keys-no-summary, keys-no-list: what key_columns writes for a column of 400, 50,000 and sequential keys, and what a lookup reads"""
    o = Out(s, 'keys_columns', 'caps')
    caps(s)
    for col, what in (('mid', 'bloom filter (400 keys a unit)'), ('many', 'no key list (50,000 keys a unit)'), ('id', 'no summary (every integer of a range)')):
        for state, kc in ((what, col), ('key_columns off', 'off')):
            drop(s, 'm_k')
            s.run("CREATE INDEX m_k ON caps USING sift_col (%s) WITH (key_columns = '%s', %s)" % ('id' if col == 'id' else 'id, ' + col, kc, OPTS))
            o.stmt('sift %s, equality %s' % (state, col), "SELECT count(*) FROM caps WHERE %s = 3" % col, 'm_k')
            o.stmt('sift %s, count(DISTINCT %s)' % (state, col), "SELECT count(DISTINCT %s) FROM caps" % col, 'm_k')
        drop(s, 'm_k')
        s.run('CREATE INDEX m_bt ON caps (%s)' % col)
        o.stmt('b-tree on %s, equality' % col, "SELECT count(*) FROM caps WHERE %s = 3" % col, 'm_bt')
        o.stmt('b-tree on %s, count(DISTINCT)' % col, "SELECT count(DISTINCT %s) FROM caps" % col, 'm_bt')
        drop(s, 'm_bt')
    return o.rows


@case
def rollup_not_written(s):
    """rollup-not-written: the combinations over the cells a unit may keep write no rollup; a GROUP BY of both scans the units"""
    o = Out(s, 'rollup_not_written', 'caps')
    caps(s)
    for cols, state in (('few, mid', 'rollup written (2,800 combinations)'), ('few, many', 'no rollup (350,000 combinations)')):
        a, b = cols.split(', ')
        drop(s, 'm_r')
        s.run("CREATE INDEX m_r ON caps USING sift_col (id, %s, %s) WITH (rollup_dims = '%s', key_columns = 'off', %s)" % (a, b, cols, OPTS))
        o.stmt('sift ' + state, "SELECT %s, %s, count(*) FROM caps GROUP BY %s, %s" % (a, b, a, b), 'm_r')
        drop(s, 'm_r')
        s.run('CREATE INDEX m_bt ON caps (%s, %s)' % (a, b))
        o.stmt('b-tree on (%s, %s)' % (a, b), "SELECT %s, %s, count(*) FROM caps GROUP BY %s, %s" % (a, b, a, b), 'm_bt')
        drop(s, 'm_bt')
    return o.rows


@case
def cube_columns_left_out(s):
    """cube-columns-left-out: a plain column past the 16 dimensions of the cube has no histogram; GROUP BY it scans the units"""
    o = Out(s, 'cube_columns_left_out', 'wide20')
    for q in PRE + ["DROP TABLE IF EXISTS wide20 CASCADE", "CREATE TABLE wide20 (" + ', '.join('c%d int' % i for i in range(1, 21)) + ") WITH (autovacuum_enabled = off)",
                    "INSERT INTO wide20 SELECT " + ', '.join('g %% %d' % (i + 2) for i in range(20)) + " FROM generate_series(1, 100000) g", "VACUUM (FREEZE, ANALYZE) wide20"]:
        s.run(q)
    s.run("CREATE INDEX m_w ON wide20 USING sift_col (" + ', '.join('c%d' % i for i in range(1, 21)) + ") WITH (key_columns = 'off', point_tid_columns = 'off', order_array_columns = 'off', rollup_dims = 'off')")
    for col, state in (('c3', 'inside the 16 dimensions'), ('c19', 'left out of the cube')):
        o.stmt('sift, GROUP BY %s %s' % (col, state), "SELECT %s, count(*) FROM wide20 GROUP BY %s" % (col, col), 'm_w')
    drop(s, 'm_w')
    for col in ('c3', 'c19'):
        s.run('CREATE INDEX m_bt ON wide20 (%s)' % col)
        o.stmt('b-tree on %s' % col, "SELECT %s, count(*) FROM wide20 GROUP BY %s" % (col, col), 'm_bt')
        drop(s, 'm_bt')
    s.run('DROP TABLE wide20')
    return o.rows


@case
def array_elements(s):
    """array-elements: the elements are kept in place of the arrays; an element test reads the rows that hold the element, GROUP BY and DISTINCT of the array read the table"""
    o = Out(s, 'array_elements', 'tags')
    for q in PRE + ["DROP TABLE IF EXISTS tags CASCADE", "CREATE TABLE tags (id int, tg text[], n int) WITH (autovacuum_enabled = off)",
                    "INSERT INTO tags SELECT g, ARRAY['t' || (g % 5000), 'u' || ((g * 7) % 7001), 'v' || ((g * 13) % 9001)], g % 10 FROM generate_series(1, 100000) g", "VACUUM (FREEZE, ANALYZE) tags"]:
        s.run(q)
    for state, paths in (('elements kept (paths auto)', 'auto'), ('arrays kept (paths auto:)', 'auto:')):
        drop(s, 'm_t')
        s.run("CREATE INDEX m_t ON tags USING sift_col (id, tg, n) WITH (%s)" % ("paths = '%s', " % paths + "key_columns = 'off', " + OPTS))
        o.stmt('sift %s, tg @> {t17}' % state, "SELECT count(*) FROM tags WHERE tg @> ARRAY['t17']", 'm_t')
        o.stmt('sift %s, GROUP BY tg' % state, "SELECT count(*) FROM (SELECT tg FROM tags GROUP BY tg) x", 'm_t')
    drop(s, 'm_t')
    s.run('CREATE INDEX m_g ON tags USING gin (tg)')
    o.stmt('gin on tg, tg @> {t17}', "SELECT count(*) FROM tags WHERE tg @> ARRAY['t17']", 'm_g')
    o.stmt('gin on tg, GROUP BY tg', "SELECT count(*) FROM (SELECT tg FROM tags GROUP BY tg) x", 'm_g')
    drop(s, 'm_g')
    s.run('DROP TABLE tags')
    return o.rows


@case
def string_overflow(s):
    """string-overflow: past the byte budget of a unit the strings of a leaf are read from the heap (sift.test_limits lowers the budget to 64 kB)"""
    o = Out(s, 'string_overflow', 'so')
    for q in PRE + ["DROP TABLE IF EXISTS so CASCADE", "CREATE TABLE so (id int, payload text, g int) WITH (autovacuum_enabled = off)",
                    "INSERT INTO so SELECT i, repeat(md5(i::text), 6), i % 50 FROM generate_series(1, 30000) i", "VACUUM (FREEZE, ANALYZE) so"]:
        s.run(q)
    for state, lim in (('no overflow (budget 1 GB)', None), ('overflow (budget 64 kB a unit)', 'unit_bytes=65536')):
        drop(s, 'm_s')
        if lim:
            s.run("SET sift.test_limits = '%s'" % lim)
        s.run("CREATE INDEX m_s ON so USING sift_col (id, g, payload) WITH (key_columns = 'off', %s)" % OPTS)
        s.run('RESET sift.test_limits')
        o.stmt('sift %s, count(DISTINCT payload)' % state, "SELECT count(DISTINCT payload) FROM so", 'm_s')
        o.stmt('sift %s, payload = x' % state, "SELECT count(*) FROM so WHERE payload = repeat(md5('777'), 6)", 'm_s')
    drop(s, 'm_s')
    s.run('CREATE INDEX m_bt ON so (payload)')
    o.stmt('b-tree on payload, payload = x', "SELECT count(*) FROM so WHERE payload = repeat(md5('777'), 6)", 'm_bt')
    drop(s, 'm_bt')
    s.run('DROP TABLE so')
    return o.rows


@case
def hot_update_columns(s):
    """hot-update-columns: covering a counter keeps its aggregates answerable from the index; the UPDATE of it is not HOT"""
    o = Out(s, 'hot_update_columns', 'hu')
    for q in PRE + ["DROP TABLE IF EXISTS hu CASCADE", "CREATE TABLE hu (id int, g int, views int) WITH (autovacuum_enabled = off, fillfactor = 70)",
                    "INSERT INTO hu SELECT i, i % 50, i % 1000 FROM generate_series(1, 100000) i", "VACUUM (FREEZE, ANALYZE) hu"]:
        s.run(q)
    q = "SELECT g, sum(views) FROM hu GROUP BY g"
    for state, cols in (('covers views', 'id, g, views'), ('does not cover views', 'id, g')):
        drop(s, 'm_h')
        s.run("CREATE INDEX m_h ON hu USING sift_col (%s) WITH (key_columns = 'off', %s)" % (cols, OPTS))
        o.stmt('sift %s, sum(views) by g' % state, q, 'm_h')
        s.run("SELECT pg_stat_reset_single_table_counters('hu'::regclass)")
        w0 = s.lsn()
        s.run("UPDATE hu SET views = views + 1 WHERE id % 10 = 0")
        w = s.lsn() - w0
        s.run('SELECT pg_stat_force_next_flush()')
        hot = s.run("SELECT n_tup_hot_upd || ' of ' || n_tup_upd FROM pg_stat_user_tables WHERE relname = 'hu'")
        o.rows.append((o.name, 'sift %s, UPDATE of 10,000 rows' % state, 'UPDATE hu SET views = views + 1 WHERE id % 10 = 0', 'wal_bytes=%d' % w, '', '', s.size('m_h'), 'hot updates: %s' % (hot[0] if hot else '?')))
        s.run('VACUUM hu')
    drop(s, 'm_h')
    s.run('CREATE INDEX m_bt ON hu (g) INCLUDE (views)')
    s.run('VACUUM hu')
    o.stmt('b-tree on (g) INCLUDE (views), sum(views) by g', q, 'm_bt')
    drop(s, 'm_bt')
    s.run('DROP TABLE hu')
    return o.rows



@case
def join_keys(s):
    """primary-key-not-indexed, foreign-key-not-indexed: a join of two tables of sift indexes on a key that is a column of both indexes against one that is not"""
    o = Out(s, 'join_keys', 'orders')
    for q in PRE + ["DROP TABLE IF EXISTS orders, customers CASCADE", "CREATE TABLE customers (id int PRIMARY KEY, region int) WITH (autovacuum_enabled = off)",
                    "INSERT INTO customers SELECT g, g % 20 FROM generate_series(1, 5000) g",
                    "CREATE TABLE orders (id int PRIMARY KEY, cust_id int REFERENCES customers, amt int) WITH (autovacuum_enabled = off)",
                    "INSERT INTO orders SELECT g, (g * 7919) % 5000 + 1, g % 1000 FROM generate_series(1, 100000) g", "VACUUM (FREEZE, ANALYZE) customers", "VACUUM (FREEZE, ANALYZE) orders"]:
        s.run(q)
    q = "SELECT c.region, sum(o.amt) FROM orders o JOIN customers c ON c.id = o.cust_id GROUP BY c.region"
    qf = "SELECT sum(o.amt) FROM orders o WHERE o.cust_id IN (SELECT id FROM customers WHERE region = 3)"
    qd = "SELECT count(*), sum(o.amt) FROM customers c JOIN orders o ON o.cust_id = c.id WHERE c.region = 3"
    for state, ic, io in (('both keys are columns of the indexes', 'id, region', 'id, cust_id, amt'), ('the primary key is not a column of customers index', 'region', 'id, cust_id, amt'),
                          ('the foreign key is not a column of orders index', 'id, region', 'id, amt')):
        drop(s, 'm_c', 'm_o')
        s.run("CREATE INDEX m_c ON customers USING sift_col (%s) WITH (key_columns = 'off', %s)" % (ic, OPTS))
        s.run("CREATE INDEX m_o ON orders USING sift_col (%s) WITH (key_columns = 'off', %s)" % (io, OPTS))
        o.stmt('sift: %s (customers index %d B, orders index %d B)' % (state, s.size('m_c'), s.size('m_o')), q, 'm_c,m_o')
        if 'primary' in state or 'both' in state:
            o.stmt('sift, customers of one region: %s' % state, qf, 'm_c,m_o')
            o.stmt('sift, customers of one region joined: %s' % state, qd, 'm_c,m_o')
    drop(s, 'm_c', 'm_o')
    s.run('CREATE INDEX m_bo ON orders (cust_id)')
    o.stmt('b-tree: primary keys and a b-tree on orders(cust_id)', q.replace('SELECT', 'SELECT', 1), 'm_bo')
    drop(s, 'm_bo')
    s.run('DROP TABLE orders, customers')
    return o.rows



@case
def wildcard_keys(s):
    """wildcard-keys-overflow: past 65,536 distinct keys of a map in a segment the keys are read from the heap, and a query that reads the map keeps the normal plan"""
    o = Out(s, 'wildcard_keys', 'wk')
    q = "SELECT count(*) FROM wk WHERE (d->>'k77')::int = 77"
    for rows, state in ((60000, 'under the limit (60,000 keys)'), (80000, 'over the limit (80,000 keys, 2 of 4 units overflow)')):
        for stmt in PRE + ["DROP TABLE IF EXISTS wk CASCADE", "CREATE TABLE wk (id int, d jsonb) WITH (autovacuum_enabled = off)",
                           "INSERT INTO wk SELECT g, jsonb_build_object('k' || g, g, 'common', g %% 10) FROM generate_series(1, %d) g" % rows, "VACUUM (FREEZE, ANALYZE) wk"]:
            s.run(stmt)
        s.run("CREATE INDEX m_w ON wk USING sift_col (id, d) WITH (paths = 'd->* int4', key_columns = 'off', %s)" % OPTS)
        o.stmt('sift ' + state, q, 'm_w')
        drop(s, 'm_w')
    s.run('CREATE INDEX m_gin ON wk USING gin (d jsonb_path_ops)')
    o.stmt('gin (jsonb_path_ops), 80,000 keys', "SELECT count(*) FROM wk WHERE d @> '{\"k77\": 77}'", 'm_gin')
    drop(s, 'm_gin')
    s.run('DROP TABLE wk')
    return o.rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', default='')
    ap.add_argument('--out', default='docs/messages-measured.tsv')
    a = ap.parse_args()
    s = Session()
    rows = []
    for n, f in CASES.items():
        if a.only and n not in a.only.split(','):
            continue
        print(n, file=sys.stderr)
        rows += f(s)
    with open(a.out, 'w') as f:
        f.write('# case\tstate\tstatement\tresult\tinstructions\tbuffers\tindex_bytes\tplan (bench/messages_measure.py: one backend, perf stat -e instructions:u, warm, tmpfs; rows in the statement column of the case)\n')
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')
    print('%d rows in %s' % (len(rows), a.out))


if __name__ == '__main__':
    sys.exit(main())
