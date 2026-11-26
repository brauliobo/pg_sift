#!/usr/bin/env python3
"""Planner shape matrix: no statement shape may crash the backend, raise an internal error or return other rows than the plain plan.

  test/fuzz/shapes_matrix.py [--format f2|f1|release] [--tier quick|full] [--chunk K/N] [--only RE] [--budget SEC] [--list] [--show N] [--facts a,b] [--no-schema] [--jobs 4]

The statements are enumerated (no randomness) over test/fuzz/shapes_matrix/schema.sql: a fact table of every index availability on the join column (none, btree,
partial, expression, covering, sift_col with scope, connect and projection options, BM25, inheritance parent, partitioned, sift_table, sift_table with BM25), joined to a source (table,
CTE, subquery, outer-join subquery with a COALESCE key, VALUES, function scan, view, outer-join range table, pairs) by every join type and key form, then
crossed in rotation with the output shapes (grouping, DISTINCT, windows, ordering, LIMIT, DESC top-N), the predicates and the planner settings; the fact table
also sits inside a correlated LATERAL, a correlated subquery and a Materialize (a Param rescan of the pushdown). Each statement runs
as EXPLAIN (COSTS OFF) and for real under its settings, and its rows are compared with the rows under sift.enable_scan = off, sift.enable_agg = off
(the ordered md5 of the rows when the statement has a total order, else the sorted rows). Needs the scripts/pgtest.sh cluster (SIFT_TMP, SIFT_PORT).
--format release runs on the release library: a sift.test_* / sift.agg_* setting the library lacks is left out of the configurations (test/fuzz/fmt.py).
Exit 1 on a signal, an internal error (XX000, "cache lookup failed", "unrecognized node", "could not find", "sift.score() must be computed", "plan ... failed"),
an error the plain plan does not raise, a timeout or other rows; the failure names the statement number (--show N prints it), the settings and the first differing row.
"""
import argparse
import hashlib
import itertools
import os
import re
import select
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fmt  # noqa: E402

TMP = os.environ.get('SIFT_TMP', '/tmp/sift-arch')
CLIENT_OPTIONS = '-c client_min_messages=warning -c lc_messages=C'
ENV = fmt.libpq_env(dict(PGHOST=f'{TMP}/sock', PGPORT=os.environ.get('SIFT_PORT', '56100'), PGUSER='postgres', PGOPTIONS=CLIENT_OPTIONS))
LIB = None
# the indexes whose declaration a format may refuse: a refusal is printed (UNSUPPORTED) and the fact table keeps its other indexes, so that the rest of the matrix runs
OPTIONAL_INDEXES = {'connect': "CREATE INDEX ON ps_events_connect USING sift_col (id, dim_a_id, tenant_id, created_at, kind, amount, qty, score, flag) "
                               "WITH (connect = 'dim_a_id -> ps_entities(id)')"}
# the limit of one statement of the matrix; a sanitizer build runs the reference plan several times slower, so it gets six times as long (SIFT_STATEMENT_TIMEOUT, in seconds, sets it)
STATEMENT_SECS = int(os.environ.get('SIFT_STATEMENT_TIMEOUT') or (120 if os.environ.get('SANITIZE') else 20))
READ_TIMEOUT = float(STATEMENT_SECS + 100)
BAD = re.compile(r'cache lookup failed|unrecognized|could not find|must be computed|plan .* failed|invalid memory|could not open relation|'
                 r'variable not found|no relation entry|negative bitmapset|was not found|bogus|unsupported (node|path)')
FACTS = ['none', 'btree', 'partial', 'expr', 'cover', 'scope', 'connect', 'proj', 'bm25', 'inh', 'part', 'tbl', 'tbl_bm25']
FACT_TABLE = {f: f'ps_events_{f}' for f in FACTS}
BM25_FACTS = {'bm25', 'tbl_bm25'}
PAR = ['max_parallel_workers_per_gather=2', 'parallel_setup_cost=0', 'parallel_tuple_cost=0', 'min_parallel_table_scan_size=0',
       'min_parallel_index_scan_size=0']
ENABLES = ['nestloop', 'hashjoin', 'mergejoin', 'indexscan', 'bitmapscan', 'seqscan', 'indexonlyscan', 'sort', 'hashagg', 'material', 'memoize',
           'gathermerge', 'incremental_sort', 'tidscan', 'partitionwise_join', 'partitionwise_aggregate', 'parallel_hash', 'async_append',
           'presorted_aggregate', 'group_by_reordering', 'self_join_elimination']
NESTLOOP = ['enable_hashjoin=off', 'enable_mergejoin=off']
BASELINE = ['sift.enable_scan=off', 'sift.enable_agg=off', 'max_parallel_workers_per_gather=0']
BASELINE_SCORE = ['sift.enable_agg=off', 'max_parallel_workers_per_gather=0']


class Crash(Exception):
    pass


class Session:
    """psql -At on a pipe: run(sql) sends the text and a sentinel and returns the lines before it."""

    def __init__(self, db):
        self.db, self.n, self.buf = db, 0, b''
        self.p = subprocess.Popen(['psql', '-X', '-q', '-At', '-d', db, '-v', 'ON_ERROR_STOP=0'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1, env=ENV)
        self.fd = self.p.stdout.fileno()
        self.run('\\set VERBOSITY verbose')

    def run(self, sql):
        self.n += 1
        tag = f'@@END {self.n}'
        try:
            self.p.stdin.write(sql.rstrip() + f'\n\\echo {tag}\n')
            self.p.stdin.flush()
        except BrokenPipeError:
            raise Crash('psql ended')
        out, end = [], time.time() + READ_TIMEOUT
        while True:
            while b'\n' not in self.buf:
                left = end - time.time()
                if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                    self.p.kill()
                    raise Crash('no answer in %ds' % READ_TIMEOUT)
                data = os.read(self.fd, 1 << 20)
                if not data:
                    raise Crash('connection ended')
                self.buf += data
            line, _, self.buf = self.buf.partition(b'\n')
            line = line.decode(errors='replace')
            if line == tag:
                return out
            if 'server closed the connection' in line or 'connection to server was lost' in line or 'terminating connection' in line:
                raise Crash(line)
            out.append(line)

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=20)
        except Exception:
            self.p.kill()


def wait_ready(db, secs=90):
    end = time.time() + secs
    while time.time() < end:
        if subprocess.run(['pg_isready', '-q', '-d', db], env=ENV).returncode == 0:
            r = subprocess.run(['psql', '-X', '-qAt', '-d', db, '-c', 'SELECT 1'], env=ENV, capture_output=True, text=True)
            if r.returncode == 0:
                return True
        time.sleep(0.5)
    return False


# ---------------------------------------------------------------- the statement shapes

# a source: the text of the relation, an optional WITH prefix, and the SQL of its key (K), second key (K2) and group (G) columns
SOURCES = {
    'ent':       dict(prefix='', rel='ps_entities s', K='s.id', K2='s.prop', G='s.grp'),
    'cte':       dict(prefix='WITH s AS (SELECT id AS k, prop AS k2, grp AS g FROM ps_entities WHERE id < 30) ', rel='s', K='s.k', K2='s.k2', G='s.g'),
    'cte_oj':    dict(prefix='WITH s AS (SELECT a.id AS k0, coalesce(a.prop, g.prop) AS k, g.prop AS k2, a.grp AS g FROM ps_entities a '
                             'LEFT JOIN ps_groups g ON g.grp = a.grp WHERE a.id < 40) ', rel='s', K='s.k', K2='s.k2', G='s.g'),
    'sub_oj':    dict(prefix='', rel='(SELECT a.id AS k0, coalesce(a.prop, g.prop) AS k, g.prop AS k2, a.grp AS g FROM ps_entities a '
                                     'LEFT JOIN ps_groups g ON g.grp = a.grp WHERE a.id < 40) s', K='s.k', K2='s.k2', G='s.g'),
    'sub_full':  dict(prefix='', rel='(SELECT coalesce(a.id, g.prop) AS k, g.prop AS k2, coalesce(a.grp, g.grp) AS g FROM ps_entities a '
                                     'FULL JOIN ps_groups g ON g.prop = a.id) s', K='s.k', K2='s.k2', G='s.g'),
    'values':    dict(prefix='', rel='(VALUES (1, 2, 1), (5, NULL, 2), (NULL, 3, 3), (7, 7, 4), (41, 5, 0)) s(k, k2, g)', K='s.k', K2='s.k2', G='s.g'),
    'unnest':    dict(prefix='', rel='unnest(ARRAY[3, 7, NULL, 12, 40, 41]) WITH ORDINALITY s(k, o)', K='s.k', K2='s.o', G='s.o'),
    'series':    dict(prefix='', rel='generate_series(1, 40) s(k)', K='s.k', K2='s.k', G='s.k % 5'),
    'view':      dict(prefix='', rel='ps_view s', K='s.prop', K2='s.id', G='s.grp'),
    'oj_left':   dict(prefix='', rel='(ps_entities a LEFT JOIN ps_groups g ON g.grp = a.grp)', K='coalesce(a.prop, g.prop)', K2='g.prop', G='a.grp'),
    'oj_full':   dict(prefix='', rel='(ps_entities a FULL JOIN ps_groups g ON g.prop = a.id)', K='coalesce(a.id, g.prop)', K2='g.prop', G='g.grp'),
    'oj_right':  dict(prefix='', rel='(ps_entities a RIGHT JOIN ps_groups g ON g.prop = a.id)', K='g.prop', K2='a.id', G='g.grp'),
    'pairs':     dict(prefix='', rel='ps_pairs s', K='s.dim_a_id', K2='s.tenant_id', G='s.tenant_id'),
    'agg_sub':   dict(prefix='', rel='(SELECT grp AS k, max(prop) AS k2, count(*) AS g FROM ps_entities GROUP BY grp) s', K='s.k', K2='s.k2', G='s.g'),
    'setop':     dict(prefix='', rel='(SELECT id AS k, prop AS k2, grp AS g FROM ps_entities WHERE id < 10 UNION SELECT grp, id, tenant_id FROM '
                                     'ps_entities WHERE id > 50) s', K='s.k', K2='s.k2', G='s.g'),
}

# the join of the fact key to the source: (left side, right side) of an equality, or a whole condition built from K, K2, G
EQ_FORMS = {
    'plain':      ('f.dim_a_id', '{K}'),
    'coalesce_f': ('coalesce(f.dim_a_id, 0)', '{K}'),
    'expr':       ('f.dim_a_id + 0', '{K}'),
    'cast':       ('f.dim_a_id::bigint', '{K}::bigint'),
    'func':       ('abs(f.dim_a_id)', '{K}'),
    'text':       ('f.dim_a_id::text', '{K}::text'),
    'coalesce_s': ('f.dim_a_id', 'coalesce({K}, {K2})'),
    'k2':         ('f.dim_a_id', '{K2}'),
}
COND_FORMS = {
    'two':       'f.dim_a_id = {K} AND f.qty = {G}',
    'nonequi':   'f.dim_a_id < {K}',
    'eqconst':   'f.dim_a_id = {K} AND f.dim_a_id = 7',
    'param':     'f.dim_a_id = {K} AND f.tenant_id = $1',
    'nullsafe':  'f.dim_a_id IS NOT DISTINCT FROM {K}',
    'or':        '(f.dim_a_id = {K} OR f.tenant_id = {G})',
    'const':     'f.dim_a_id = 7 AND {K} IS NOT NULL',
    'param_only': 'f.dim_a_id = $1 AND {K} IS NOT NULL',
}
JOIN_TYPES = ['inner', 'left', 'right', 'full', 'semi_exists', 'semi_in', 'anti_exists', 'anti_in', 'lat_inner', 'lat_left', 'cross']
NO_S = {'semi_exists', 'semi_in', 'anti_exists', 'anti_in'}
IN_TYPES = {'semi_in', 'anti_in'}


def build_join(jt, src, form):
    """(prefix, from, where, exposure) of a join of the fact f to a source by a key form; None when the combination does not exist"""
    s = SOURCES[src]
    if form in EQ_FORMS:
        lhs, rhs = EQ_FORMS[form]
        lhs, rhs, cond = lhs, rhs.format(**s), None
        cond = f'{lhs} = {rhs}'
    else:
        if jt in IN_TYPES:
            return None
        lhs, cond = None, COND_FORMS[form].format(**s)
    K, K2, G = s['K'], s['K2'], s['G']
    expo = dict(K=K, K2=K2, G=G)
    f = 'FROM {F} f'
    if jt in ('inner', 'left', 'right', 'full'):
        kind = {'inner': 'JOIN', 'left': 'LEFT JOIN', 'right': 'RIGHT JOIN', 'full': 'FULL JOIN'}[jt]
        return s['prefix'], f'{f} {kind} {s["rel"]} ON {cond}', '', expo
    if jt == 'cross':
        return s['prefix'], f'{f} CROSS JOIN {s["rel"]}', cond, expo
    if jt in ('semi_exists', 'anti_exists'):
        neg = 'NOT ' if jt == 'anti_exists' else ''
        return s['prefix'], f, f'{neg}EXISTS (SELECT 1 FROM {s["rel"]} WHERE {cond})', None
    if jt in IN_TYPES:
        neg = 'NOT ' if jt == 'anti_in' else ''
        return s['prefix'], f, f'{lhs} {neg}IN (SELECT {rhs} FROM {s["rel"]})', None
    kind = 'JOIN' if jt == 'lat_inner' else 'LEFT JOIN'
    inner = f'SELECT {K} AS k, {K2} AS k2, {G} AS g FROM {s["rel"]} WHERE {cond} ORDER BY 1, 2, 3 LIMIT 2'
    return s['prefix'], f'{f} {kind} LATERAL ({inner}) l ON true', '', dict(K='l.k', K2='l.k2', G='l.g')


# the output: select list, GROUP BY, HAVING, ORDER BY, LIMIT; total: the order is a total order of the rows; s: uses the source columns
def T(sel, group='', having='', order='', limit='', total=False, s=False, only=None):
    return dict(sel=sel, group=group, having=having, order=order, limit=limit, total=total, s=s, only=only)


TAILS = [
    T('count(*)', total=True),
    T('count(*), sum(f.amount), min(f.qty), max(f.created_at)', total=True),
    T('f.kind, count(*), sum(f.amount)', 'f.kind', order='1', total=True),
    T('f.tenant_id, f.kind, count(*), sum(f.qty)', 'f.tenant_id, f.kind', order='1, 2', total=True),
    T("date_trunc('month', f.created_at) AS m, count(*)", '1', order='1', total=True),
    T('f.qty % 3 AS q, count(*), avg(f.amount)', 'f.qty % 3', order='1', total=True),
    T('DISTINCT f.kind, f.tenant_id', order='1, 2', total=True),
    T('DISTINCT ON (f.kind) f.kind, f.id', order='f.kind, f.id', total=True),
    T('f.tenant_id, f.kind, count(*)', 'GROUPING SETS ((f.kind), (f.tenant_id), ())', total=False),
    T('f.tenant_id, f.kind, sum(f.amount)', 'ROLLUP (f.tenant_id, f.kind)'),
    T('f.tenant_id, f.flag, count(*)', 'CUBE (f.tenant_id, f.flag)'),
    T('f.id, sum(f.amount) OVER (PARTITION BY f.kind ORDER BY f.id) AS run', order='f.id', limit='LIMIT 25', total=True),
    T('f.kind, count(*) AS n, rank() OVER (ORDER BY count(*) DESC, f.kind) AS r', 'f.kind', order='1', total=True),
    T('f.kind, count(*), sum(f.amount)', 'f.kind', having='count(*) > 20 AND sum(f.amount) > 100', order='1', total=True),
    T('count(DISTINCT f.dim_a_id), count(DISTINCT f.kind), count(DISTINCT f.tenant_id)', total=True),
    T("f.tenant_id, count(*) FILTER (WHERE f.flag), sum(f.qty) FILTER (WHERE f.kind = 'k1')", 'f.tenant_id', order='1', total=True),
    T("f.kind, array_agg(f.id ORDER BY f.id), string_agg(DISTINCT f.tags[1], ',' ORDER BY f.tags[1])", 'f.kind', order='1', total=True),
    T('f.tenant_id, percentile_cont(0.5) WITHIN GROUP (ORDER BY f.amount), mode() WITHIN GROUP (ORDER BY f.kind)', 'f.tenant_id', order='1', total=True),
    T('f.id, unnest(f.tags) AS t', order='f.id, t', limit='LIMIT 30', total=True),
    T('f.id, generate_series(1, f.qty % 3) AS g', order='f.id, g', limit='LIMIT 30', total=True),
    T('f.id, f.amount', order='f.amount DESC, f.id', limit='LIMIT 10 OFFSET 5', total=True),
    T('f.id, f.amount', order='f.id DESC', limit='LIMIT 12', total=True),
    T('f.id, f.created_at', order='f.created_at DESC, f.id DESC', limit='LIMIT 20 OFFSET 3', total=True),
    T('f.kind, f.id', order='f.kind DESC, f.id DESC', limit='LIMIT 9', total=True),
    T('f.tenant_id, max(f.amount), count(*)', 'f.tenant_id', order='2 DESC NULLS LAST, 1', limit='LIMIT 3', total=True),
    T('f.id, f.kind', order='f.kind', limit='FETCH FIRST 5 ROWS WITH TIES'),
    T('f.id, f.amount', order='f.dim_a_id NULLS FIRST, f.id', limit='LIMIT 15', total=True),
    T('f.id', order='f.id', limit='LIMIT 20', total=True),
    T('f.kind, round(avg(f.score)::numeric, 4), max(f.attrs ->> \'a\')', 'f.kind', order='1', total=True),
    T('count(*), (SELECT count(*) FROM ps_tenants), (SELECT max(id) FROM ps_entities)', total=True),
    T('f.kind, count(*)', 'f.kind', order='count(*) DESC, f.kind', limit='LIMIT 3', total=True),
    T('f.created_at::date AS d, count(*), sum(f.amount)', '1', order='1', limit='LIMIT 20', total=True),
    T('f.tags, count(*)', 'f.tags', order='1', total=True),
    T('f.attrs, count(*)', 'f.attrs', order='1', total=True),
    T('{K}, count(*)', '{K}', order='1', s=True, total=True),
    T('{K}, f.kind, sum(f.amount)', '{K}, f.kind', order='1, 2', s=True, total=True),
    T('count(DISTINCT {K}), sum({K2})', s=True, total=True),
    T('coalesce({K}, -1) AS k, count(*), max({G})', 'coalesce({K}, -1)', order='1', s=True, total=True),
    T('{G}, count(*), sum(f.amount)', '{G}', order='1', s=True, total=True),
    T('f.id, {K}, {K2}', order='f.id, 2, 3', limit='LIMIT 20', s=True, total=True),
    T('f.tenant_id, count(*)', 'f.tenant_id', having='count(DISTINCT {K}) > 1', order='1', s=True, total=True),
    T('f.kind, sum({K}) FILTER (WHERE {G} > 1)', 'f.kind', order='1', s=True, total=True),
    T('f.id, sift.score(f.ctid) AS sc', order='sc DESC, f.id', limit='LIMIT 10', total=True, only=BM25_FACTS),
    T('f.kind, count(*), max(sift.score(f.ctid))', 'f.kind', order='1', total=True, only=BM25_FACTS),
]

PREDS = [
    ('none', ''), ('eq', 'f.tenant_id = 2'), ('in', 'f.tenant_id IN (1, 3)'), ('any', 'f.tenant_id = ANY (ARRAY[2, 4])'),
    ('range', "f.created_at >= '2026-03-01' AND f.created_at < '2026-04-15'"), ('between', 'f.amount BETWEEN 10 AND 60'),
    ('isnull', 'f.dim_a_id IS NULL'), ('notnull', 'f.dim_a_id IS NOT NULL'), ('or', "(f.kind = 'k1' OR f.qty > 9)"), ('not', 'NOT f.flag'),
    ('notand', "NOT (f.tenant_id = 2 AND f.kind = 'k3')"), ('contains', "f.tags @> ARRAY['t1']"), ('overlap', "f.tags && ARRAY['t0', 't4']"),
    ('jsonb', 'f.attrs @> \'{"a": 3}\''), ('like', "f.kind LIKE 'k%'"), ('id', 'f.id < 50'), ('param_any', 'f.dim_a_id = ANY ($2::int[])'),
    ('sublink', 'f.dim_a_id IN (SELECT id FROM ps_entities WHERE grp < 3)'), ('initplan', 'f.tenant_id = (SELECT max(id) FROM ps_tenants)'),
    ('const', 'f.dim_a_id = 5'), ('notexists', 'NOT EXISTS (SELECT 1 FROM ps_pairs p WHERE p.dim_a_id = f.dim_a_id AND p.tenant_id = 2)'),
    ('false', 'false'), ('inlist_null', 'f.dim_a_id IN (1, 2, NULL)'), ('bm25', "f.body @@> 'word1'"), ('bm25_and', "f.body @@> 'word2 common' AND f.tenant_id < 4"),
    ('bm25_or', "(f.body @@> 'word3' OR f.tenant_id = 1)"),
    ('bm25_exists', "EXISTS (SELECT 1 FROM ps_events_bm25 b WHERE b.id = f.id AND b.body @@> 'word4')"),
]
BM25_PREDS = {'bm25', 'bm25_and', 'bm25_or', 'bm25_exists'}

WRAPS = ['plain', 'fact_sub', 'fact_cte', 'outer_sub', 'union_all', 'view_cte']


def sift_bool_agg(sess):
    return [l for l in sess.run("SELECT name FROM pg_settings WHERE name LIKE 'sift.agg\\_%' AND vartype = 'bool' ORDER BY 1;")]


def server_enables(sess):
    have = set(sess.run("SELECT substr(name, 8) FROM pg_settings WHERE name LIKE 'enable\\_%' AND vartype = 'bool';"))
    return [e for e in ENABLES if e in have]


def configs(sess):
    """the named setting lists this library accepts; a list that the library reduces to another one (or to none) is not run twice"""
    c = {name: LIB.sets(sets) for name, sets in all_configs(sess).items()}
    seen, out = set(), {}
    for name, sets in c.items():
        if name == 'default' or (sets and tuple(sets) not in seen):
            out[name] = sets
            seen.add(tuple(sets))
    return out


def all_configs(sess):
    c = {'default': [], 'force': ['sift.agg_force=on'], 'w2': PAR, 'force_w2': ['sift.agg_force=on'] + PAR,
         'nl': NESTLOOP, 'nl_force': NESTLOOP + ['sift.agg_force=on'], 'nl_nomat': NESTLOOP + ['enable_material=off', 'sift.agg_force=on'],
         'generic': ['plan_cache_mode=force_generic_plan'], 'custom': ['plan_cache_mode=force_custom_plan'],
         'generic_force_w2': ['plan_cache_mode=force_generic_plan', 'sift.agg_force=on'] + PAR,
         'noscan': ['sift.enable_scan=off'], 'noagg': ['sift.enable_agg=off'], 'nocolscan': ['sift.enable_col_scan=off', 'sift.agg_force=on'],
         'notopn': ['sift.enable_agg=off', 'sift.agg_force=on'], 'pscan': ['sift.parallel_scan=on'] + PAR,
         'plan_lo': ['sift.test_plan_scale=1', 'sift.agg_force=on'], 'plan_hi': ['sift.test_plan_scale=1000000'], 'cheap_idx': ['random_page_cost=1.01', 'seq_page_cost=4']}
    for e in server_enables(sess):
        c[f'no_{e}'] = [f'enable_{e}=off']
        c[f'no_{e}_force'] = [f'enable_{e}=off', 'sift.agg_force=on']
    for name in sift_bool_agg(sess):
        c[name.replace('sift.', 'no_')] = [f'{name}=off', 'sift.agg_force=on']
    return c


def wrap_fact(wrap, F):
    """(prefix, relation text standing for the fact table, outer wrapper)"""
    t = FACT_TABLE[F]
    if wrap == 'fact_sub':
        return '', f'(SELECT * FROM {t}) f', None
    if wrap == 'fact_cte':
        return f'fc AS MATERIALIZED (SELECT * FROM {t})', 'fc f', None
    if wrap == 'view_cte':
        return f'fc AS (SELECT id, dim_a_id, tenant_id, created_at, kind, amount, qty, score, tags, flag, attrs, body, ctid FROM {t} WHERE id > 0)', 'fc f', None
    if wrap == 'union_all':
        return '', f'(SELECT * FROM ps_events_btree UNION ALL SELECT * FROM {t}) f', None
    return '', f'{t} f', 'outer' if wrap == 'outer_sub' else None


def statement(F, jt, src, form, tail, pred, wrap):
    """(sql, ordered, prepared) or None: one statement of the matrix"""
    j = build_join(jt, src, form)
    if j is None:
        return None
    prefix, frm, jwhere, expo = j
    if expo is None and tail['s']:
        return None
    if tail['only'] and F not in tail['only']:
        return None
    pname, ptext = pred
    if pname in BM25_PREDS and F not in BM25_FACTS:
        return None
    if wrap == 'union_all' and F in ('part', 'inh'):
        return None
    fpre, frel, outer = wrap_fact(wrap, F)
    sub = dict(expo) if expo else {}
    fx = lambda text: text.format(**sub) if text else ''
    where = ' AND '.join(x for x in (jwhere, ptext) if x)
    frm = frm.replace('FROM {F} f', 'FROM ' + frel)
    sel, group, having, order = fx(tail['sel']), fx(tail['group']), fx(tail['having']), fx(tail['order'])
    parts = [f'SELECT {sel}', frm]
    if where:
        parts.append('WHERE ' + where)
    if group:
        parts.append('GROUP BY ' + group)
    if having:
        parts.append('HAVING ' + having)
    if order:
        parts.append('ORDER BY ' + order)
    if tail['limit']:
        parts.append(tail['limit'])
    sql = ' '.join(parts)
    ctes = [p for p in (prefix[5:-1] if prefix else '', fpre) if p]
    if ctes:
        sql = 'WITH ' + ', '.join(ctes) + ' ' + sql
    ordered = tail['total'] and bool(order)
    if outer:
        sql, ordered = f'SELECT * FROM ({sql}) w', False
    prepared = '$1' in sql or '$2' in sql
    return sql, ordered, prepared


# the fact table inside the statement, re-read for every row of the outer relation: a correlated LATERAL, a correlated subquery or EXISTS (a Param rescan of the
# pushdown) and a Materialize node (the nested loop with the inner side kept). {F} is the fact table; every one has a total order
INNER = {
    'lat_agg':      'SELECT s.id, l.n, l.t FROM ps_entities s CROSS JOIN LATERAL (SELECT count(*) AS n, sum(f.amount) AS t FROM {F} f WHERE f.dim_a_id = s.id) l ORDER BY s.id',
    'lat_left_grp': 'SELECT s.id, l.kind, l.c FROM ps_entities s LEFT JOIN LATERAL (SELECT f.kind, count(*) AS c FROM {F} f WHERE f.dim_a_id = s.id GROUP BY f.kind '
                    'ORDER BY c DESC, f.kind LIMIT 2) l ON true ORDER BY s.id, l.kind',
    'lat_topn':     'SELECT s.id, l.id, l.amount FROM ps_entities s CROSS JOIN LATERAL (SELECT f.id, f.amount FROM {F} f WHERE f.tenant_id = s.tenant_id AND f.qty > s.grp '
                    'ORDER BY f.amount DESC, f.id DESC LIMIT 3) l WHERE s.id < 25 ORDER BY s.id, l.amount DESC, l.id DESC',
    'lat_distinct': 'SELECT s.id, l.d FROM ps_entities s CROSS JOIN LATERAL (SELECT count(DISTINCT f.kind) AS d FROM {F} f WHERE f.dim_a_id <= s.id) l WHERE s.id < 30 ORDER BY s.id',
    'param_scalar': 'SELECT s.id, (SELECT count(*) FROM {F} f WHERE f.dim_a_id = s.id AND f.qty > s.grp), (SELECT max(f.amount) FROM {F} f WHERE f.tenant_id = s.tenant_id) '
                    'FROM ps_entities s ORDER BY s.id',
    'param_exists': "SELECT s.id FROM ps_entities s WHERE EXISTS (SELECT 1 FROM {F} f WHERE f.dim_a_id = s.id AND f.kind = 'k' || (s.grp % 7)) ORDER BY s.id",
    'param_notin':  'SELECT s.id FROM ps_entities s WHERE s.id NOT IN (SELECT f.dim_a_id FROM {F} f WHERE f.dim_a_id IS NOT NULL AND f.tenant_id = s.tenant_id) ORDER BY s.id',
    'param_having': 'SELECT s.id, (SELECT sum(f.amount) FROM {F} f WHERE f.dim_a_id = s.id GROUP BY f.tenant_id HAVING count(*) > 2 ORDER BY 1 DESC LIMIT 1) '
                    'FROM ps_entities s ORDER BY s.id',
    'mat_cte':      "WITH m AS MATERIALIZED (SELECT f.kind, count(*) AS c, sum(f.amount) AS t FROM {F} f GROUP BY f.kind) SELECT s.id, m.c, m.t FROM ps_entities s "
                    "JOIN m ON m.kind = 'k' || (s.grp % 7) ORDER BY s.id",
    'mat_nl':       'SELECT s.id, count(*), sum(x.amount) FROM ps_entities s JOIN (SELECT f.dim_a_id, f.amount FROM {F} f OFFSET 0) x ON x.dim_a_id < s.id WHERE s.id < 20 '
                    'GROUP BY s.id ORDER BY s.id',
    'mat_agg_nl':   'SELECT s.id, x.c FROM ps_entities s JOIN (SELECT f.tenant_id, count(*) AS c FROM {F} f GROUP BY f.tenant_id) x ON x.tenant_id <= s.tenant_id '
                    'WHERE s.id < 20 ORDER BY s.id, x.c',
    'desc_keyset':  'SELECT f.id, f.amount FROM {F} f WHERE f.id < 200 ORDER BY f.id DESC LIMIT 10',
    'desc_cursor':  'SELECT f.id, f.created_at FROM {F} f WHERE (f.created_at, f.id) < (timestamptz \'2026-04-01\', 120) ORDER BY f.created_at DESC, f.id DESC LIMIT 15',
}
INNER_CFGS = ['default', 'force', 'nl_force', 'nl_nomat', 'force_w2', 'generic_force_w2']


class Item:
    __slots__ = ('n', 'F', 'jt', 'src', 'form', 'tail', 'pred', 'wrap', 'sql', 'ordered', 'prepared', 'cfgs')


def enumerate_items(tier, cfg_names, facts):
    """the whole matrix of the tier, in a fixed order: a statement number is the same on every run of the same tree"""
    tails = [t for t in TAILS]
    cfg_main = [c for c in cfg_names if c not in ('default',)]
    items, seen = [], set()
    forms = list(EQ_FORMS) + list(COND_FORMS)
    joins = [(jt, src, form) for jt in JOIN_TYPES for src in SOURCES for form in forms]
    k = 0

    def add(F, jt, src, form, ti, pi, wi, cfgs):
        nonlocal k
        t, p, w = tails[ti % len(tails)], PREDS[pi % len(PREDS)], WRAPS[wi % len(WRAPS)]
        # a tail of the source needs its columns: take the next tail until one fits
        for _ in range(len(tails)):
            st = statement(F, jt, src, form, t, p, w)
            if st:
                break
            ti += 1
            t = tails[ti % len(tails)]
        else:
            return
        sql, ordered, prepared = st
        key = (sql, tuple(cfgs))
        if key in seen:
            return
        seen.add(key)
        it = Item()
        it.n, it.F, it.jt, it.src, it.form, it.tail, it.pred, it.wrap = k, F, jt, src, form, t, p[0], w
        it.sql, it.ordered, it.prepared, it.cfgs = sql, ordered, prepared, cfgs
        items.append(it)
        k += 1

    def add_inner(F, name, sql, cfgs):
        nonlocal k
        it = Item()
        it.n, it.F, it.jt, it.src, it.form, it.tail, it.pred, it.wrap = k, F, 'inner', name, 'param', None, 'none', 'plain'
        it.sql, it.ordered, it.prepared, it.cfgs = sql, True, False, cfgs
        items.append(it)
        k += 1

    def pick(i, stride, n=2, names=None):
        names = names or cfg_main
        return ['default'] + [names[(i * stride + j * 7) % len(names)] for j in range(n)]

    # 1. every join form on the btree fact (the crash of the COALESCE key of an outer join was this table)
    for i, (jt, src, form) in enumerate(joins):
        add('btree', jt, src, form, i, i // 3, i // 5, pick(i, 5))
    # 2. every other fact table on a rotating share of the join forms
    share = 9 if tier == 'full' else 40
    for fi, F in enumerate([f for f in facts if f != 'btree']):
        for i, (jt, src, form) in enumerate(joins):
            if (i + fi * 3) % share == 0:
                add(F, jt, src, form, i + fi, i // 2 + fi, i + fi * 2, pick(i + fi, 11))
    # 3. every output shape and predicate on every fact table, on reference joins
    refs = [('inner', 'ent', 'plain'), ('left', 'cte_oj', 'coalesce_s'), ('semi_exists', 'sub_oj', 'plain'), ('anti_in', 'values', 'plain')]
    for F in facts:
        for ti in range(len(tails)):
            for ri, (jt, src, form) in enumerate(refs):
                if tier == 'quick' and (ti + ri) % 2:
                    continue
                add(F, jt, src, form, ti, ti * 3 + ri, ti + ri, pick(ti + ri, 3, 1))
        for pi in range(len(PREDS)):
            for ri, (jt, src, form) in enumerate(refs[:2]):
                add(F, jt, src, form, pi * 5, pi, pi + ri, pick(pi, 13, 1))
        for ti in range(len(tails)):
            add(F, 'inner', 'ent', 'plain', ti, 0, 0, pick(ti, 3, 1))
    # 4. the fact table inside a correlated LATERAL, a correlated subquery and a Materialize, on every fact table
    for F in facts:
        for name, text in INNER.items():
            add_inner(F, name, text.replace('{F}', FACT_TABLE[F]), [c for c in INNER_CFGS if c == 'default' or c in cfg_names])
    # 5. the whole planner setting list on a reference set of statements
    ref_items = [it for it in items if it.jt in ('inner', 'left', 'semi_exists') and it.src in ('cte_oj', 'ent', 'oj_left') and it.F == 'btree']
    stride = max(1, len(ref_items) // (30 if tier == 'full' else 6))
    for it in ref_items[::stride]:
        it.cfgs = ['default'] + [c for c in cfg_names if c != 'default']
    return items


# ---------------------------------------------------------------- running

def split_run(lines):
    """the lines of one run -> (explain lines, result lines)"""
    sec, out = None, {'X': [], 'R': []}
    for l in lines:
        if l in ('@@X', '@@R'):
            sec = l[2]
        elif sec:
            out[sec].append(l)
    return out['X'], out['R']


NOISE = re.compile(r'^(LOCATION|DETAIL|HINT|CONTEXT|QUERY|STATEMENT):')


def errors_of(lines):
    errs = []
    for l in lines:
        m = re.match(r'ERROR:\s+([0-9A-Z]{5}): (.*)', l)
        if m:
            errs.append((m.group(1), m.group(2)))
    return errs


def rows_of(lines):
    return [l for l in lines if not NOISE.match(l) and not re.match(r'(ERROR|WARNING|NOTICE):', l)]


def run_text(it, sets, explain):
    sql = it.sql
    if it.prepared or 'generic' in ''.join(sets):
        text = f'DEALLOCATE ALL; PREPARE p(int, int[]) AS {sql};\n'
        run_sql = 'EXECUTE p(3, \'{2,5,9}\')'
    else:
        text, run_sql = '', sql
    sets_sql = f'RESET ALL; SET statement_timeout = \'{STATEMENT_SECS}s\'; ' + ''.join(f'SET {s.split("=")[0]} = \'{s.split("=", 1)[1]}\'; ' for s in sets)
    return (sets_sql + '\n' + text + ('\\echo @@X\nEXPLAIN (COSTS OFF) ' + run_sql + ';\n' if explain else '') + '\\echo @@R\n' + run_sql + ';')


def baseline_text(it):
    sql = it.sql.replace('$1', '3').replace('$2', "'{2,5,9}'::int[]")
    sets = BASELINE_SCORE if 'sift.score' in sql else BASELINE
    sets_sql = f'RESET ALL; SET statement_timeout = \'{STATEMENT_SECS}s\'; ' + ''.join(f'SET {s.split("=")[0]} = \'{s.split("=", 1)[1]}\'; ' for s in sets)
    return sets_sql + '\n\\echo @@R\n' + sql + ';'


def digest(rows, ordered):
    return hashlib.md5('\n'.join(rows if ordered else sorted(rows)).encode()).hexdigest()


def first_diff(a, b, ordered):
    if not ordered:
        a, b = sorted(a), sorted(b)
    for i, (x, y) in enumerate(itertools.zip_longest(a, b)):
        if x != y:
            return f'row {i}: sift: {x!r} plain: {y!r} ({len(a)} vs {len(b)} rows)'
    return 'equal'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    fmt.add_argument(ap)
    ap.add_argument('--tier', default='quick', choices=['quick', 'full'])
    ap.add_argument('--chunk', default='0/1')
    ap.add_argument('--only', default='')
    ap.add_argument('--budget', type=int, default=0)
    ap.add_argument('--db', default='shapes')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--show', type=int, default=-1)
    ap.add_argument('--facts', default=','.join(FACTS))
    ap.add_argument('--no-schema', action='store_true')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--jobs', type=int, default=4, help='sessions that run the statements at a time')
    a = ap.parse_args()
    global LIB
    LIB = fmt.Lib(a.format, env=ENV)
    ENV['PGOPTIONS'] = f'{CLIENT_OPTIONS} {LIB.pgoptions}'.strip()
    db = a.db
    subprocess.run(['psql', '-X', '-q', '-d', 'postgres', '-c', f'DROP DATABASE IF EXISTS {db}', '-c', f'CREATE DATABASE {db}'] if not a.no_schema else ['true'],
                   env=ENV, check=True, capture_output=True)
    if not a.no_schema:
        r = subprocess.run(['psql', '-X', '-q', '-d', db, '-v', 'ON_ERROR_STOP=1', '-f', os.path.join(HERE, 'shapes_matrix', 'schema.sql')], env=ENV, capture_output=True, text=True)
        if r.returncode:
            sys.exit('schema: ' + r.stderr)
        for fact, ddl in OPTIONAL_INDEXES.items():
            r = subprocess.run(['psql', '-X', '-q', '-d', db, '-v', 'ON_ERROR_STOP=1', '-c', ddl, '-c', 'ANALYZE'], env=ENV, capture_output=True, text=True)
            if r.returncode:
                print(f'UNSUPPORTED index of fact {fact} on --format {a.format}: {r.stderr.strip().splitlines()[0]}', flush=True)
    sess = Session(db)
    cfgs = configs(sess)
    cfgs['baseline'] = BASELINE
    items = enumerate_items(a.tier, [c for c in cfgs if c != 'baseline'], a.facts.split(','))
    if a.only:
        items = [i for i in items if re.search(a.only, i.sql) or re.search(a.only, f'{i.F} {i.jt} {i.src} {i.form}')]
    k, n = map(int, a.chunk.split('/'))
    items = [i for i in items if i.n % n == k]
    if a.list or a.show >= 0:
        for it in items:
            if a.list or it.n == a.show:
                print(f'#{it.n} {it.F} {it.jt}/{it.src}/{it.form} cfg={",".join(it.cfgs)} ordered={it.ordered}\n  {it.sql}')
        return 0
    sess.close()
    t0 = time.time()
    stats = dict(statements=0, runs=0, invalid=0, sift_plans=0, crashes=0, skipped_budget=0)
    fails, warnings = [], {}
    sift_by_fact = {}
    log_start = log_mark()
    lock = threading.Lock()

    def fail(kind, it, cfg, detail):
        with lock:
            fails.append((kind, it, cfg, detail))
            print(f'FAIL {kind} #{it.n} fact={it.F} join={it.jt}/{it.src}/{it.form} cfg={cfg} sets={" ".join(cfgs[cfg])}\n  sql: {it.sql}\n  {detail}', flush=True)

    def count(key, n=1):
        with lock:
            stats[key] += n

    def worker(part):
        """the statements of part on a session of its own (the database is read only)"""
        sess = Session(db)

        def send(text, it, cfg):
            nonlocal sess
            try:
                return sess.run(text)
            except Crash as e:
                count('crashes')
                sess.close()
                fail('CRASH', it, cfg, f'the session ended: {e}')
                if not wait_ready(db):
                    sys.exit('the server did not come back after the crash')
                sess = Session(db)
                return None

        for it in part:
            if a.budget and time.time() - t0 > a.budget:
                count('skipped_budget')
                continue
            count('statements')
            base = send(baseline_text(it), it, 'baseline')
            if base is None:
                continue
            brows = rows_of(split_run(base)[1])
            berr = errors_of(base)
            for cfg in it.cfgs:
                out = send(run_text(it, cfgs[cfg], True), it, cfg)
                count('runs')
                if out is None:
                    continue
                xplan, res = split_run(out)
                errs = errors_of(out)
                with lock:
                    for l in out:
                        if l.startswith('WARNING:'):
                            warnings[l] = warnings.get(l, 0) + 1
                    if any('Sift' in l for l in xplan):
                        stats['sift_plans'] += 1
                        sift_by_fact[it.F] = sift_by_fact.get(it.F, 0) + 1
                if errs and berr and errs[0][0] == berr[0][0]:
                    count('invalid')
                    continue
                bad = [e for e in errs if e[0] == 'XX000' or BAD.search(e[1])]
                if bad:
                    fail('INTERNAL', it, cfg, f'{bad[0][0]}: {bad[0][1]}')
                elif errs and not berr:
                    fail('ERROR', it, cfg, f'{errs[0][0]}: {errs[0][1]}')
                elif berr and not errs:
                    fail('ROWS', it, cfg, f'the plain plan raised {berr[0][0]}: {berr[0][1]} and sift returned {len(rows_of(res))} rows')
                elif digest(rows_of(res), it.ordered) != digest(brows, it.ordered):
                    fail('ROWS', it, cfg, first_diff(rows_of(res), brows, it.ordered))
        sess.close()

    jobs = max(1, min(a.jobs, len(items)))
    with ThreadPoolExecutor(jobs) as ex:
        list(ex.map(worker, [items[j::jobs] for j in range(jobs)]))
    elapsed = time.time() - t0
    ok = wait_ready(db, 10)
    crashed_log = log_scan(log_start)
    print(f'shapes: format={a.format} tier={a.tier} chunk={a.chunk} statements={stats["statements"]} runs={stats["runs"]} sift_plans={stats["sift_plans"]} '
          f'invalid_in_both={stats["invalid"]} crashes={stats["crashes"]} skipped_for_budget={stats["skipped_budget"]} failures={len(fails)} '
          f'seconds={elapsed:.0f} server_up={ok} log_signals={crashed_log}')
    print('sift plans by fact: ' + ' '.join(f'{k}={v}' for k, v in sorted(sift_by_fact.items())))
    for w, c in sorted(warnings.items(), key=lambda x: -x[1])[:10]:
        print(f'warning x{c}: {w}')
    if not a.keep:
        subprocess.run(['psql', '-X', '-q', '-d', 'postgres', '-c', f'DROP DATABASE IF EXISTS {db}'], env=ENV, capture_output=True)
    return 1 if fails or crashed_log or not ok else 0


def log_files():
    d = os.path.join(TMP, 'data', 'log')
    return [os.path.join(d, f) for f in sorted(os.listdir(d))] if os.path.isdir(d) else []


def log_mark():
    return {f: os.path.getsize(f) for f in log_files()}


def log_scan(mark):
    n = 0
    for f in log_files():
        with open(f, errors='replace') as fh:
            fh.seek(mark.get(f, 0))
            n += len(re.findall(r'terminated by signal|TRAP|PANIC', fh.read()))
    return n


if __name__ == '__main__':
    sys.exit(main())
