# A run of a script: the sessions, the checks and the verdict.

import collections
import re

from .session import Hang, Session, create_db, errors, psql
from .gen import setup_sql

PLAIN = ['SET sift.enable_agg = off', 'SET sift.enable_col_scan = off', 'SET enable_indexscan = off', 'SET enable_bitmapscan = off', 'SET enable_indexonlyscan = off',
         'SET max_parallel_workers_per_gather = 0']
SCAN = ['SET sift.enable_agg = on', 'SET sift.agg_force = on', 'SET sift.enable_col_scan = on', 'SET max_parallel_workers_per_gather = 0']
# the reference is the plain heap plan; the variants are the scan forced, the default plan, an index scan (the point lookups) and the scan with workers
VARIANTS = [('plain', PLAIN), ('scan', SCAN), ('default', ['SET sift.enable_agg = on']),
            ('index', ['SET sift.enable_agg = off', 'SET enable_seqscan = off', 'SET max_parallel_workers_per_gather = 0']),
            ('workers', SCAN[:3] + ['SET max_parallel_workers_per_gather = 2', 'SET parallel_setup_cost = 0', 'SET parallel_tuple_cost = 0', 'SET min_parallel_table_scan_size = 0',
                                   'SET min_parallel_index_scan_size = 0'])]
RESET = ['sift.enable_agg', 'sift.agg_force', 'sift.enable_col_scan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan', 'enable_seqscan', 'max_parallel_workers_per_gather',
         'parallel_setup_cost', 'parallel_tuple_cost', 'min_parallel_table_scan_size', 'min_parallel_index_scan_size']
ERRORS = collections.Counter()      # the statement errors of the runs, by message: a generator that raises many of one kind wastes the script
CRASH = re.compile(r'server closed the connection|connection to server was lost|terminating connection|crash of another server|server process .* was terminated')


def queries(c, a):
    k = c.key
    pts = ', '.join(k(i) for i in a['pts'])
    lo, hi = a['rng']
    return [('groups', 'SELECT tenant, count(*), sum(amount), min(amount), max(amount) FROM t GROUP BY 1 ORDER BY 1'),
            ('totals', 'SELECT count(*), sum(amount), sum(tenant) FROM t'),
            ('points', f'SELECT k, tenant, amount FROM t WHERE k IN ({pts}) ORDER BY 1'),
            ('top', 'SELECT k, amount FROM t ORDER BY amount DESC, k LIMIT 15'),
            ('range', f'SELECT count(*), sum(amount) FROM t WHERE k BETWEEN {k(lo)} AND {k(hi)}'),
            ('tenant', f"SELECT count(*), min(amount) FROM t WHERE tenant = {a['ten']}")]


def check_sql(c, a):
    s = []
    for v, sets in VARIANTS:
        s += sets
        for q, text in queries(c, a):
            s += [f'\\echo @@Q {v} {q}', text + ';']
        s += [f'RESET {g};' for g in RESET]
    return '\n'.join(x if x.endswith(';') or x.startswith('\\') else x + ';' for x in s)


def compare(out):
    """-> None or a description of the first query whose rows differ from the plain heap plan's"""
    res, cur = {}, None
    for l in out:
        m = re.match(r'@@Q (\w+) (\w+)', l)
        if m:
            cur = res.setdefault((m[1], m[2]), [])
        elif cur is not None:
            cur.append(l)
    for (v, q), rows in res.items():
        ref = res.get(('plain', q))
        if v != 'plain' and ref is not None and rows != ref and not (errors(rows) and errors(ref) and len(rows) == len(ref) and
                                                                     all('aborted' in e for e in errors(rows) + errors(ref))):
            return f'{v} differs from plain in {q}: plain {ref[:8]} | {v} {rows[:8]}'
    return None


def release_prepared(db=None):
    """Rolls back the prepared transactions of the database (of every database): one left pending holds the horizon of dead rows back, and hides what the next run would show"""
    out, _ = psql('postgres', 'SELECT gid, database FROM pg_prepared_xacts' + ('' if db is None else f" WHERE database = '{db}'"), check=False)
    for line in out.split('\n'):
        if '|' in line:
            gid, d = line.split('|')
            psql(d, f"ROLLBACK PREPARED '{gid}'", check=False)


def build_base(c, name):
    create_db(name)
    psql(name, setup_sql(c))


def run(c, acts, db, stop_at_first=True):
    """Runs the script on the database db (built from setup_sql); returns (None | (index of the action, text), checks, errors)"""
    sess, nchecks, nerr = {}, 0, 0

    def get(s):
        if s not in sess:
            sess[s] = Session(db)
        return sess[s]

    fail = None
    try:
        for i, a in enumerate(acts):
            if a['t'] == 'reconnect':
                if a['s'] in sess:
                    sess.pop(a['s']).close()
                continue
            fresh = a['s'] == 'F'
            s = Session(db) if fresh else get(a['s'])
            try:
                if a['t'] == 'sql':
                    out = s.run(a['sql'])
                    nerr += len(errors(out))
                    ERRORS.update(re.sub(r'\d+', 'N', e)[:110] for e in errors(out))
                else:
                    if errors(s.run('SELECT 1')):
                        continue
                    nchecks += 1
                    out = s.run(check_sql(c, a))
                    d = compare(out)
                    if d:
                        fail = (i, d)
                if any(CRASH.search(l) for l in out):
                    fail = (i, 'the server crashed: ' + ' | '.join(l for l in out if CRASH.search(l))[:300])
            except Hang as e:
                fail = (i, f'HANG: {e}')
            finally:
                if fresh:
                    s.close()
            if fail and stop_at_first:
                break
    finally:
        for s in sess.values():
            s.close()
        release_prepared(db)
    return fail, nchecks, nerr
