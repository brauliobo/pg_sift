#!/usr/bin/env python3
"""What track_changes costs the writer of a sift_col index, and the budget it must stay in (bench/steps/track_gate.sh starts the cluster and runs this).

One table (id int PRIMARY KEY, g, s, amt, q, pad; fillfactor 90) and one index, built four ways, on fresh copies, ROWS rows, one backend:
  base      index (g, s, amt, q), track_changes = 'off': nothing is tracked
  ctid      the same index, track_changes = 'ctid': a row trigger writes the address of each version a statement ends (no key, any heap table)
  key       index (id, g, s, amt, q), track_changes = 'id': the statement triggers with the old rows, the unique column found by its b-tree
  ctid_pk   index (id, g, s, amt, q), track_changes = 'ctid': the same index as `key`, tracked by address (the two ways on one table, same columns)
Per operation (INSERT of every row, UPDATE of 5% of the rows in an indexed column, the same in a column no index holds (HOT), DELETE of 2%, 2,000 one-row UPDATE transactions):
milliseconds, WAL bytes and the size of the index, the median of REPS runs. With --mass N the DELETE and the UPDATE of every row of N and the peak memory of the backend.
The budget is the one of `ctid` over `base`: an indexed UPDATE and a DELETE at most +30% time and +40% WAL, an INSERT +5% time (noise) and +3% WAL (the 4 bytes of the inserter's transaction id that every row of a tracked index carries), a one-row transaction +10% time.
Each measurement waits for a quiet host (the CPU and IO pressure of /proc/pressure, avg10 under 10%, at most 5 minutes) and a figure over its budget is measured again, up to 3 times in all: a
host that was busy for one run does not fail the gate, a cost that is over its budget every time does.
Exit status 1 when a figure is over its budget in every run."""
import argparse, json, os, re, statistics, subprocess, sys, time

DB = 'track_gate'
BUDGET = {   # operation: (time %, WAL %)
    'update_5pct_indexed': (30, 40),
    'delete_2pct': (30, 40),
    'insert': (5, 3),
    'update_1row_tx': (10, None),
}
VARIANTS = {
    'base': ('g, s, amt, q', 'off'),
    'ctid': ('g, s, amt, q', 'ctid'),
    'key': ('id, g, s, amt, q', 'id'),
    'ctid_pk': ('id, g, s, amt, q', 'ctid'),
}
ENV = dict(os.environ, LC_ALL='C')


def psql(script, db=DB, timeout=3600):
    r = subprocess.run(['psql', '-X', '-qAt', '-U', 'postgres', '-d', db, '-v', 'ON_ERROR_STOP=1'], input=script, capture_output=True, text=True, env=ENV, timeout=timeout)
    if r.returncode:
        sys.exit('track_gate: %s' % (r.stderr or r.stdout)[-600:])
    return r.stdout


def table_sql(variant):
    cols, track = VARIANTS[variant]
    return ['SET max_parallel_workers_per_gather = 0;', 'DROP TABLE IF EXISTS wt;',
            "CREATE TABLE wt (id int PRIMARY KEY, g int NOT NULL, s text NOT NULL, amt numeric NOT NULL, q int NOT NULL, pad text NOT NULL DEFAULT 'x') WITH (autovacuum_enabled = false, fillfactor = 90);",
            "CREATE INDEX wt_i ON wt USING sift_col (%s) WITH (track_changes = '%s');" % (cols, track)]


def op(name, stmt):
    return ['SELECT pg_current_wal_insert_lsn() AS l0 \\gset', '\\echo OP %s' % name, '\\timing on', stmt + ';', '\\timing off',
            "SELECT 'WAL', pg_wal_lsn_diff(pg_current_wal_insert_lsn(), :'l0');", "SELECT 'SIZE', pg_relation_size('wt_i');"]


def insert_sql(rows):
    return "INSERT INTO wt SELECT i, i %% 997, 'st' || (i %% 7), (i %% 1000) / 10.0, i %% 50 FROM generate_series(1, %d) i" % rows


def run_bulk(variant, rows):
    s = table_sql(variant) + op('insert', insert_sql(rows)) + ['VACUUM wt;']
    s += op('update_5pct_indexed', 'UPDATE wt SET q = q + 1 WHERE id % 20 = 0')
    s += op('update_5pct_unindexed', "UPDATE wt SET pad = 'y' WHERE id % 20 = 1")
    s += op('delete_2pct', 'DELETE FROM wt WHERE id % 50 = 7')
    res, cur = {}, None
    for line in psql('\n'.join(s) + '\n').splitlines():
        m = re.match(r'OP (\S+)', line)
        if m:
            cur = res.setdefault(m.group(1), {})
        elif cur is not None and (m := re.match(r'(WAL|SIZE)\|(\d+)', line)):
            cur[m.group(1).lower()] = int(m.group(2))
        elif cur is not None and (m := re.match(r'Time: ([0-9.]+) ms', line)):
            cur['ms'] = float(m.group(1))
    return res


def run_one_row(rows, transactions=2000):
    """one-row UPDATE transactions, pgbench on one client: ms and WAL bytes per transaction"""
    path = '/tmp/track_gate_update.sql'
    open(path, 'w').write('\\set rid random(1, %d)\nUPDATE wt SET q = q + 1 WHERE id = :rid;\n' % rows)
    l0 = psql("SELECT pg_current_wal_insert_lsn()").strip()
    r = subprocess.run(['pgbench', '-n', '-U', 'postgres', '-d', DB, '-f', path, '-t', str(transactions), '-c', '1'], capture_output=True, text=True, env=ENV, timeout=900)
    m = re.search(r'latency average = ([0-9.]+) ms', r.stdout)
    wal = float(psql("SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), '%s')" % l0).strip())
    return dict(ms=float(m.group(1)), wal=wal / transactions)


def run_mass(variant, rows):
    """DELETE and UPDATE of every row: ms, WAL bytes, the peak memory of the backend (VmHWM, MB)"""
    out = {}
    for name, stmt in (('delete_all', 'DELETE FROM wt'), ('update_all_indexed', 'UPDATE wt SET q = q + 1')):
        s = table_sql(variant) + [insert_sql(rows) + ';', 'VACUUM wt;', 'CHECKPOINT;'] + op(name, stmt)
        s.append("DO $$ BEGIN EXECUTE format($f$COPY (SELECT 1) TO PROGRAM 'grep VmHWM /proc/%s/status > /tmp/track_gate_hwm'$f$, pg_backend_pid()); END $$;")
        r = psql('\n'.join(s) + '\n')
        d = {}
        for line in r.splitlines():
            if (m := re.match(r'WAL\|(\d+)', line)):
                d['wal'] = int(m.group(1))
            elif (m := re.match(r'Time: ([0-9.]+) ms', line)):
                d['ms'] = float(m.group(1))
        d['peak_mb'] = int(open('/tmp/track_gate_hwm').read().split()[1]) / 1024
        out[name] = d
    return out


def wait_quiet(limit=300):
    """waits until the some-avg10 of the CPU and IO pressure is under 10%; False when it was not within `limit` seconds"""
    def avg10(name):
        return float(re.search(r'some avg10=([0-9.]+)', open('/proc/pressure/' + name).read()).group(1))
    end = time.time() + limit
    while avg10('cpu') >= 10 or avg10('io') >= 10:
        if time.time() > end:
            return False
        time.sleep(5)
    return True


def median(rows, op_, key):
    return statistics.median(r[op_][key] for r in rows)


def measure(args):
    """one measurement on a quiet host: the figures and the operations over their budget"""
    if not wait_quiet():
        print('track_gate: the host was not quiet (pressure avg10 >= 10%) after 5 minutes; measuring anyway')
    runs = {v: [] for v in VARIANTS}
    for _ in range(args.reps):
        for v in VARIANTS:
            r = run_bulk(v, args.rows)
            r['update_1row_tx'] = run_one_row(args.rows)
            runs[v].append(r)
    fig = {v: {op_: {k: median(rs, op_, k) for k in rs[0][op_]} for op_ in rs[0]} for v, rs in runs.items()}
    bad = []
    for op_, (t, w) in BUDGET.items():
        base, ctid = fig['base'][op_], fig['ctid'][op_]
        dt, dw = 100 * (ctid['ms'] / base['ms'] - 1), 100 * (ctid['wal'] / base['wal'] - 1)
        print('budget %-22s ctid over base: %+.1f%% time (<= %s), %+.1f%% WAL (<= %s)' % (op_, dt, t, dw, w))
        if dt > t or (w is not None and dw > w):
            bad.append(op_)
    return fig, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rows', type=int, default=200000)
    ap.add_argument('--reps', type=int, default=5)
    ap.add_argument('--mass', type=int, default=0, help='rows of the DELETE and UPDATE of every row (memory of the backend); 0 skips')
    ap.add_argument('--json', help='file for the figures')
    args = ap.parse_args()
    psql('DROP DATABASE IF EXISTS %s' % DB, db='postgres')
    psql('CREATE DATABASE %s' % DB, db='postgres')
    psql('CREATE EXTENSION pg_sift')
    for attempt in range(1, 4):
        fig, bad = measure(args)
        if not bad:
            break
        print('run %d of 3: over budget: %s' % (attempt, ', '.join(bad)))
    if args.mass:
        fig['mass'] = {v: run_mass(v, args.mass) for v in ('base', 'key', 'ctid')}
    print('%d rows, median of %d; ms, WAL kB, index kB (last run)' % (args.rows, args.reps))
    for op_ in fig['base']:
        for v in VARIANTS:
            f = fig[v][op_]
            print('  %-22s %-8s %10.2f ms %10.1f kB WAL %s' % (op_, v, f['ms'], f['wal'] / 1024, '%10.0f kB index' % (f['size'] / 1024) if 'size' in f else ''))
    for v in ('key', 'ctid_pk'):
        for op_ in ('update_5pct_indexed', 'delete_2pct'):
            print('over base, %-8s %-22s %+.1f%% time %+.1f%% WAL' % (v, op_, 100 * (fig[v][op_]['ms'] / fig['base'][op_]['ms'] - 1), 100 * (fig[v][op_]['wal'] / fig['base'][op_]['wal'] - 1)))
    for v, m in fig.get('mass', {}).items():
        for name, d in m.items():
            print('mass %-6s %-20s %9.0f ms %8.1f MB WAL %7.0f MB peak' % (v, name, d['ms'], d['wal'] / 1e6, d['peak_mb']))
    if args.json:
        json.dump(fig, open(args.json, 'w'), indent=1)
    if bad:
        sys.exit('track_gate: over budget in all 3 runs: %s' % ', '.join(bad))


main()
