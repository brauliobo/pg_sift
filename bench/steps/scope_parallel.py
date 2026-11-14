#!/usr/bin/env python3
"""The workers of a scope read: time, instructions, memory and blocks of every participant, with 0, 2 and 4 workers, at the default prices and with the prices at zero.

    python3 -I bench/steps/scope_parallel.py build   --root PREFIX --dir D --port N --facts F [--db fx]
    python3 -I bench/steps/scope_parallel.py measure --root PREFIX --dir D --port N --facts F [--tenants 1,2] [--classes a,b] [--rounds 7] [--prices default,zero] --out RAW.tsv
    python3 -I bench/steps/scope_parallel.py table   RAW.tsv [RAW2.tsv ..] [--md FILE]

The fixture is the sift_scope arm of steps/scope_stock_arms.py (`build`). `measure` needs the testing flavour of pg_sift (scripts/pgtest.sh install: SIFT_ROOT/usr is the PREFIX), whose
sift.test_par_stats makes every participant of a parallel read log one line with its instructions, shared blocks, memory and group table (src/agg/par/stats.c), and the leader the
sizes of the shared memory of the statement. A round takes each worker count in turn (the order rotates) and for each runs four statements: warm and cold without the counters (the
executor time of EXPLAIN ANALYZE, which the counters would lengthen by the reads of /proc), and warm and cold with them (the statement's user instructions by perf stat of the backend and
the postmaster, which counts the workers it forks; the instructions, blocks and memory of each participant from the log; pg_stat_database blocks of the statement). Cold is iolib.cool of
every table of the fixture (shared buffers emptied; the page cache too where the data directory is not tmpfs). Each cell is the median of the rounds with its first and third quartile.
The host must be quiet before every cell (cpu and io `some` avg10 < 10%): the pressure it was measured under is a column of the raw file.
"""
import argparse
import os
import re
import signal
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import iolib  # noqa: E402
import scope_stock_arms as S  # noqa: E402

CLASSES = ('by_member', 'top_members', 'distinct_members_by_month', 'filtered_count', 'count_by_score', 'sum_by_month', 'latest_20_rows')
# a read with a group for each event, named with --classes: what a participant's own group table costs when the groups are millions
EXTRA = {'many_groups': 'SELECT id, count(*) FROM events WHERE ' + S.SEMI + ' GROUP BY 1'}
ZERO = 'SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET min_parallel_index_scan_size = 0; SET sift.agg_par_min_rows = 0'
PART = ('instr', 'hit', 'read', 'rss_peak_kb', 'rss_kb', 'pss_kb', 'pss_anon_kb', 'pss_anon_start_kb', 'pss_shmem_kb', 'pss_file_kb', 'table_bytes', 'node_bytes')
DSM = ('segment', 'shared', 'spec', 'params', 'snapshot', 'view', 'queues', 'maps', 'scope', 'other')
COLS = (['facts', 'prices', 'tenant', 'share_pct', 'class', 'workers', 'round', 'kind', 'cache', 'launched', 'ms', 'hit', 'read', 'psi_cpu', 'psi_io', 'load1', 'total_instr', 'stmt_hit', 'stmt_read']
        + ['leader_' + k for k in PART] + ['nworkers'] + ['w_' + k for k in PART] + ['dsm_' + k for k in DSM])


class IoSess:
    """iolib.cool wants the session of steps/duckdb.py: (_, rows, error, _) of run()"""

    def __init__(self, s):
        self.s = s

    def run(self, sql):
        try:
            return None, self.s.run(sql), None, None
        except RuntimeError as e:
            return None, [], str(e), None


class Meter(S.Sess):
    def __init__(self, db, pm):
        super().__init__(db)
        self.pm = pm

    def counted(self, sql):
        """(instructions:u of the backend and of what the postmaster forks while it runs sql, output)"""
        for _ in range(12):
            perfs = [subprocess.Popen(['perf', 'stat', '-x,', '-e', 'instructions:u', '-p', str(p)], stderr=subprocess.PIPE, text=True,
                                      preexec_fn=lambda: signal.signal(signal.SIGINT, signal.SIG_DFL)) for p in (self.pid, self.pm)]
            time.sleep(0.25)
            out = self.run(sql)
            time.sleep(0.15)
            tot = 0
            for pf in perfs:
                pf.send_signal(signal.SIGINT)
                m = re.search(r'^(\d+),,instructions', pf.communicate()[1], re.M)
                tot += int(m.group(1)) if m else 0
            if tot:
                return tot, out
            time.sleep(2)   # the counters of the host are shared with other perf runs
        raise RuntimeError('perf stat gave nothing')


def blocks(s):
    s.run('SELECT pg_stat_force_next_flush()')
    time.sleep(0.15)
    h, r = s.run('SELECT blks_hit, blks_read FROM pg_stat_database WHERE datname = current_database()')[0].split('|')
    return int(h), int(r)


class Log:
    """the lines of the server log after a point"""

    def __init__(self, path):
        self.path = path
        self.mark()

    def mark(self):
        self.pos = os.path.getsize(self.path)

    def since(self):
        with open(self.path, errors='replace') as f:
            f.seek(self.pos)
            return f.read()


def parse(text):
    """{leader: dict, workers: [dict], dsm: dict} of the sift par stats lines"""
    leaders, workers, dsm = [], [], {}
    for line in text.splitlines():
        m = re.search(r'sift par (stats|dsm): (.*)$', line)
        if not m:
            continue
        kv = {k: v for k, v in re.findall(r'(\w+)=(\S+)', m.group(2))}
        if m.group(1) == 'dsm':
            dsm = kv
        elif kv['role'] == 'leader':
            leaders.append(kv)
        else:
            workers.append(kv)
    leader = max(leaders, key=lambda d: int(d['instr'])) if leaders else {}
    return leader, workers, dsm


def pressure():
    out = []
    for r in ('cpu', 'io'):
        with open('/proc/pressure/' + r) as f:
            out.append(re.search(r'some avg10=([\d.]+)', f.readline()).group(1))
    return out + [open('/proc/loadavg').read().split()[0]]


def quiet(io_wait=10, limit=3600):
    """waits for cpu and io `some` avg10 < 10%; the data directory is tmpfs, so when only the io of other runs' disks stays above it for io_wait seconds the cell goes on (the pressure is in the raw file)"""
    t0 = time.time()
    while time.time() - t0 < limit:
        cpu, io = float(pressure()[0]), float(pressure()[1])
        if cpu < 10 and (io < 10 or time.time() - t0 > io_wait):
            return
        time.sleep(5)
    raise SystemExit('host never quiet')


def explain(s, sql):
    out = s.run('EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) ' + sql)
    txt = '\n'.join(out)
    return float(re.search(r'Execution Time: ([\d.]+)', txt).group(1)), max([int(x) for x in re.findall(r'Workers Launched: (\d+)', txt)] or [0]), txt


def run_cell(s, ms_, log, pm, sql, io, tables, base, w, rnd, price_sql):
    """the four statements of one worker count in one round: rows of the raw file"""
    rows = []
    s.run(f'SET max_parallel_workers_per_gather = {w}')
    for cache in ('warm', 'cold'):
        if cache == 'cold':
            iolib.cool(io, tables)
        ms, launched, _ = explain(s, sql)
        rows.append(dict(base, workers=w, round=rnd, kind='time', cache=cache, launched=launched, ms='%.3f' % ms))
        if cache == 'cold':
            iolib.cool(io, tables)
        s.run('SET sift.test_par_stats = on')
        h0, r0 = blocks(s)
        log.mark()
        total, out = ms_.counted('EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) ' + sql)
        h1, r1 = blocks(s)
        s.run('SET sift.test_par_stats = off')
        txt = '\n'.join(out)
        leader, workers, dsm = parse(log.since())
        row = dict(base, workers=w, round=rnd, kind='stats', cache=cache, ms=re.search(r'Execution Time: ([\d.]+)', txt).group(1),
                   launched=max([int(x) for x in re.findall(r'Workers Launched: (\d+)', txt)] or [0]), total_instr=total, stmt_hit=h1 - h0, stmt_read=r1 - r0, nworkers=len(workers))
        row['hit'], row['read'] = int(leader.get('hit', 0)) + sum(int(x['hit']) for x in workers), int(leader.get('read', 0)) + sum(int(x['read']) for x in workers)
        for k in PART:
            row['leader_' + k] = leader.get(k, '')
            row['w_' + k] = ';'.join(x[k] for x in workers)
        for k in DSM:
            row['dsm_' + k] = dsm.get(k, '')
        rows.append(row)
    return rows


def md5(s, sql):
    return s.run(f"SELECT md5(coalesce(string_agg(x::text, '|' ORDER BY x::text), '')) FROM ({sql}) x")[0]


def measure(a):
    cl = S.Cluster(a)
    cl.restart(True)
    pm = int(open(os.path.join(cl.data, 'postmaster.pid')).readline())
    log = Log(os.path.join(a.dir, 'log'))
    s = Meter(a.db, pm)
    s.run('CREATE EXTENSION IF NOT EXISTS pg_buffercache')
    s.run('SHOW sift.test_par_stats')   # an error unless this is the testing flavour
    io = IoSess(s)
    tables = iolib.schema_tables(io, 'public')
    total = int(s.run('SELECT count(*) FROM events')[0])
    classes = {k: v for k, v in {**S.semi_classes(), **EXTRA}.items() if k in a.classes.split(',')}
    out = open(a.out, 'a')
    if os.path.getsize(a.out) == 0:
        out.write('\t'.join(COLS) + '\n')
    ws = [int(x) for x in a.workers.split(',')]
    for price in a.prices.split(','):
        price_sql = ZERO if price == 'zero' else ''
        for t in (int(x) for x in a.tenants.split(',')):
            share = 100.0 * int(s.run(f'SELECT count(*) FROM events WHERE dim_a_id IN (SELECT member FROM tenant_pairs WHERE tenant_id = {t})')[0]) / total
            for c, tmpl in classes.items():
                sql = tmpl.format(t=t)
                quiet()
                p = pressure()
                base = dict(facts=a.facts, prices=price, tenant=t, share_pct='%.1f' % share, **{'class': c}, psi_cpu=p[0], psi_io=p[1], load1=p[2])
                s.run(price_sql or 'RESET parallel_setup_cost; RESET parallel_tuple_cost; RESET min_parallel_table_scan_size; RESET min_parallel_index_scan_size; RESET sift.agg_par_min_rows')
                ref = None
                for w in ws:
                    s.run(f'SET max_parallel_workers_per_gather = {w}')
                    s.run(sql)
                    dig = md5(s, sql)
                    ref = ref or dig
                    if dig != ref:
                        raise RuntimeError(f'{c} t{t} w{w}: other rows than with {ws[0]} workers')
                for rnd in range(a.rounds):
                    order = ws[rnd % len(ws):] + ws[:rnd % len(ws)]
                    for w in order:
                        for r in run_cell(s, s, log, pm, sql, io, tables, base, w, rnd, price_sql):
                            out.write('\t'.join(str(r.get(k, '')) for k in COLS) + '\n')
                        out.flush()
                print(f'{a.facts} {price} t{t} {c}: {a.rounds} rounds, load {p[2]}, psi cpu {p[0]} io {p[1]}', file=sys.stderr, flush=True)
    s.close()
    cl.stop()


def build(a):
    cl = S.Cluster(a)
    os.makedirs(os.path.join(a.dir, 'sock'), exist_ok=True)
    if not os.path.exists(os.path.join(cl.data, 'PG_VERSION')):
        subprocess.run(['initdb', '-D', cl.data, '-U', 'postgres', '--no-sync', '-E', 'UTF8', '--locale=C'], check=True, env=cl.env, stdout=subprocess.DEVNULL)
    cl.restart(True)
    cl.psql('postgres', '-c', f'DROP DATABASE IF EXISTS {a.db} WITH (FORCE)')
    cl.psql('postgres', '-c', f'CREATE DATABASE {a.db}')
    subprocess.run(['psql', '-X', '-q', '-d', a.db, '-v', f'facts={a.facts}', '-v', 'member_total=2648', '-f', os.path.join(S.SQL, 'base.sql')], check=True, env=cl.env,
                   stdout=open(os.path.join(a.dir, 'fixture.txt'), 'w'))
    s = S.Sess(a.db)
    s.script(S.ARMS['sift_scope'][1])
    s.run('VACUUM (ANALYZE) events')
    s.close()
    cl.stop()


# --- the table

def quart(v):
    v = sorted(v)
    if not v:
        return None
    if len(v) == 1:
        return v[0], v[0], v[0]
    q = statistics.quantiles(v, n=4, method='inclusive')
    return q[0], statistics.median(v), q[2]


def nums(x):
    return [float(y) for y in x.split(';')] if x else []


def load(paths):
    rows = []
    for p in paths:
        with open(p) as f:
            head = f.readline().rstrip('\n').split('\t')
            rows += [dict(zip(head, l.rstrip('\n').split('\t'))) for l in f if l.strip()]
    return rows


def cell_stats(rows):
    """{(facts, prices, tenant, class, workers): {metric: [one value for each round]}}"""
    out = {}
    for r in rows:
        k = (int(r['facts']), r['prices'], int(r['tenant']), r['class'], int(r['workers']))
        m = out.setdefault(k, {})

        def add(name, v):
            if v is not None:
                m.setdefault(name, []).append(v)
        c = r['cache']
        if r['kind'] == 'time':
            add('ms_' + c, float(r['ms']))
            add('launched', int(r['launched']))
            continue
        w = {x: nums(r['w_' + x]) for x in PART}
        n = len(w['instr'])
        lead = lambda x: float(r['leader_' + x]) if r['leader_' + x] else 0.0   # noqa: E731
        mean = lambda x: sum(w[x]) / n if n else None   # noqa: E731
        add(f'total_instr_{c}', float(r['total_instr']) / 1e6)
        add(f'leader_instr_{c}', lead('instr') / 1e6)
        add(f'workers_instr_{c}', sum(w['instr']) / 1e6)
        add(f'start_instr_{c}', (float(r['total_instr']) - lead('instr') - sum(w['instr'])) / 1e6)
        add(f'hit_{c}', float(r['hit']))
        add(f'read_{c}', float(r['read']))
        add(f'stmt_hit_{c}', float(r['stmt_hit']))
        add(f'stmt_read_{c}', float(r['stmt_read']))
        add(f'leader_read_{c}', lead('read'))
        add(f'workers_read_{c}', sum(w['read']))
        add(f'worker_read_each_{c}', mean('read'))
        if c == 'warm':
            add('leader_peak_mb', lead('rss_peak_kb') / 1024)
            add('worker_peak_mb', mean('rss_peak_kb') / 1024 if n else None)
            add('leader_grow_mb', (lead('pss_anon_kb') - lead('pss_anon_start_kb')) / 1024)
            add('worker_grow_mb', (mean('pss_anon_kb') - mean('pss_anon_start_kb')) / 1024 if n else None)
            add('worker_anon_mb', mean('pss_anon_kb') / 1024 if n else None)
            add('pss_sum_mb', (lead('pss_kb') + sum(w['pss_kb'])) / 1024)
            add('leader_node_mb', lead('node_bytes') / 2 ** 20)
            add('worker_node_mb', mean('node_bytes') / 2 ** 20 if n else None)
            add('leader_table_mb', lead('table_bytes') / 2 ** 20)
            add('worker_table_mb', mean('table_bytes') / 2 ** 20 if n else None)
            add('node_sum_mb', (lead('node_bytes') + sum(w['node_bytes'])) / 2 ** 20)
            add('dsm_mb', float(r['dsm_segment']) / 2 ** 20 if r['dsm_segment'] else 0.0)
            add('dsm_decision_kb', (float(r['dsm_view']) + float(r['dsm_scope']) + float(r['dsm_shared'])) / 1024 if r['dsm_view'] else 0.0)
            add('dsm_queues_mb', float(r['dsm_queues']) / 2 ** 20 if r['dsm_queues'] else 0.0)
    return out


def fmt(m, k, prec=1):
    """median (first-third quartile) of metric k of a cell"""
    q = quart(m.get(k, []))
    return '-' if q is None else '%.*f (%.*f-%.*f)' % (prec, q[1], prec, q[0], prec, q[2])


def med(m, k):
    q = quart(m.get(k, []))
    return None if q is None else q[1]


def x(m, base, k):
    a, b = med(m, k), med(base, k)
    return '-' if a is None or not b else '%.2f' % (a / b)


def launched(m):
    q = med(m, 'launched')
    return int(q) if q else 0


# one table of each of the five things asked for: (title, header, row function(m, base, w))
TABLES = (
    ('time', '| tenant | shape | workers | launched | ms warm | ms cold | x warm | x cold |',
     lambda m, b, w: '%d | %s | %s | %s | %s' % (launched(m), fmt(m, 'ms_warm'), fmt(m, 'ms_cold', 0), x(m, b, 'ms_warm'), x(m, b, 'ms_cold'))),
    ('instructions (millions, user)', '| tenant | shape | workers | statement | leader | workers | process start, planning, lookups | x statement |',
     lambda m, b, w: '%s | %s | %s | %s | %s' % (fmt(m, 'total_instr_warm'), fmt(m, 'leader_instr_warm'), fmt(m, 'workers_instr_warm') if w else '-', fmt(m, 'start_instr_warm'), x(m, b, 'total_instr_warm'))),
    ('memory (MB)', '| tenant | shape | workers | peak RSS leader | peak RSS each worker | private heap each worker | node memory leader / each worker | group table leader / each worker | shared memory of the read | sum of PSS | x PSS |',
     lambda m, b, w: '%s | %s | %s | %s / %s | %s / %s | %s | %s | %s' % (
         fmt(m, 'leader_peak_mb', 0), fmt(m, 'worker_peak_mb', 0) if w else '-', fmt(m, 'worker_anon_mb') if w else '-', fmt(m, 'leader_node_mb', 2), fmt(m, 'worker_node_mb', 2) if w else '-',
         fmt(m, 'leader_table_mb', 2), fmt(m, 'worker_table_mb', 2) if w else '-', fmt(m, 'dsm_mb') if launched(m) else '-', fmt(m, 'pss_sum_mb', 0), x(m, b, 'pss_sum_mb'))),
    ('blocks', '| tenant | shape | workers | warm blocks (hit + read) | x warm | cold blocks read | cold read by the leader | cold read by each worker | x cold read |',
     lambda m, b, w: '%s | %s | %s | %s | %s | %s' % (fmt(m, 'hit_warm', 0), x(m, b, 'hit_warm'), fmt(m, 'read_cold', 0), fmt(m, 'leader_read_cold', 0), fmt(m, 'worker_read_each_cold', 0) if w else '-', x(m, b, 'read_cold'))),
)


def combined_row(m, b, w):
    """the five things in one row: time, instructions, memory, blocks, and their ratios to the read in one process"""
    q = quart(m.get('ms_warm', []))
    ms = '%.1f (%.1f-%.1f)' % (q[1], q[0], q[2]) if q else '-'
    pct = lambda k: '%+.0f%%' % (100 * (med(m, k) / med(b, k) - 1)) if med(m, k) is not None and med(b, k) else '-'   # noqa: E731
    return ' | '.join([
        str(launched(m)), ms, fmt(m, 'ms_cold', 0), '%s / %s' % (x(m, b, 'ms_warm'), x(m, b, 'ms_cold')),
        '%s + %s' % (fmt(m, 'leader_instr_warm', 0).split(' ')[0], fmt(m, 'workers_instr_warm', 0).split(' ')[0] if w else '0'), fmt(m, 'total_instr_warm', 0).split(' ')[0], x(m, b, 'total_instr_warm'),
        '%s / %s' % (fmt(m, 'leader_node_mb', 1).split(' ')[0], fmt(m, 'worker_node_mb', 1).split(' ')[0] if w else '-'),
        '%s / %s' % (fmt(m, 'worker_anon_mb', 1).split(' ')[0] if w else '-', fmt(m, 'worker_peak_mb', 0).split(' ')[0] if w else '-'),
        fmt(m, 'dsm_mb').split(' ')[0] if launched(m) else '-',
        fmt(m, 'hit_warm', 0).split(' ')[0], x(m, b, 'hit_warm'),
        '%s (%s / %s)' % (fmt(m, 'read_cold', 0).split(' ')[0], fmt(m, 'leader_read_cold', 0).split(' ')[0], fmt(m, 'worker_read_each_cold', 0).split(' ')[0] if w else '-'), x(m, b, 'read_cold')])


COMBINED = ('| tenant | shape | workers | launched | ms warm | ms cold | x ms warm / cold | M instr leader + workers | M instr statement | x instr | node memory MB leader / each worker | private heap / peak RSS MB of each worker '
            '| shared memory MB | blocks warm | x blocks | cold blocks read (leader / each worker) | x cold read |')


def table(paths, md=None, only=None, combined=False):
    cells = cell_stats(load(paths))
    out = []
    keys = sorted({k[:4] for k in cells}, key=lambda k: (-k[0], k[1] != 'default', k[2], (CLASSES + ('many_groups',)).index(k[3])))
    for facts in sorted({k[0] for k in keys}, reverse=True):
        for price in ('default', 'zero'):
            ks = [k for k in keys if k[0] == facts and k[1] == price]
            if not ks or only and (facts, price) not in only:
                continue
            what = f'{facts / 1e6:g} M events, ' + ('default prices' if price == 'default' else 'every price at zero')
            for title, head, row in ((('', COMBINED, combined_row),) if combined else TABLES):
                out.append(f'\n**{what}{": " + title if title else ""}.** Median (first-third quartile for the milliseconds) of the rounds; x is the value with workers divided by the value of the read in one process (below 1 is less, for the time a speedup of 1/x).\n')
                out.append(head)
                out.append('|' + '---|' * head.count('|') if False else '|' + '---|' * (head.count('|') - 1))
                for (f, p, t, c) in ks:
                    base = cells.get((f, p, t, c, 0), {})
                    for w in (0, 2, 4):
                        m = cells.get((f, p, t, c, w))
                        if m:
                            out.append('| %s | %s | %d | %s |' % (t, c, w, row(m, base, w)))
    text = '\n'.join(out) + '\n'
    if md:
        open(md, 'w').write(text)
    print(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd')
    ap.add_argument('paths', nargs='*')
    ap.add_argument('--root', action='append', default=[])
    ap.add_argument('--dir')
    ap.add_argument('--port', type=int)
    ap.add_argument('--facts', type=int, default=2400000)
    ap.add_argument('--db', default='fx')
    ap.add_argument('--tenants', default='1,2')
    ap.add_argument('--classes', default=','.join(CLASSES))
    ap.add_argument('--workers', default='0,2,4')
    ap.add_argument('--prices', default='default,zero')
    ap.add_argument('--rounds', type=int, default=7)
    ap.add_argument('--out')
    ap.add_argument('--md')
    ap.add_argument('--combined', action='store_true')
    ap.add_argument('--only', help='table: FACTS:PRICES,.. (10000000:default)')
    a = ap.parse_args()
    if a.cmd == 'table':
        return table(a.paths, a.md, {(int(x.split(':')[0]), x.split(':')[1]) for x in a.only.split(',')} if a.only else None, a.combined)
    a.arms, a.base = 'sift_scope', 'base'
    {'build': build, 'measure': measure}[a.cmd](a)


if __name__ == '__main__':
    main()
