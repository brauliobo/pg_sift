"""The update, delete and VACUUM section of the README (store.py section): PostgreSQL and the pg_sift lanes change a table of --rows orders in bulk (the steps of
store_updates.py, with the WAL of each), then under steady mixed churn and under inserts only (pgbench, CLIENTS clients, --churn-seconds each). One result file per size and repetition."""
import glob
import json
import os
import subprocess
import tempfile
import time
from store_common import host, percentile, write
from store_data import Rows, effective, load_pg, mib, servers
from store_engine import Window, gate, psql_value
from store_lib import LANES, NOISY, container, load_queries, log, port
from store_updates import update_engine

SECTION_LANES = ['pg', 'sift', 'sift_maint', 'sift_table', 'sift_table_maint']
CLIENTS = 4

CHURN = r"""\set pick random(1, 100)
\set id random(1, :rows)
\set nid random(:rows + 1, 2000000000)
\if :pick <= 40
UPDATE orders SET status = 'refunded', amount = amount - 1 WHERE id = :id;
\elif :pick <= 70
DELETE FROM orders WHERE id = :id;
\else
INSERT INTO orders SELECT :nid, customer_id, country, category, status, amount, quantity, created_at, title FROM orders WHERE id = :id;
\endif
"""

APPEND = r"""\set base random(:rows + 1, 1900000000)
INSERT INTO orders SELECT :base + g, 1 + g % 1000, 'DE', 'cat' || g % 40, 'shipped', 10 + g % 90, 1, timestamp '2025-06-01' + g * interval '1 second', 'item ' || g FROM generate_series(1, 1000) g;
"""


def lsn(eid):
    return psql_value(eid, 'SELECT pg_current_wal_lsn()')


def wal_bytes(eid, since):
    return int(psql_value(eid, f"SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '{since}')"))


def size_mib(eid):
    psql_value(eid, 'CHECKPOINT')
    return mib(psql_value(eid, "SELECT pg_total_relation_size('orders')"))


def pgbench(eid, script, rows, seconds, clients, prefix, extra=()):
    cmd = ['pgbench', '-n', '-h', '127.0.0.1', '-p', str(port(eid)), '-U', 'postgres', '-T', str(seconds), '-c', str(clients), '-j', str(min(clients, 2)), '-l', f'--log-prefix={prefix}',
           '-D', f'rows={rows}', '-f', script, *extra, LANES[eid][3]]
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def latencies_ms(prefix):
    out = []
    for f in glob.glob(prefix + '*'):
        out += [int(l.split()[2]) / 1000 for l in open(f) if l.strip()]
    return out


def window(eid, rows, args, name, script, read=None):
    """CLIENTS clients run `script` for --churn-seconds; the reads while it runs (one client, 20 a second at most) are the statement `read`; WAL and the size follow"""
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, 'w.pgb')
        open(path, 'w').write(script)
        start, t0 = lsn(eid), time.time()
        w = pgbench(eid, path, args.rows, args.churn_seconds, CLIENTS, os.path.join(d, 'w'))
        r = None
        if read:
            rp = os.path.join(d, 'r.pgb')
            open(rp, 'w').write(read + ';\n')
            r = pgbench(eid, rp, args.rows, args.churn_seconds, 1, os.path.join(d, 'r'), ('-R', '20'))
        out = w.communicate()[0]
        if r:
            r.communicate()
        secs = time.time() - t0
        lat = latencies_ms(os.path.join(d, 'w'))
        wal = wal_bytes(eid, start)
        if not lat:
            raise RuntimeError(f'{name}: pgbench ran no transaction: {out.strip()[-300:]}')
        rows.add(eid, name, 'steady', 'tps', 'transactions/s', value=round(len(lat) / args.churn_seconds, 1))
        rows.add(eid, name, 'steady', 'rows_s', 'rows/s', value=round(len(lat) * (1000 if name == 'append' else 1) / args.churn_seconds))
        rows.add(eid, name, 'steady', 'p99_ms', 'ms', value=round(percentile(lat, 99), 2))
        rows.add(eid, name, 'steady', 'wal_bytes_per_tx', 'bytes', value=round(wal / len(lat)))
        rows.add(eid, name, 'steady', 'window_s', 's', value=round(secs, 1))
        if read:
            rl = latencies_ms(os.path.join(d, 'r'))
            if rl:
                rows.add(eid, name, 'steady', 'read_p50_ms', 'ms', value=round(percentile(rl, 50), 2))
        rows.add(eid, name, 'steady', 'size_mib', 'MiB', value=size_mib(eid))


def cmd_section(args):
    rows, ref = Rows(args.tier), {}
    args.threads = effective(args)
    lanes = [e for e in SECTION_LANES if e in args.engines]
    gate('section', lanes)
    read = load_queries(['03'])[0]
    try:
        for eid in lanes:
            log(f'section on {eid}, {args.rows:,} rows')
            try:
                with Window() as w:
                    load_pg(eid, args, Rows('load'))
                    update_engine(eid, args, rows, ref)
                    if args.churn_seconds:
                        for name, script, rd in (('churn', CHURN, read.statement(eid, args.rows)), ('append', APPEND, None)):
                            load_pg(eid, args, Rows('load'))
                            window(eid, rows, args, name, script, rd)
                why = w.record(rows, '-', f'section-{eid}', w.judge({container(eid)}, io=False))
                if why:
                    NOISY.append(f'section {eid}: {why}')
            except Exception as x:   # recorded; the other lanes still run
                rows.add(eid, '-', 'run', 'error', 'text', value=str(x)[:300])
                log(f'{eid}: {x}')
    finally:
        write(args, rows, 'section', {'churn_seconds': args.churn_seconds, 'probes': args.probes, 'clients': CLIENTS, 'servers': servers(lanes)})
    return 3 if any(r['metric'] == 'result' and str(r['value']).startswith('DIFF') for r in rows.rows) else 0


def cmd_section_merge(args):
    """the result files of every size and repetition of a section pass into one (bench/results/updates.json): the rows as they are, the meta of each file by its tier"""
    runs = [json.load(open(f)) for f in sorted(args.files)]
    first = runs[0]['meta']
    out = dict(meta=dict(host=host(), sha=args.sha, date=first['date'][:10], cpus=first['cpus'], runs={r['meta']['tier']: r['meta'] for r in runs}), rows=[row for r in runs for row in r['rows']])
    with open(args.o, 'w') as f:
        json.dump(out, f)
        f.write('\n')
    log(f'written: {args.o} ({len(runs)} runs, {len(out["rows"])} rows)')
    return 0
