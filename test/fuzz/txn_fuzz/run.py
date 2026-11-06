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
RESET = ['plan_cache_mode', 'sift.enable_agg', 'sift.agg_force', 'sift.enable_col_scan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan', 'enable_seqscan', 'max_parallel_workers_per_gather',
         'parallel_setup_cost', 'parallel_tuple_cost', 'min_parallel_table_scan_size', 'min_parallel_index_scan_size']
ERRORS = collections.Counter()      # the statement errors of the runs, by message: a generator that raises many of one kind wastes the script
ENV = re.compile(r'Disk quota|No space left|could not resize shared memory|out of memory|too many open files')   # the machine, not sift
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


def cursor_queries(c, a):
    """The reads of a cursor: ordered, so that the rows a plan returns are the rows of the others; the last two run the scan again for each row of an outer query"""
    k = c.key
    lo, hi = a['rng']
    pts = ', '.join(k(i) for i in a['pts'])
    return [f'SELECT k, tenant, amount FROM t WHERE k BETWEEN {k(lo)} AND {k(lo + 60)} ORDER BY k',
            'SELECT tenant, count(*), sum(amount) FROM t GROUP BY 1 ORDER BY 1',
            'SELECT k, amount FROM t ORDER BY amount DESC, k LIMIT 25',
            f'SELECT k, tenant, amount FROM t WHERE k IN ({pts}) ORDER BY k',
            f'SELECT tenant, count(*) FROM t WHERE k IN (SELECT {k("i")} FROM generate_series({lo}, {lo + 300}, 7) i) GROUP BY 1 ORDER BY 1',
            f'SELECT s.v, (SELECT count(*) FROM t WHERE tenant = s.v), (SELECT max(amount) FROM t WHERE tenant = s.v AND k <= {k(hi)}) FROM generate_series(0, 9) s(v) ORDER BY 1',
            f'SELECT s.v, x.n, x.m FROM generate_series(0, 9) s(v) CROSS JOIN LATERAL (SELECT count(*) AS n, sum(amount) AS m FROM t WHERE tenant = s.v AND k BETWEEN {k(lo)} AND {k(hi)}) x ORDER BY 1']


def prep_queries(c):
    """(parameter types, text) of the prepared statements: the parameters are in the key, the tenant and the amount, so that the generic plan is a plan of the scan too"""
    k = c.key
    return [('int', 'SELECT tenant, count(*), sum(amount) FROM t WHERE amount >= $1 GROUP BY 1 ORDER BY 1'),
            ('int, int', f"SELECT count(*), sum(amount) FROM t WHERE k BETWEEN {k('$1')} AND {k('$1 + $2')}"),
            ('int, int, int', f"SELECT k, tenant, amount FROM t WHERE k IN ({k('$1')}, {k('$2')}, {k('$3')}) ORDER BY 1"),
            ('int', 'SELECT k, amount FROM t WHERE tenant = $1 ORDER BY amount DESC, k LIMIT 10'),
            ('int, int', 'SELECT count(*), min(amount), max(amount) FROM t WHERE tenant = $1 AND amount < $2'),
            ('int, int', f"SELECT tenant, count(*) FROM t WHERE k IN (SELECT {k('i')} FROM generate_series($1, $1 + $2, 3) i) GROUP BY 1 ORDER BY 1")]


# the action of a cursor check: BEGIN (outside a transaction), DECLARE .. SCROLL CURSOR, the moves, COMMIT; WITH HOLD cursors are read after the COMMIT
def cursor_check(c, a, v, intxn):
    q = cursor_queries(c, a)[a['q']]
    hold = a['hold'] and not intxn
    decl = f"DECLARE tfk SCROLL CURSOR {'WITH HOLD ' if hold else ''}FOR {q};"
    s = [] if intxn else ['BEGIN;']
    s.append(decl)
    for i, op in enumerate(a['ops']):
        if hold and i == 2:
            s.append('COMMIT;')
        s += [f'\\echo @@Q {v} c{i}', f'{op} IN tfk;']
    if hold and len(a['ops']) <= 2:
        s.append('COMMIT;')
    s.append('CLOSE tfk;')
    if not intxn and not hold:
        s.append('COMMIT;')
    return s


def prep_check(c, a, v):
    types, text = prep_queries(c)[a['q']]
    name = f"tfp_{v}_{a['q']}_{a['pcm'][:3]}"
    s = [] if a['pcm'] == 'auto' else [f"SET plan_cache_mode = {a['pcm']};"]
    # a prepared statement lives as long as the session: the DO block creates it once and the executions of the later checks run the plan it cached (the sixth may be the generic one)
    s.append(f"DO $do$ BEGIN EXECUTE $p$PREPARE {name}({types}) AS {text}$p$; EXCEPTION WHEN duplicate_prepared_statement THEN NULL; END $do$;")
    for i, args in enumerate(a['args']):
        s += [f'\\echo @@Q {v} p{i}', f"EXECUTE {name}({', '.join(map(str, args))});"]
    return s


def check_sql(c, a, intxn=False):
    s = []
    kind = a.get('kind', 'plain')
    for v, sets in VARIANTS:
        s += sets
        if kind == 'plain':
            for q, text in queries(c, a):
                s += [f'\\echo @@Q {v} {q}', text + ';']
        elif kind == 'cursor':
            s += cursor_check(c, a, v, intxn)
        elif kind == 'prep':
            s += prep_check(c, a, v)
        elif kind == 'curdecl':
            s += [f"SELECT count(*) = 0 AS free FROM pg_cursors WHERE name = 'tfc_{v}' \\gset", '\\if :free', f'\\echo @@Q {v} decl',
                  f"DECLARE tfc_{v} SCROLL CURSOR {'WITH HOLD ' if a['hold'] else ''}FOR {cursor_queries(c, a)[a['q']]};", '\\endif']
        else:
            # the cursor of a plan that is not there (the transaction ended, a savepoint rolled it back) is said, not an error that aborts the transaction under the other plans
            s += [f"SELECT count(*) = 1 AS has FROM pg_cursors WHERE name = 'tfc_{v}' \\gset", '\\if :has']
            for i, op in enumerate(a['ops'] if kind == 'curfetch' else ['CLOSE']):
                s += [f'\\echo @@Q {v} f{i}', f'{op} IN tfc_{v};' if kind == 'curfetch' else f'CLOSE tfc_{v};']
            s += ['\\else', f'\\echo @@Q {v} missing', '\\endif']
        s += [f'RESET {g};' for g in RESET]
    return '\n'.join(x if x.endswith(';') or x.startswith('\\') or x.endswith('\\gset') else x + ';' for x in s)


def tx_after(intxn, sql):
    """Is a session in a transaction block after the statements of sql (as a reproducer is read; a run asks the server)"""
    first = sql.lstrip().split(None, 1)[0].upper().rstrip(';') if sql.strip() else ''
    if first in ('BEGIN', 'START'):
        return True
    if first in ('COMMIT', 'ROLLBACK', 'END', 'ABORT') and not re.match(r'\s*(COMMIT|ROLLBACK|END|ABORT)\s+(TO|PREPARED)\b', sql, re.I):
        return False
    if re.match(r'\s*PREPARE\s+TRANSACTION\b', sql, re.I):
        return False
    return intxn


def in_transaction(s):
    """Asks the session: a SAVEPOINT is an error outside a transaction block"""
    out = s.run('SAVEPOINT tfp')
    if any('transaction blocks' in e for e in errors(out)):
        return False
    s.run('RELEASE tfp')
    return True


def compare(out):
    """-> None or a description of the first query whose rows differ from the plain heap plan's"""
    res, cur = {}, None
    if any('lock timeout' in l for l in out):
        return None             # the check waited for a lock of the other session: its queries after the timeout run in an aborted transaction and say nothing of sift
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
                    if a.get('kind') == 'curdecl' and not in_transaction(s):
                        continue
                    out = s.run(check_sql(c, a, a.get('kind') == 'cursor' and in_transaction(s)))
                    if any(ENV.search(l) for l in out):
                        fail = (i, 'ENVIRONMENT: ' + ' | '.join(l for l in out if ENV.search(l))[:300])
                        break
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
