#!/usr/bin/env python3
"""Differential fuzz of the aggregate pushdown (docs/AGG.md, "Differential suite").

A seed gives a database (test/fuzz/diff_fuzz_data.sql: small tables, NULLs, duplicates, a sift_col index on each fact table) and a script
of episodes: a few DML statements (updates, deletes, inserts: dirty rows and write-buffer rows) and then random queries of the shapes of
the pushdown (first-row lookups, two-level aggregates, partition pruning, pre-aggregates, dirty-row shapes, UNION ALL arms, top-N). Each
query runs under the plan PostgreSQL has without sift (sift.enable_agg off, the reference) and under variants of the pushdown (the scan,
the pre-aggregates, the arms alone, parallel workers, ...); the results are compared exactly, floats within 1e-12 + 1e-9 relative
(test/col/run.sql), rows by position only when the query orders them totally. A query that raises must raise the same SQLSTATE everywhere.

  test/fuzz/diff_fuzz.py --seeds 1-3 --queries 40 --rows 3000        the quick tier (scripts/ci.sh, job difftest-fuzz)
  test/fuzz/diff_fuzz.py --seeds 100-199 --queries 300 --out DIR     a long search: a reproducer per divergence in DIR (shrunk)
  test/fuzz/diff_fuzz.py --replay FILE                               runs a reproducer written by --out again

Needs the scripts/pgtest.sh cluster (SIFT_TMP, SIFT_PORT) installed and started. Exit 1 on a divergence or a query the generator got wrong.
"""
import argparse
import os
import random
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TMP = os.environ.get('SIFT_TMP', '/tmp/sift-arch')
ENV = dict(os.environ, PGHOST=f'{TMP}/sock', PGPORT=os.environ.get('SIFT_PORT', '56100'), PGUSER='postgres',
           PGOPTIONS='-c client_min_messages=warning')

BASE = 'seq_page_cost=1000;random_page_cost=1000;sift.agg_left_lateral=on'
REF = 'sift.enable_agg=off;max_parallel_workers_per_gather=0'


# ---------- psql ----------
def psql(db, text, check=True):
    for _ in range(120):
        p = subprocess.run(['psql', '-X', '-q', '-At', '-d', db, '-v', 'ON_ERROR_STOP=%d' % check], input=text, capture_output=True, text=True, env=ENV)
        if 'not yet accepting connections' not in p.stderr and 'the database system is' not in p.stderr:
            break
        time.sleep(1)
    if check and p.returncode:
        sys.exit(f'psql {db} failed: {p.stderr.strip()[:2000]}')
    return p.stdout, p.stderr


def create_db(name, template=None):
    psql('postgres', f'DROP DATABASE IF EXISTS {name} WITH (FORCE); CREATE DATABASE {name}' + (f' TEMPLATE {template}' if template else ''))


def data_sql(seed, rows, parts):
    t = open(f'{HERE}/diff_fuzz_data.sql').read()
    return t.replace('@SEED@', str(seed)).replace('@ROWS@', str(rows)).replace('@PARTS@', str(parts))


def build_base(name, seed, rows, parts):
    create_db(name)
    psql(name, open(f'{REPO}/test/col/run.sql').read() + data_sql(seed, rows, parts) + open(f'{HERE}/diff_fuzz.sql').read())


# ---------- predicates, keys, aggregates ----------
# Each entry: (columns it needs, builder(r, e)); a table without a column does not get the entry.
def _in(r, vals, k):
    return ', '.join(str(v) if not isinstance(v, str) else f"'{v}'" for v in r.sample(vals, k))


def _wide(r, n):
    """A list of ids with a member far beyond the table's: a range wider than a byte table (a bit table, a hash table when it is wider than 2^28), long, or short for
    the variant that lowers bits_set_values; a NULL now and then."""
    ids = r.sample(range(1, max(n, 8)), min(r.choice([5, 60, 1100]), max(n, 8) - 1)) + [r.choice([5_000_000, 30_000_000, 200_000_000, 300_000_000])]
    return ', '.join([str(v) for v in ids] + (['NULL'] if r.random() < 0.15 else []))


def _date(r):
    return f"date '2025-{r.randrange(1, 13):02d}-{r.randrange(1, 29):02d}'"


def _dates(r):
    a, b = sorted((_date(r), _date(r)))
    return a, b


PREDS = [
    ({'id'}, lambda r, e, n: f'{e}.id < {r.randrange(1, n)}'),
    ({'id'}, lambda r, e, n: f'{e}.id % {r.choice([2, 3, 7, 16])} = {r.randrange(2)}'),
    ({'id'}, lambda r, e, n: f'{e}.id BETWEEN {r.randrange(1, n // 2)} AND {r.randrange(n // 2, n)}'),
    ({'id'}, lambda r, e, n: f"{e}.id {r.choice(['', 'NOT '])}IN ({_wide(r, n)})"),
    ({'kind'}, lambda r, e, n: f"{e}.kind = 'k{r.randrange(6)}'"),
    ({'kind'}, lambda r, e, n: f'{e}.kind IN ({_in(r, ["k0", "k1", "k2", "k3", "k4", "k9"], r.randrange(1, 4))})'),
    ({'kind'}, lambda r, e, n: f"{e}.kind <> 'k{r.randrange(5)}'"),
    ({'kind'}, lambda r, e, n: f'{e}.kind IS {r.choice(["", "NOT "])}NULL'),
    ({'grp'}, lambda r, e, n: f'{e}.grp = {r.randrange(13)}'),
    ({'grp'}, lambda r, e, n: f'{e}.grp BETWEEN {r.randrange(6)} AND {r.randrange(6, 13)}'),
    ({'grp'}, lambda r, e, n: f'{e}.grp IN ({_in(r, list(range(13)), r.randrange(1, 5))})'),
    ({'grp'}, lambda r, e, n: f'{e}.grp IS {r.choice(["", "NOT "])}NULL'),
    ({'grp'}, lambda r, e, n: f'{e}.grp % 3 = {r.randrange(3)}'),
    ({'ts'}, lambda r, e, n: f'{e}.ts {r.choice([">=", "<", ">", "<="])} {_date(r)}'),
    ({'ts'}, lambda r, e, n: f'{e}.ts BETWEEN %s AND %s' % _dates(r)),
    ({'ts'}, lambda r, e, n: f'{e}.ts IS {r.choice(["", "NOT "])}NULL'),
    ({'ts'}, lambda r, e, n: f"{e}.ts <@ daterange(%s, %s, '[]')" % _dates(r)),
    ({'amount'}, lambda r, e, n: f'{e}.amount {r.choice([">", "<", ">=", "<="])} {r.randrange(1000)}'),
    ({'amount'}, lambda r, e, n: f'{e}.amount BETWEEN {r.randrange(500)} AND {r.randrange(500, 1000)}'),
    ({'amount'}, lambda r, e, n: f'{e}.amount IS {r.choice(["", "NOT "])}NULL'),
    ({'val'}, lambda r, e, n: f'{e}.val {r.choice([">", "<"])} {r.randrange(-50, 50)}'),
    ({'val'}, lambda r, e, n: f'{e}.val IS {r.choice(["", "NOT "])}NULL'),
    ({'big'}, lambda r, e, n: f'{e}.big {r.choice([">", "<"])} {r.randrange(-3, 4)}00000000000'),
    ({'big'}, lambda r, e, n: f'{e}.big % 3 = {r.randrange(3)}'),
    ({'num'}, lambda r, e, n: f'{e}.num {r.choice(["<", ">="])} {r.randrange(1000)}'),
    ({'flag'}, lambda r, e, n: f'{r.choice(["", "NOT "])}{e}.flag'),
    ({'flag'}, lambda r, e, n: f'{e}.flag IS {r.choice(["", "NOT "])}NULL'),
    ({'parent_id'}, lambda r, e, n: f'{e}.parent_id IN ({_in(r, list(range(600)), r.randrange(1, 8))})'),
    ({'parent_id'}, lambda r, e, n: f'{e}.parent_id IS {r.choice(["", "NOT "])}NULL'),
    ({'parent_id'}, lambda r, e, n: f"{e}.parent_id {r.choice(['', 'NOT '])}IN (SELECT id FROM items WHERE {r.choice(['enabled', 'kind = ' + chr(39) + 'k1' + chr(39), 'grp < 5'])})"),
    ({'parent_id'}, lambda r, e, n: f"{r.choice(['', 'NOT '])}EXISTS (SELECT 1 FROM items i WHERE i.id = {e}.parent_id AND i.kind = 'k{r.randrange(5)}')"),
    ({'doc'}, lambda r, e, n: f'''{e}.doc @> '{{"parts": [{{"cat": "cat_{r.randrange(7)}"}}]}}' '''),
    ({'doc'}, lambda r, e, n: f"{e}.doc ? 'parts'"),
    ({'doc'}, lambda r, e, n: f"jsonb_array_length({e}.doc -> 'parts') {r.choice(['>', '<=', '='])} {r.randrange(4)}"),
    ({'metrics'}, lambda r, e, n: f"({e}.metrics ->> 'm1')::int {r.choice(['>', '<', '='])} {r.randrange(20)}"),
    ({'metrics'}, lambda r, e, n: f"jsonb_typeof({e}.metrics -> 'm3') = '{r.choice(['number', 'string', 'null'])}'"),
    ({'metrics'}, lambda r, e, n: f"({e}.metrics -> 'm2')::float8 {r.choice(['>', '<'])} {r.randrange(7)}"),
]
COLUMNS = {'id', 'kind', 'grp', 'ts', 'amount', 'val', 'parent_id'}
EVENTS = COLUMNS | {'a', 'b', 'big', 'num', 'flag', 'doc', 'metrics'}


def preds(r, e, n, cols, k):
    out = []
    for _ in range(k):
        need, f = r.choice([p for p in PREDS if p[0] <= cols])
        out.append(f(r, e, n))
    if len(out) > 1 and r.random() < 0.25:
        out[:2] = [f'({out[0]} OR {out[1]})']
    return out


KEYS = [
    ({'kind'}, '{e}.kind'), ({'grp'}, '{e}.grp'), ({'ts'}, '{e}.ts'), ({'ts'}, "date_trunc('month', {e}.ts)::date"), ({'flag'}, '{e}.flag'),
    ({'big'}, '{e}.big / 100000000000'), ({'big'}, '{e}.big % 5'), ({'big'}, '{e}.big'), ({'num'}, 'floor({e}.num / 5000)'), ({'num'}, '{e}.num::int / 10000'),
    ({'amount'}, '{e}.amount / 100'), ({'a'}, '{e}.a % 10'), ({'parent_id'}, '{e}.parent_id % 20'), ({'grp'}, '{e}.grp % 4'),
]
AGGS = [
    ({'id'}, 'count(*)'), ({'amount'}, 'count({e}.amount)'), ({'kind'}, 'count(DISTINCT {e}.kind)'), ({'grp'}, 'count(DISTINCT {e}.grp)'),
    ({'amount'}, 'sum({e}.amount)'), ({'amount'}, 'avg({e}.amount)'), ({'amount'}, 'sum(3 * {e}.amount)'), ({'big'}, 'sum({e}.big)'),
    ({'big'}, 'sum({e}.big + {c})'), ({'big'}, 'avg(5 - {e}.big)'), ({'big'}, 'sum(3 * {e}.big)'), ({'val'}, 'avg({e}.val)'), ({'val'}, 'sum({e}.val)'),
    ({'num'}, 'sum({e}.num)'), ({'num'}, 'avg({e}.num)'),
    ({'amount'}, 'min({e}.amount)'), ({'amount'}, 'max({e}.amount)'), ({'val'}, 'min({e}.val)'), ({'val'}, 'max({e}.val)'), ({'ts'}, 'min({e}.ts)'),
    ({'ts'}, 'max({e}.ts)'), ({'kind'}, 'min({e}.kind)'), ({'kind'}, 'max({e}.kind)'), ({'big'}, 'min({e}.big)'), ({'big'}, 'max({e}.big)'),
    ({'num'}, 'max({e}.num)'), ({'flag'}, 'bool_and({e}.flag)'), ({'flag'}, 'bool_or({e}.flag)'),
    ({'parent_id'}, 'count(DISTINCT {e}.parent_id)'), ({'parent_id', 'amount'}, 'count(DISTINCT {e}.parent_id) FILTER (WHERE {e}.amount > {k}00)'),
    ({'amount'}, 'count(DISTINCT {e}.amount)'), ({'amount'}, 'sum(DISTINCT {e}.amount)'), ({'amount'}, 'avg(DISTINCT {e}.amount)'), ({'num'}, 'count(DISTINCT {e}.num)'),
    ({'num'}, 'sum(DISTINCT {e}.num)'), ({'amount'}, 'count(DISTINCT 3 * {e}.amount)'),  # sets of numbers: the workers send them by value
    ({'kind'}, "count(*) FILTER (WHERE {e}.kind = 'k{k}')"), ({'kind'}, "count(*) FILTER (WHERE {e}.kind IS NULL)"),
    ({'grp', 'amount'}, 'sum({e}.amount) FILTER (WHERE {e}.grp IN ({k}, {k2}))'), ({'grp', 'val'}, 'max({e}.val) FILTER (WHERE {e}.grp = {k})'),
    ({'kind', 'big'}, "sum({e}.big) FILTER (WHERE {e}.kind IN ('k{k}', 'k{k2}'))"), ({'amount'}, 'count(*) FILTER (WHERE {e}.amount > {k}00)'),
    ({'ts', 'amount'}, "count({e}.amount) FILTER (WHERE {e}.ts >= date '2025-06-01')"),
]


def pick(r, table, cols, e):
    need, t = r.choice([x for x in table if x[0] <= cols])
    return t.format(e=e, c=r.choice([1, 7, 1000, 9223372036854775000]) if 'big + ' in t else r.randrange(-50, 50), k=r.randrange(6), k2=r.randrange(6, 12))


def keys(r, e, cols, n):
    return list(dict.fromkeys(pick(r, KEYS, cols, e) for _ in range(n)))


def aggs(r, e, cols, n):
    return list(dict.fromkeys(pick(r, AGGS, cols, e) for _ in range(n)))


# ---------- families: gen(r, ctx) -> parts, render(parts) -> (sql, ordered) ----------
def select_list(p):
    ks = [f'{k} AS k{i}' for i, k in enumerate(p['keys'])]
    ag = [f'{a} AS a{i}' for i, a in enumerate(p['aggs'])]
    return ', '.join(ks + ag)


def where(p, name='where'):
    return 'WHERE ' + ' AND '.join(p[name]) if p[name] else ''


def grouped(p, frm):
    g = ' GROUP BY ' + ', '.join(str(i + 1) for i in range(len(p['keys']))) if p['keys'] else ''
    o = ' ORDER BY ' + ', '.join(str(i + 1) for i in range(len(p['keys']))) if p['keys'] and p['ordered'] else ''
    h = ' HAVING ' + ' AND '.join(p['having']) if p['having'] else ''
    return f"SELECT {select_list(p)} FROM {frm} {where(p)}{g}{h}{o}"


def gen_preagg(r, ctx):
    t, e = ('events', 'e') if r.random() < 0.8 else ('ev_part', 'e')
    cols = EVENTS if t == 'events' else COLUMNS
    n = ctx['rows'] if t == 'events' else ctx['rows'] // 2
    p = dict(table=t, keys=keys(r, e, cols, r.choice([0, 1, 1, 2, 3])), aggs=aggs(r, e, cols, r.randrange(1, 7)), where=preds(r, e, n, cols, r.randrange(0, 4)),
             having=[], ordered=True)
    if p['keys'] and r.random() < 0.2:
        p['having'] = [r.choice(['count(*) > 3', 'count(*) < 400', 'sum(1) <> 7'])]
    return p


def render_preagg(p):
    return grouped(p, f"{p['table']} e"), p['ordered'] and bool(p['keys'])


def gen_keys(r, ctx):
    """A flat (or lightly grouped) count(DISTINCT parent_id) under random filters: the units that keep the key list or bitmap of the column (key_columns) answer it from the list when every filter holds for the whole unit, the others (table kc) from the histogram of the cube."""
    t = r.choice(['events', 'events', 'events', 'ev_part', 'kc', 'kc'])
    cols = EVENTS if t == 'events' else COLUMNS
    n = ctx['rows'] // 2 if t == 'ev_part' else ctx['rows']
    ag = ['count(DISTINCT e.parent_id)'] + aggs(r, 'e', cols, r.randrange(0, 4))
    if r.random() < 0.3:
        ag.append('count(DISTINCT e.parent_id) FILTER (WHERE e.amount > %d00)' % r.randrange(6))
    return dict(table=t, keys=keys(r, 'e', cols, r.choice([0, 0, 0, 1])), aggs=list(dict.fromkeys(ag)), where=preds(r, 'e', n, cols, r.randrange(0, 4)), having=[], ordered=True)


def gen_partition(r, ctx):
    n = ctx['rows'] // 2
    cols = COLUMNS
    sel = r.randrange(8)
    if sel == 0:
        pr = [f'e.id = {r.randrange(1, n)}']
    elif sel == 1:
        pr = [f'e.id IN ({_in(r, list(range(1, n)), r.randrange(1, 9))})']
    elif sel == 2:
        pr = [f'e.id = ANY (ARRAY[{_in(r, list(range(1, n)), r.randrange(1, 6))}])']
    elif sel == 3:
        pr = [f"e.id = (SELECT max(id) FROM items WHERE kind = 'k{r.randrange(5)}')"]
    elif sel == 4:
        pr = [f"e.id IN (SELECT id FROM items WHERE kind = 'k{r.randrange(5)}' AND id % {r.choice([5, 11, 30])} = 0)"]
    elif sel == 5:
        pr = [f'e.id % {r.choice([4, 16])} = {r.randrange(4)}']
    elif sel == 6:
        pr = [f'e.id BETWEEN {r.randrange(1, n // 2)} AND {r.randrange(n // 2, n)}']
    else:
        pr = [f'e.id IN (VALUES ({r.randrange(1, n)}), ({r.randrange(1, n)}), (NULL))']
    join = r.choice(['', '', f"JOIN (VALUES ({r.randrange(1, n)}), ({r.randrange(1, n)}), ({r.randrange(1, n)}), (NULL)) v(id) ON v.id = e.id",
                     f"JOIN items i ON i.id = e.id AND i.kind = 'k{r.randrange(5)}'", 'LEFT JOIN items i ON i.id = e.parent_id',
                     f"JOIN items i ON i.id = e.parent_id AND i.enabled"])
    p = dict(keys=keys(r, 'e', cols, r.choice([0, 1, 1, 2])), aggs=aggs(r, 'e', cols, r.randrange(1, 4)), where=pr + preds(r, 'e', n, cols, r.randrange(0, 2)),
             having=[], ordered=True, join=join)
    return p


def render_partition(p):
    return grouped(p, f"ev_part e {p['join']}"), bool(p['keys'])


def gen_lookup(r, ctx):
    t = r.choice(['dims', 'dims', 'dims_dup', 'vers', 'vers'])
    child_filter = []
    if r.random() < 0.4:
        child_filter.append(r.choice(['d.score > 100', 'd.score % 2 = 0', "d.label LIKE 'l1%'", 'd.a < 100', 'd.b > 20', 'd.score IS NOT NULL']))
    order = None
    if t == 'vers':
        order = r.choice(['d.ts DESC, d.id', 'd.ts, d.id', 'd.ts DESC NULLS LAST, d.id DESC', 'd.score DESC, d.id', 'd.id DESC'])
    elif r.random() < 0.25:
        order = r.choice(['d.score DESC', 'd.label', 'd.score, d.label'])
    nested = r.random() < 0.3
    cols = EVENTS
    return dict(table=t, jointype=r.choice(['LEFT JOIN', 'LEFT JOIN', 'JOIN']), child_filter=child_filter, order=order, nested=nested,
                keys=keys(r, 'e', cols, r.choice([0, 1, 1, 2])) + r.sample(['d.score % 5', "d.label IS NULL", 'd.a % 7'], r.randrange(0, 2)),
                aggs=['count(*)', 'count(d.score)'] + r.sample(['sum(d.score)', 'min(d.label)', 'max(d.score)', 'avg(d.score)', 'count(DISTINCT d.label)',
                                                              'sum(e.amount)', 'max(x.w)', 'count(x.tag)'], r.randrange(1, 4)),
                where=preds(r, 'e', ctx['rows'], cols, r.randrange(0, 3)), having=[], ordered=True)


def render_lookup(p):
    order = f" ORDER BY {p['order']}" if p['order'] else ''
    cf = ''.join(f' AND {c}' for c in p['child_filter'])
    sel = 'd.a, d.b, d.label, d.score' + (', d.ts' if p['table'] == 'vers' else '')
    if p['table'] == 'vers' or 'd.id' in (p['order'] or ''):
        sel += ', d.id' if p['table'] == 'vers' else ''
    frm = f"events e {p['jointype']} LATERAL (SELECT {sel} FROM {p['table']} d WHERE d.a = e.a AND d.b = e.b{cf}{order} LIMIT 1) d ON true"
    if p['nested']:
        frm += ' LEFT JOIN LATERAL (SELECT x.tag, x.w FROM dims2 x WHERE x.id = d.score % 50 LIMIT 1) x ON true'
    # aggregates and keys that name x need the nested lookup
    if not p['nested']:
        p = dict(p, aggs=[a for a in p['aggs'] if 'x.' not in a])
    return grouped(p, frm), bool(p['keys'])


def gen_dirty(r, ctx):
    form = r.randrange(5)
    cols = EVENTS
    member = r.choice(["({e}.metrics -> 'm1')::int", "({e}.metrics -> 'm2')::int", "CAST(jsonb_extract_path({e}.metrics, 'm1') AS bigint)",
                       "({e}.metrics ->> 'm1')::float8", "({e}.metrics -> 'm2')::float8", "({e}.metrics -> 'm3')::int",
                       "CASE WHEN jsonb_typeof({e}.metrics -> 'm3') = 'number' THEN ({e}.metrics -> 'm3')::int END"])
    p = dict(form=form, member=member, where=preds(r, 'e', ctx['rows'], cols, r.randrange(0, 3)), keys=[], aggs=[], having=[], ordered=True,
             elem=r.choice(["p ->> 'cat' = 'cat_%d'" % r.randrange(6), "(p ->> 'score')::float8 > %d" % r.randrange(9), "p ->> 'dir_a' = 'p'",
                            "p ->> 'cat' = 'cat_%d' AND (p ->> 'score')::float8 >= %d" % (r.randrange(6), r.randrange(9))]),
             neg=r.random() < 0.3, outer=r.choice(['jsonb_array_elements', 'jsonb_to_recordset']))
    if form == 0:
        p['keys'] = keys(r, 'e', cols, r.randrange(0, 2)) + r.sample(['{m}', 'floor(({m})::float8)::int % 3'], r.randrange(0, 2))
        p['aggs'] = ['count(*)'] + r.sample(['sum({m})', 'max({m})', 'avg({m})', 'count({m})', 'sum(e.amount)'], r.randrange(1, 3))
    elif form == 1:
        p['keys'] = keys(r, 'e', cols, r.randrange(0, 2))
        p['aggs'] = aggs(r, 'e', cols, r.randrange(1, 4))
        p['where'] = p['where'] + ['{m} {op} {n}'.format(m=member.format(e='e'), op=r.choice(['>', '<=', '=']), n=r.randrange(7))]
    elif form == 2:
        p['keys'] = keys(r, 'e', cols, r.randrange(0, 2))
        p['aggs'] = aggs(r, 'e', cols, r.randrange(1, 4))
    elif form == 3:
        p['keys'] = ['m.key'] + keys(r, 'e', cols, r.randrange(0, 2))
        p['aggs'] = ['count(*)', 'sum(CAST(m.value AS float8))'] + r.sample(['max(CAST(m.value AS float8))', 'avg(CAST(m.value AS float8))'], r.randrange(0, 2))
        p['where'] = p['where'] + r.sample(["m.key IN ('m1', 'm2')", "m.key <> 'm3'", "m.key = 'm1'"], r.randrange(0, 2))
    else:
        p['keys'] = ['o.cat'] + r.sample(['o.dir_a', 'e.kind'], r.randrange(0, 2))
        p['aggs'] = ['count(*)', 'avg(o.score)'] + r.sample(['sum(o.score)', 'max(o.score)', 'count(DISTINCT e.id)'], r.randrange(0, 2))
        p['where'] = p['where'] + r.sample(["o.dir_a = 'p'", 'o.score > 3', "o.cat <> 'cat_1'"], r.randrange(0, 2))
    return p


def render_dirty(p):
    m = p['member']
    sub = lambda s: s.replace('{m}', m.format(e='e')).replace('{e}', 'e')
    form = p['form']
    q = dict(p, keys=[sub(k) for k in p['keys']], aggs=[sub(a) for a in p['aggs']], where=[sub(w) for w in p['where']])
    if form == 2:
        ex = f"{'NOT ' if p['neg'] else ''}EXISTS (SELECT 1 FROM jsonb_array_elements(e.doc -> 'parts') p WHERE {p['elem']})"
        q['where'] = q['where'] + [ex]
        frm = 'events e'
    elif form == 3:
        frm = 'events e CROSS JOIN LATERAL jsonb_each(e.metrics) m'
    elif form == 4:
        if p['outer'] == 'jsonb_to_recordset':
            frm = "events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> 'parts') o(cat text, dir_a text, score float8)"
        else:
            q['keys'] = [k.replace('o.cat', "(o ->> 'cat')").replace('o.dir_a', "(o ->> 'dir_a')") for k in q['keys']]
            q['aggs'] = [re.sub(r'o\.score', "(o ->> 'score')::float8", a) for a in q['aggs']]
            q['where'] = [re.sub(r"o\.score", "(o ->> 'score')::float8", w).replace('o.dir_a', "(o ->> 'dir_a')").replace('o.cat', "(o ->> 'cat')") for w in q['where']]
            frm = "events e CROSS JOIN LATERAL jsonb_array_elements(e.doc -> 'parts') o"
    else:
        frm = 'events e'
    return grouped(q, frm), bool(q['keys'])


def gen_leftlateral(r, ctx):
    cols = EVENTS
    p = dict(form=r.randrange(3), func=r.choice(['jsonb_to_recordset', 'jsonb_array_elements']), cond=r.choice(['', '', 'o.score > %d' % r.randrange(9), "o.dir_a = 'p'",
                                                                                                              "o.cat <> 'cat_%d'" % r.randrange(6), 'o.score IS NOT NULL']),
             inner=r.random() < 0.3, where=preds(r, 'e', ctx['rows'], cols, r.randrange(0, 3)), keys=[], aggs=[], having=[], ordered=True)
    p['keys'] = r.sample(['o.cat', 'o.dir_a'], r.randrange(0, 2)) + keys(r, 'e', cols, r.randrange(0, 2))
    p['aggs'] = ['count(*)', 'count(o.score)', 'avg(o.score)'] + r.sample(['sum(o.score)', 'max(o.score)', 'min(o.cat)', 'count(DISTINCT e.id)', 'count(DISTINCT o.cat)'], r.randrange(0, 3))
    p['aggs'] = p['aggs'] + aggs(r, 'e', cols, r.randrange(0, 2))
    return p


def render_leftlateral(p):
    cond = p['cond']
    if p['func'] == 'jsonb_to_recordset':
        lat = "jsonb_to_recordset(e.doc -> 'parts') AS o(cat text, dir_a text, score float8)"
        sub = lambda t: t
    else:
        lat = "jsonb_array_elements(e.doc -> 'parts') AS o"
        sub = lambda t: re.sub(r'o\.score', "(o ->> 'score')::float8", t).replace('o.dir_a', "(o ->> 'dir_a')").replace('o.cat', "(o ->> 'cat')")
    join = 'JOIN' if p['inner'] else 'LEFT JOIN'
    on = ' ON ' + (sub(cond) if cond and not p['inner'] else 'true')
    frm = f"events e {join} LATERAL {lat}{on}"
    if p['inner'] and cond:
        p = dict(p, where=p['where'] + [cond])
    q = dict(p, keys=[sub(k) for k in p['keys']], aggs=[sub(a) for a in p['aggs']], where=[sub(w) for w in p['where']])
    if q['aggs'] == ['count(*)'] and not q['keys']:
        q['aggs'] = q['aggs'] + ['count(DISTINCT e.id)']
    if p['form'] == 0:
        return grouped(q, frm), bool(q['keys'])
    # per row, then over the rows: the row keeps its group when it has no element
    inner = (f"SELECT e.id, e.grp, e.kind, max(e.amount) AS amount, {sub('avg(o.score)')} AS sc, count(*) AS n FROM {frm} "
             f"{where(q)} GROUP BY e.id, e.grp, e.kind")
    if p['form'] == 1:
        return f"SELECT s.grp, s.kind, count(s.id) AS ids, avg(s.amount) AS amount, avg(s.sc) AS sc, sum(s.n) AS n FROM ({inner}) s GROUP BY 1, 2 ORDER BY 1, 2", True
    return f"SELECT count(s.id) AS ids, avg(s.amount) AS amount, avg(s.sc) AS sc FROM ({inner}) s", False


def gen_lateralagg(r, ctx):
    p = dict(func=r.choice(['jsonb_to_recordset', 'jsonb_array_elements']), cond=r.choice(['', '', 'o.score IS NOT NULL', 'o.score > %d' % r.randrange(9), "o.dir_a = 'p'"]),
             aggs=r.sample(['avg(o.score)', 'sum(o.score)', 'min(o.score)', 'max(o.score)', 'count(o.score)', 'max(o.score * 2)', 'count(o.cat)', 'min(o.cat)'], r.randrange(1, 4)),
             where=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 3)), keys=keys(r, 'e', EVENTS, r.randrange(0, 2)), outer=r.choice(['avg', 'sum', 'max', 'count']), ordered=True)
    return p


def render_lateralagg(p):
    if p['func'] == 'jsonb_to_recordset':
        lat = "jsonb_to_recordset(e.doc -> 'parts') AS o(cat text, dir_a text, score float8)"
        sub = lambda t: t
    else:
        lat = "jsonb_array_elements(e.doc -> 'parts') AS o"
        sub = lambda t: re.sub(r'o\.score', "(o ->> 'score')::float8", t).replace('o.dir_a', "(o ->> 'dir_a')").replace('o.cat', "(o ->> 'cat')")
    cols = ', '.join(f"{sub(a)} AS a{i}" for i, a in enumerate(p['aggs']))
    wh = (' WHERE ' + sub(p['cond'])) if p['cond'] else ''
    inner = f"SELECT e.id, e.grp, e.kind, {', '.join(f't.a{i}' for i in range(len(p['aggs'])))} FROM events e LEFT JOIN LATERAL (SELECT {cols} FROM {lat}{wh}) t ON true"
    if p['where']:
        inner += ' WHERE ' + ' AND '.join(p['where'])
    outer = ', '.join(f"{p['outer']}(s.a{i}) AS v{i}" for i in range(len(p['aggs'])) if not (p['outer'] in ('avg', 'sum') and 'cat' in p['aggs'][i]))
    k = p['keys'] and [re.sub(r'\be\.', 's.', x) for x in p['keys'] if re.match(r'e\.(id|grp|kind)\b', x)]
    if not outer:
        outer = 'count(s.id) AS n'
    else:
        outer += ', count(s.id) AS n'
    if k:
        return f"SELECT {', '.join(k)}, {outer} FROM ({inner}) s GROUP BY {', '.join(k)} ORDER BY {', '.join(k)}", True
    return f"SELECT {outer} FROM ({inner}) s", False


def gen_latest(r, ctx):
    orders = ['e.ts', 'e.amount', 'e.val', 'e.a', 'e.big', 'e.kind', 'e.num', 'e.parent_id']
    n = r.choice([1, 1, 2, 3, 5, 12, 400])
    forms = [f't.rn <= {n}', f't.rn < {n + 1}', f'{n} >= t.rn', f'{n + 1} > t.rn', f't.rn <= {n}::bigint AND t.rn <= {n + 3}'] + ([f't.rn = 1', '1 = t.rn'] if n == 1 else [])
    return dict(parts=r.sample(['e.kind', 'e.grp', 'e.a', 'e.parent_id', 'e.flag'], r.randrange(1, 3)),
                order=[o + r.choice(['', ' DESC', ' NULLS FIRST', ' DESC NULLS LAST']) for o in r.sample(orders, r.randrange(1, 3))],
                filter=r.choice(forms), cte=r.random() < 0.3, extra=r.sample(['e.ts', 'e.val', 'e.num', 'e.title', 'e.amount * 2'], r.randrange(0, 3)),
                where=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 3)), outer=r.choice([[], [], ['t.amount > %d' % r.randrange(20)]]),
                shape=r.choice(['rows', 'rows', 'rn', 'count']))


def render_latest(p):
    # id last: the order has no ties, so the first rows of a group are the ones the normal plan keeps
    win = f"row_number() OVER (PARTITION BY {', '.join(p['parts'])} ORDER BY {', '.join(p['order'])}, e.id) rn"
    extra = ''.join(f', {x} AS x{i}' for i, x in enumerate(p['extra']))
    sub = f"SELECT e.id, e.kind, e.grp, e.amount{extra}, {win} FROM events e {where(p)}"
    frm, pre = (f"latest t", f"WITH latest AS ({sub}) ") if p['cte'] else (f"({sub}) t", '')
    cond = ' AND '.join([p['filter']] + p['outer'])
    sel = {'rows': 't.id, t.kind, t.amount' + ''.join(f', t.x{i}' for i in range(len(p['extra']))), 'rn': 't.id, t.rn, t.rn + 1 AS next', 'count': 'count(*) AS n, sum(t.amount) AS amount, max(t.rn) AS deepest'}[p['shape']]
    return f"{pre}SELECT {sel} FROM {frm} WHERE {cond}", False


def gen_decor(r, ctx):
    tie = r.choice([['f.parent_id = e.parent_id'], ['f.grp = e.grp'], ["f.kind = e.kind"], ['f.parent_id = e.parent_id', 'f.grp = e.grp'], ['f.grp = e.grp', 'f.kind = e.kind']])
    return dict(form=r.choice(['lateral', 'left', 'select', 'where']), tie=tie, aggs=r.sample(['count(*)', 'count(f.amount)', 'sum(f.amount)', 'min(f.ts)', 'max(f.kind)', 'avg(f.val)', 'bool_or(f.flag)',
                                                                                      'count(DISTINCT f.grp)', "sum(f.amount) FILTER (WHERE f.grp > 5)", 'array_agg(f.a ORDER BY f.id)', 'coalesce(sum(f.amount), -1)',
                                                                                      'sum(f.amount) / nullif(count(f.amount), 0)'], r.randrange(1, 4)),
                inner=preds(r, 'f', ctx['rows'], EVENTS, r.randrange(0, 3)), outer=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 2)), pick=r.randrange(4, 12),
                test=r.choice(['', 'IS NULL', 'IS NOT NULL', '> 100', '= 0', 'IS NULL OR {c} > 100']), total=r.random() < 0.3)


NUMERIC_AGG = re.compile(r'^(count|sum|avg|coalesce\(sum)')


def render_decor(p):
    if not NUMERIC_AGG.match(p['aggs'][0]) or 'sum(f.amount) / nullif' in p['aggs'][0]:
        p = dict(p, test=p['test'] if p['test'] in ('', 'IS NULL', 'IS NOT NULL') else 'IS NULL')

    # the outer rows are a few hundred: the normal plan looks up the group of each, the scan groups the table once
    wh = ' AND '.join([f"e.id % {p['pick']} = 0"] + p['outer'])
    sub = lambda i: f"SELECT {p['aggs'][i]} FROM events f WHERE {' AND '.join(p['tie'] + p['inner'])}"
    n = len(p['aggs'])
    if p['form'] in ('lateral', 'left'):
        join = 'JOIN' if p['form'] == 'lateral' else 'LEFT JOIN'
        cols = ', '.join(f"{p['aggs'][i]} AS v{i}" for i in range(n))
        frm = f"events e {join} LATERAL (SELECT {cols} FROM events f WHERE {' AND '.join(p['tie'] + p['inner'])}) x ON true"
        outs = [f'x.v{i}' for i in range(n)]
    else:
        frm = 'events e'
        outs = [f'({sub(i)})' for i in range(n)]
    cond = ''
    if p['test']:
        c = outs[0]
        cond = ' AND (' + (c + ' ' + p['test']).replace('{c}', c) + ')' if '{c}' not in p['test'] else ' AND (' + c + ' ' + p['test'].replace('{c}', c) + ')'
    if p['form'] == 'where':
        cond = f" AND ({outs[0]} {p['test'] or 'IS NULL'})".replace('{c}', outs[0])
    sel = 'count(*) AS n' if p['total'] else 'e.id, ' + ', '.join(f'{o} AS v{i}' for i, o in enumerate(outs))
    if p['total'] and p['form'] != 'where':
        sel = 'count(*) AS n, ' + ', '.join(f'count({o}) AS c{i}' for i, o in enumerate(outs))
    return f"SELECT {sel} FROM {frm} WHERE {wh}{cond}", False


def gen_union(r, ctx):
    cols = EVENTS
    t = 'events'
    arms = []
    shared = preds(r, 'e', ctx['rows'], cols, r.randrange(0, 3))
    for i in range(r.randrange(2, 5)):
        arms.append(dict(key=r.choice([None, None] + [k for _, k in KEYS if k in ('kind', 'grp', 'ts', 'flag')]),
                         extra=preds(r, 'e', ctx['rows'], cols, 1) if r.random() < 0.2 else []))
    return dict(arms=arms, aggs=aggs(r, 'e', cols, r.randrange(1, 4)), where=shared, ordered=True, keys=[], having=[], table=t)


def render_union(p):
    parts = []
    for i, a in enumerate(p['arms']):
        k = a['key']
        wh = p['where'] + a['extra']
        sel = f"'{i}' AS lvl, {k + '::text' if k else 'NULL::text'} AS k, " + ', '.join(f'{x} AS a{j}' for j, x in enumerate(p['aggs']))
        parts.append(f"SELECT {sel} FROM events e {'WHERE ' + ' AND '.join(wh) if wh else ''}{' GROUP BY ' + k if k else ''}")
    return ' UNION ALL '.join(parts) + ' ORDER BY 1, 2', True


def gen_topn(r, ctx):
    cols = EVENTS
    sortable = ['amount', 'ts', 'kind', 'grp', 'val', 'big', 'num', 'flag', 'parent_id', 'a', 'ref', 'blob', 'title', 'ref', 'blob', 'title', 'wnum', 'inum', 'wnum', 'inum', 'lv', 'lv', 'lv', 'ivl', 'ivl', 'ivl']
    ks = r.sample(sortable, r.choice([1, 2, 2, 3, 5, 6]))  # more than 4 sort keys sort every row that passes
    # sort expressions the scan evaluates itself: coalesce of a column and a constant of its type, lower and upper of the text column, arithmetic and casts with
    # PostgreSQL's own evaluation (the data keeps them clear of overflow, division by a column and NaN into an integer)
    k1, k2, d = r.choice([1, 7, 100, 5000]), r.choice([2, 3, 11]), r.choice([3, 7, 1000])
    ints = lambda c: r.choice([f'coalesce({c}, {r.choice([0, 300, 5000])})', f'{c} + {k1}', f'{k1} - {c}', f'-{c}', f'{c} * {k2}', f'{c} % {d}', f'{c}::bigint', f'{c}::text',
                               f'({c}::numeric / {d})', f'{c}::float8 / {d}', f'{c} > {k1}', f'CASE WHEN {c} IS NULL THEN {k1} WHEN {c} > {d} THEN {c} ELSE -{c} END', f'nullif({c}, {k2})',
                               f'greatest({c}, {k1})', f'least({c}, {k1}) + 1', f'{c} IS NULL', f'{c} IN ({k1}, {k2}, 7)', f'CASE {c} WHEN {k2} THEN 1 WHEN {d} THEN 2 END'])
    xform = {'parent_id': ints, 'grp': ints, 'a': ints, 'amount': ints,
             'big': lambda c: r.choice([f'coalesce({c}, {r.choice([0, 9223372036854775807, -9223372036854775807 - 1])})', f'{c} / {d}', f'{c} * {k2}', f'{c}::numeric', f'{c} % {d}']),
             'val': lambda c: r.choice([f'{c} * {k2}', f'-{c}', f'{c} + {k1}', f'{c} / {k2}']),
             'num': lambda c: r.choice([f'{c} * {k2}', f'{c} + 1.5', f'{c} / {k2}', f'{c}::float8', f'{c}::text', f'-{c}']),
             'ts': lambda c: r.choice([f"coalesce({c}, DATE '{r.choice(['2025-01-01', '2025-09-15', '2026-03-01'])}')", f'{c} + {k1}', f'{c} - {k2}', f"{c} - DATE '2025-06-01'"]),
             'kind': lambda c: r.choice([f"{r.choice(['lower', 'upper'])}({c})", f"CASE WHEN {c} = 'k1' THEN 0 WHEN {c} < 'k3' THEN 1 ELSE 2 END", f'length({c})', f"{c} IN ('k1', 'k4')", f"coalesce({c}, 'zz')", f"nullif({c}, 'k2')", f"lower(upper({c}))", f"{c} || '-' || e.grp::text", f'left({c}, 1)', f'md5({c})', f"replace({c}, 'k', 'K')", f"upper({c}) || lower(e.kind)"])}
    two = {('parent_id', 'grp'): 'e.parent_id + e.grp', ('a', 'amount'): 'e.a * 2 + e.amount', ('grp', 'a'): 'e.grp - e.a', ('c', 'd'): 'coalesce(e.parent_id, e.grp, e.a)',
           ('e', 'f'): 'CASE WHEN e.a > e.b THEN e.a ELSE e.b END', ('g', 'h'): 'greatest(e.a, e.b, e.grp)', ('i', 'j'): 'coalesce(e.kind, e.id::text)',
           ('k', 'l'): 'e.a IS NULL OR e.b IS NULL', ('o', 'p'): 'abs(e.a - e.b)', ('q', 'r'): 'round(e.num, 1)', ('s', 't'): 'date_trunc(\'month\', e.ts)', ('u', 'v'): 'mod(e.id, 10) + e.grp', ('m', 'n'): 'least(e.ts, DATE \'2025-06-01\')'}
    order = [f"{xform[k](f'e.{k}') if k in xform and r.random() < 0.5 else f'e.{k}' + (' COLLATE \"C\"' if k == 'title' and r.random() < 0.8 else '')} {r.choice(['', 'DESC'])} {r.choice(['', 'NULLS FIRST', 'NULLS LAST'])}".strip() for k in ks]
    if r.random() < 0.15:
        order[0] = f"{two[r.choice(list(two))]} {r.choice(['', 'DESC'])} {r.choice(['', 'NULLS FIRST', 'NULLS LAST'])}".strip()
    grouped_ = r.random() < 0.25
    # the page with the totals of its set: aggregates over the whole set as window functions (not a float: the sums of another order differ in the last digits)
    windows = [] if grouped_ or r.random() < 0.6 else r.sample(['count(*)', 'count(e.amount)', 'sum(e.amount)', 'sum(e.big)', 'max(e.ts)', 'min(e.grp)', 'sum(e.num)', 'avg(e.num)', 'min(e.kind)',
                                                                'count(*) FILTER (WHERE e.flag)', 'max(e.big) FILTER (WHERE e.grp > 3)'], r.choice([1, 1, 2, 4]))
    return dict(order=order, windows=windows, limit=r.choice([1, 5, 10, 50, 300, 1500]), offset=r.choice([0, 0, 3, 40, 500, 2500]), where=preds(r, 'e', ctx['rows'], cols, r.randrange(0, 3)),
                grouped=grouped_, keys=[], aggs=[], having=[], ordered=True, gkeys=keys(r, 'e', cols, 2), gagg=[r.choice(['count(*)', 'sum(e.amount)', 'max(e.amount)', 'count(DISTINCT e.grp)', 'min(e.big)', 'sum(e.big)'])])  # not a float: ties within the tolerance would order differently


def render_topn(p):
    if p['grouped']:
        gk = p['gkeys']
        ks = ', '.join(f'{k} AS k{i}' for i, k in enumerate(gk))
        pos = ', '.join(str(i + 1) for i in range(len(gk)))
        sql = f"SELECT {ks}, {p['gagg'][0]} AS s FROM events e {where(p)} GROUP BY {pos} ORDER BY s DESC NULLS LAST, {pos} LIMIT {p['limit']}"
    else:
        order = ', '.join(p['order'] + ['e.id'])
        win = ''.join(f', {w} OVER () AS w{i}' for i, w in enumerate(p['windows']))
        sql = f"SELECT e.id, e.kind, e.ts, e.amount, e.val{win} FROM events e {where(p)} ORDER BY {order} LIMIT {p['limit']} OFFSET {p['offset']}"
    return sql, True


SPANS = ["('span_a', daterange('2025-07-01', '2026-01-31', '[]'))", "('span_b', daterange('2025-01-01', '2025-06-30', '[]'))",
         "('span_c', daterange('2025-04-01', '2025-09-30', '[]'))", "('span_d', daterange(NULL, '2025-03-01'))"]


def gen_twolevel(r, ctx):
    cols = EVENTS
    return dict(spans=r.sample(SPANS, r.randrange(1, 4)), parent=r.choice(['', 'JOIN items pa ON pa.id = e.parent_id AND pa.enabled', 'LEFT JOIN items pa ON pa.id = e.parent_id']),
                lookup=r.choice(['lookup', 'lookup', 'lookup_dup', 'lookup_empty', None]), where=preds(r, 'e', ctx['rows'], cols, r.randrange(0, 3)),
                spine=r.choice([None, None, 'JOIN', 'LEFT JOIN']), spinesrc=r.choice(['TABLE buckets', "VALUES ('k0', NULL::date, 'a'), ('k1', NULL, 'b'), (NULL, NULL, 'd'), ('k7', NULL, 'e')"]),
                total=r.random() < 0.8, keys=[], aggs=[], having=[], ordered=True,
                # a group key that holds a sub-select: each arm plans one of its own
                dim=r.choice([None, None, f"{r.choice(['', 'NOT '])}IN (SELECT id FROM items WHERE {r.choice(['enabled', 'grp < 5', 'kind <> ' + chr(39) + 'k1' + chr(39)])})"]),
                # the total arm counts the rows the lookup has none for: its join is an outer one next to the inner joins of the arms with the NOT NULL test
                unmatched=r.random() < 0.4,
                # the lookup behind a lateral sub-select of its own, over the parts with a score: its WHERE sits at the inner join of the arms with the NOT NULL test
                wrapped=r.random() < 0.4)


def render_twolevel(p):
    w = ' WHERE ' + ' AND '.join(p['where']) if p['where'] else ''
    dim = p.get('dim')
    dim_col = f", CASE WHEN e.parent_id {dim} THEN jsonb_build_object('id', e.parent_id) END AS dim" if dim else ''
    scoped = (f"WITH scoped AS NOT MATERIALIZED (SELECT e.id, e.parent_id, e.doc, e.kind, p.span{dim_col} FROM events e JOIN (VALUES {', '.join(p['spans'])}) p(span, range) "
              f"ON e.ts <@ p.range {p['parent']}{w}), ")

    def arm(cat):
        if p['lookup'] and p['wrapped']:
            c = 'mo.category' if cat else 'NULL::text'
            g = 'GROUP BY s.id, s.parent_id, s.kind, s.span' + (', s.dim' if dim else '') + (f', {c}' if cat else '')
            return (f"SELECT s.id, s.parent_id, s.kind, s.span, {'s.dim, ' if dim else ''}{c} AS category, avg(mo.score) AS score, count(s.id) AS n FROM scoped s "
                    f"INNER JOIN LATERAL (SELECT rm.derived_cat AS category, o.score FROM LATERAL (SELECT cat, dir_a, score FROM jsonb_to_recordset(s.doc -> 'parts') q(cat text, dir_a text, score float8) "
                    f"WHERE q.score IS NOT NULL) o LEFT JOIN LATERAL (SELECT derived_cat FROM {p['lookup']} m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true) mo ON true "
                    + ("" if p['unmatched'] and not cat else "WHERE mo.category IS NOT NULL ") + g)
        if p['lookup']:
            lk = (f"LEFT JOIN LATERAL (SELECT derived_cat FROM {p['lookup']} m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true "
                  + ("" if p['unmatched'] and not cat else "WHERE rm.derived_cat IS NOT NULL"))
            c = 'rm.derived_cat' if cat else 'NULL::text'
        else:
            lk, c = '', 'o.cat' if cat else 'NULL::text'
        g = 'GROUP BY s.id, s.parent_id, s.kind, s.span' + (', s.dim' if dim else '') + (f', {c}' if cat else '')
        return (f"SELECT s.id, s.parent_id, s.kind, s.span, {'s.dim, ' if dim else ''}{c} AS category, avg(o.score) AS score, count(s.id) AS n FROM scoped s "
                f"CROSS JOIN LATERAL jsonb_to_recordset(s.doc -> 'parts') o(cat text, dir_a text, score float8) {lk} {g}")

    flag_a = f"flag_a AS ({arm(True)}{' UNION ALL ' + arm(False) if p['total'] else ''}) "
    if p['spine']:
        sp = f"sp(kind, ts, label) AS (SELECT * FROM ({p['spinesrc']}) q(kind, ts, label)) " if p['spinesrc'].startswith('VALUES') else "sp(kind, ts, label) AS (SELECT kind, ts, label FROM buckets) "
        sql = (scoped + flag_a + ', ' + sp + "SELECT coalesce(category, 'all_cats') AS category, sp.label, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n, "
               f"count(*) AS cells, count(DISTINCT id) AS ids FROM sp {p['spine']} flag_a m ON m.kind = sp.kind GROUP BY 1, 2 ORDER BY 1, 2")
        sql = sql.replace('FROM sp', 'FROM sp').replace('(score)', '(m.score)').replace('sum(n)', 'sum(m.n)').replace('count(DISTINCT id)', 'count(DISTINCT m.id)') \
                 .replace("coalesce(category,", "coalesce(m.category,")
    else:
        sql = (scoped + flag_a + f"SELECT coalesce(category, 'all_cats') AS category, span{', dim' if dim else ''}, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n, "
               f"count(DISTINCT id) AS ids, count(id) AS idn, count(DISTINCT parent_id) AS parents FROM flag_a GROUP BY category, span{', dim' if dim else ''} "
               f"ORDER BY 1, 2{', 3' if dim else ''}")
    return sql, True


# ---------- the distinct rows of a group: count(DISTINCT id) over the elements of a list, FILTERs, grouping sets, a spine joined from the left ----------
def gen_distinctrows(r, ctx):
    return dict(spans=r.sample(SPANS, r.randrange(1, 4)), where=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 3)), join=r.choice(['LEFT JOIN', 'LEFT JOIN', 'JOIN']),
                sets=r.random() < 0.6, src=r.choice(['parts', 'parts', 'metrics']), filt=r.randrange(10), parents=r.random() < 0.5, key=r.choice(['x.cat', 'x.cat', 'left(x.cat, 4)']))


def render_distinctrows(p):
    w = ' WHERE ' + ' AND '.join(p['where']) if p['where'] else ''
    if p['src'] == 'parts':
        inner = f"SELECT e.id, e.parent_id, e.ts, o.cat, o.score FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> 'parts') o(cat text, dir_a text, score float8){w}"
        flt = f"x.score >= {p['filt']}"
    else:
        inner = f"SELECT e.id, e.parent_id, e.ts, m.key AS cat, NULL::float8 AS score FROM events e CROSS JOIN LATERAL jsonb_each(e.metrics) m{w}"
        flt = f"x.cat = 'm{p['filt'] % 4}'"
    g = f"GROUP BY GROUPING SETS ((p.span, {p['key']}), (p.span), ())" if p['sets'] else f"GROUP BY p.span, {p['key']}"
    sql = (f"WITH p(span, range) AS (VALUES {', '.join(p['spans'])}) "
           f"SELECT p.span, {p['key']} AS cat, count(DISTINCT x.id) AS ids, count(DISTINCT x.id) FILTER (WHERE {flt}) AS ids_f, count(x.id) AS n, count(*) AS all_rows"
           f"{', count(DISTINCT x.parent_id) AS parents' if p['parents'] else ''} FROM p {p['join']} ({inner}) x ON x.ts <@ p.range {g} ORDER BY 1, 2, 3, 4, 5, 6")
    return sql, True


# ---------- lookups of one table: several, with different conditions, in every join kind (the planner can merge them) ----------
FORMS = ['exists', 'exists', 'notexists', 'in', 'notin', 'inner', 'inner', 'left', 'scalar', 'uncorr', 'intree', 'existstree', 'notexiststree', 'lefttree']


def lk_cond(r, a):
    return r.choice([f"{a}.kind = 'k{r.randrange(5)}'", f"{a}.kind <> 'k{r.randrange(5)}'", f'{a}.grp = {r.randrange(12)}', f'{a}.grp < {r.randrange(12)}',
                     f'{a}.grp > {r.randrange(12)}', f'{a}.enabled', f'NOT {a}.enabled', f'{a}.id < {r.randrange(600)}',
                     f"{a}.kind IN ('k{r.randrange(5)}', 'k{r.randrange(5)}')", f'{a}.grp IS NULL', f"{a}.kind = 'k{r.randrange(5)}' AND {a}.grp > {r.randrange(8)}"])


def lookup(r, n, forms=FORMS):
    a = f'i{n}'
    form = r.choice(forms)
    # a key set the scan reads once may be empty before it reads a row: the rows then never reach a filter that raises an error in the plan without the scan
    cond = r.choice([f"{a}.kind <> 'k{r.randrange(5)}'", f'{a}.grp > {r.randrange(3)}', f'{a}.id < {r.randrange(300, 600)}']) if form.endswith('tree') else lk_cond(r, a)
    return dict(form=form, cond=cond, key=r.choice(['e.parent_id'] * 5 + ['e.grp', 'e.a % 600']), a=a, v=r.randrange(8))


def lk_sql(l):
    """(join clause, condition) of one lookup of items."""
    a, c, k, f = l['a'], l['cond'], l['key'], l['form']
    if f == 'exists':
        return '', f'EXISTS (SELECT 1 FROM items {a} WHERE {a}.id = {k} AND {c})'
    if f == 'notexists':
        return '', f'NOT EXISTS (SELECT 1 FROM items {a} WHERE {a}.id = {k} AND {c})'
    if f == 'in':
        return '', f'{k} IN (SELECT {a}.id FROM items {a} WHERE {c})'
    if f == 'notin':
        return '', f'{k} NOT IN (SELECT {a}.id FROM items {a} WHERE {c})'
    if f == 'intree':
        return '', f'{k} IN (SELECT {a}.id FROM items {a} JOIN dims2 {a}d ON {a}d.id = {a}.grp WHERE {c})'
    if f == 'existstree':
        return '', f'EXISTS (SELECT 1 FROM items {a} JOIN dims2 {a}d ON {a}d.id = {a}.grp WHERE {a}.id = {k} AND {c})'
    if f == 'notexiststree':
        return '', f'NOT EXISTS (SELECT 1 FROM items {a} JOIN dims2 {a}d ON {a}d.id = {a}.grp WHERE {a}.id = {k} AND {c} AND {a}d.w > 20)'
    if f == 'lefttree':
        return '', (f'{k} IN (SELECT {a}.id FROM items {a} WHERE {c} AND {a}.id IN (SELECT coalesce({a}x.id, {a}y.id) FROM items {a}x '
                    f'LEFT JOIN dims2 {a}y ON {a}y.id = {a}x.grp AND {a}y.w > 30 WHERE {a}x.id % 3 <> 1))')
    if f == 'inner':
        return f'JOIN items {a} ON {a}.id = {k} AND {c}', ''
    if f == 'left':
        return f'LEFT JOIN items {a} ON {a}.id = {k} AND {c}', ''
    if f == 'scalar':
        return '', f"(SELECT {a}.grp FROM items {a} WHERE {a}.id = {k} AND {c}) > {l['v']}"
    return '', f'EXISTS (SELECT 1 FROM items {a} WHERE {c})'


def lk_from(lks):
    joins = ' '.join(j for j, _ in map(lk_sql, lks) if j)
    conds = [w for _, w in map(lk_sql, lks) if w]
    return joins, conds


def gen_same(r, ctx):
    cols = EVENTS
    shape = r.choice(['flat', 'flat', 'union', 'twolevel'])
    n = r.choice([2, 2, 3])
    p = dict(shape=shape, lks=[lookup(r, i + 1) for i in range(n)], where=preds(r, 'e', ctx['rows'], cols, r.randrange(0, 2)), ordered=True, having=[],
             keys=keys(r, 'e', cols, r.choice([0, 1, 1])), aggs=aggs(r, 'e', cols, r.randrange(1, 3)), arms=[], twolevel=gen_twolevel(r, ctx))
    p['aggs'] = ['count(*)'] + p['aggs']
    if shape == 'union':
        p['arms'] = [[lookup(r, 10 * (j + 1) + i) for i in range(r.choice([1, 2]))] for j in range(r.choice([2, 3]))]
    return p


def lk_refs(lks):
    """Keys and aggregates over the columns a join of the lookups brings."""
    out = []
    for l in lks:
        if l['form'] in ('inner', 'left'):
            out += [f"count({l['a']}.id)", f"max({l['a']}.grp)", f"count(DISTINCT {l['a']}.kind)"]
    return out


def render_same(p):
    if p['shape'] == 'union':
        parts = []
        for j, lks in enumerate(p['arms']):
            joins, conds = lk_from(lks)
            wh = conds + p['where']
            ag = ', '.join(f'{x} AS a{i}' for i, x in enumerate(p['aggs']))
            parts.append(f"SELECT '{j}' AS lvl, NULL::text AS k, {ag} FROM events e {joins} {'WHERE ' + ' AND '.join(wh) if wh else ''}")
        return ' UNION ALL '.join(parts) + ' ORDER BY 1', True
    if p['shape'] == 'twolevel':
        t = dict(p['twolevel'])
        joins, conds = lk_from(p['lks'])
        t['parent'] = (t['parent'] + ' ' + joins).strip()
        t['where'] = t['where'] + conds
        return render_twolevel(t)
    joins, conds = lk_from(p['lks'])
    q = dict(p, aggs=p['aggs'] + lk_refs(p['lks'])[:2], where=conds + p['where'])
    return grouped(q, f'events e {joins}'), bool(q['keys'])


def gen_point(r, ctx):
    """Rows of ev_part by an equality or an IN list (short, or of 65 to 400 keys) on a column with a directory of heap addresses (ev_part_pt): the index scan and the bitmap scan of the
    access method must give the rows of the normal plan, across the DML of the run (dead rows, addresses a VACUUM frees, the write buffer)."""
    n = ctx['rows'] // 2
    col = r.choice(['id', 'id', 'parent_id', 'kind'])
    val = (lambda: f"'k{r.randrange(5)}'") if col == 'kind' else (lambda: str(r.randrange(1, n if col == 'id' else n // 4)))
    form = r.randrange(6)
    if form == 0:
        cond = f'e.{col} = {val()}'
    elif form == 5:  # a list past 64 keys, which the directory answers up to a share of the rows (misses and repeats among the keys)
        cond = f'e.{col} IN ({", ".join(val() for _ in range(r.randrange(65, 400)))})'
    elif form == 1:
        cond = f'e.{col} IN ({", ".join(val() for _ in range(r.randrange(2, 9)))})'
    elif form == 2:
        cond = f'e.{col} = ANY (ARRAY[{", ".join(val() for _ in range(r.randrange(1, 5)))}])'
    elif form == 3:
        cond = f'e.{col} = {val()} AND e.amount {r.choice(["<", ">"])} {r.randrange(100, 900)}'
    else:
        cond = f'(e.{col} = {val()} OR e.{col} = {val()})'
    return dict(where=[cond])


def render_point(p):
    return f"SELECT e.id, e.kind, e.amount FROM ev_part e WHERE {p['where'][0]} ORDER BY e.id", True


# ---------- key_cut: units cut by a key, the rows of the normal plan after every kind of churn and every kind of merge ----------
KCUT_LITERALS = {
    'ki': lambda r: str(r.randrange(-6, 36)),
    'kd': lambda r: r.choice(["'infinity'::date", "'-infinity'::date", f"date '2025-{r.randrange(1, 8):02d}-{r.randrange(1, 29):02d}'", "date '2025-01-01'", "date '2025-07-19'"]),
    'kt': lambda r: r.choice(["'infinity'::timestamp", "'-infinity'::timestamp", f"timestamp '2025-{r.randrange(1, 6):02d}-{r.randrange(1, 29):02d} {r.randrange(24):02d}:00:00'",
                              "timestamp '2025-01-01 00:00:00.25'"]),
    'kn': lambda r: r.choice(["'NaN'::numeric", str(r.randrange(0, 250)), f'{r.randrange(0, 250)}.{r.randrange(0, 125) * 8:03d}', '1000000']),
    'kf': lambda r: r.choice(["'NaN'::float8", "'Infinity'::float8", "'-Infinity'::float8", "'-0'::float8", "0::float8", f'{r.randrange(-50, 50)}.{r.randrange(0, 100):02d}']),
}
KCUT_KEYS = ['ki', 'kd', 'kt', 'kn', 'kf']


def kcut_pred(r, e):
    c = r.choice(KCUT_KEYS + KCUT_KEYS + ['amount', 'kind'])
    if c == 'amount':
        return f'{e}.amount {r.choice([">", "<", ">=", "<="])} {r.randrange(1000)}'
    if c == 'kind':
        return f"{e}.kind = 'k{r.randrange(6)}'"
    lit = KCUT_LITERALS[c]
    form = r.randrange(7)
    if form == 0:
        return f'{e}.{c} = {lit(r)}'
    if form == 1:
        return f'{e}.{c} {r.choice([">", "<", ">=", "<="])} {lit(r)}'
    if form == 2:
        return f'{e}.{c} BETWEEN {lit(r)} AND {lit(r)}'
    if form == 3:
        return f'{e}.{c} IN ({", ".join(lit(r) for _ in range(r.randrange(1, 5)))})'
    if form == 4:
        return f'{e}.{c} IS {r.choice(["", "NOT "])}NULL'
    if form == 5:
        return f'({e}.{c} = {lit(r)} OR {e}.{c} > {lit(r)})'
    return f'{e}.{c} <> {lit(r)}'


KCUT_KEY_EXPRS = ['{e}.kind', '{e}.ki', '{e}.ki % 5', '{e}.kd', "date_trunc('month', {e}.kd)::date", '{e}.kt', '({e}.kn IS NULL)', "({e}.kf = 'NaN')"]
KCUT_AGGS = ['count(*)', 'count({e}.ki)', 'sum({e}.amount)', 'avg({e}.amount)', 'min({e}.amount)', 'max({e}.amount)', 'sum({e}.val)', 'min({e}.ki)', 'max({e}.ki)', 'min({e}.kd)', 'max({e}.kd)',
             'min({e}.kt)', 'max({e}.kt)', 'min({e}.kn)', 'max({e}.kn)', 'min({e}.kf)', 'max({e}.kf)', 'count(DISTINCT {e}.kind)', 'count(DISTINCT {e}.ki)', 'count(DISTINCT {e}.kd)',
             'count(*) FILTER (WHERE {e}.ki > 10)']


def gen_keycut(r, ctx):
    t = r.choice(['kcut_a', 'kcut_b', 'kcut_c'])
    form = r.choice(['agg', 'agg', 'agg', 'topn', 'rows'])
    where_ = [kcut_pred(r, 'e') for _ in range(r.randrange(0, 4))]
    if len(where_) > 1 and r.random() < 0.25:
        where_[:2] = [f'({where_[0]} OR {where_[1]})']
    if form == 'agg':
        ks = list(dict.fromkeys(k.format(e='e') for k in r.sample(KCUT_KEY_EXPRS, r.choice([0, 0, 1, 1, 2]))))
        ag = list(dict.fromkeys(a.format(e='e') for a in r.sample(KCUT_AGGS, r.randrange(1, 6))))
        return dict(table=t, form=form, keys=ks, aggs=ag, where=where_, having=[], ordered=True)
    order = ', '.join(f'e.{k} {r.choice(["", "DESC"])} {r.choice(["", "NULLS FIRST", "NULLS LAST"])}'.strip() for k in r.sample(KCUT_KEYS, r.choice([1, 2, 3])))
    return dict(table=t, form=form, keys=[], aggs=[], where=where_, having=[], ordered=True, order=order, limit=r.choice([1, 5, 20, 100, 400]), offset=r.choice([0, 0, 7]))


def render_keycut(p):
    if p['form'] == 'agg':
        return grouped(p, f"{p['table']} e"), True
    cols = 'e.id, e.ki, e.kd, e.kt, e.kn, e.kf, e.amount'
    tail = f"ORDER BY {p['order']}, e.id LIMIT {p['limit']} OFFSET {p['offset']}" if p['form'] == 'topn' else 'ORDER BY e.id'
    return f"SELECT {cols} FROM {p['table']} e {where(p)} {tail}", True


# ---------- IS [NOT] NULL as a test of the NULL count of a unit's zone: units cut by it, answered from pre-aggregates, across soft deletes and restores ----------
NZ_NULLABLE = ['deleted_at', 'seen_at', 'tag', 'score', 'kind', 'amount', 'val']


def nz_pred(r, e):
    c = r.choice(NZ_NULLABLE)
    form = r.randrange(9)
    if form < 5:
        return f'{e}.{c} IS {r.choice(["", "NOT "])}NULL'
    if form == 5:
        return f'{e}.org {r.choice(["=", "<", ">"])} {r.randrange(12)}'
    if form == 6:
        return f'{e}.amount {r.choice(["<", ">="])} {r.randrange(1000)}'
    if form == 7:
        return f'{e}.id {r.choice(["<", ">"])} {r.randrange(1, 4 * 256)}'
    return f'({e}.{c} IS NULL OR {e}.org = {r.randrange(12)})'


NZ_KEYS = ['{e}.org', '{e}.kind', '{e}.org % 3', '({e}.deleted_at IS NULL)', '{e}.seen_at', '{e}.tag']
NZ_AGGS = ['count(*)', 'count({e}.deleted_at)', 'count({e}.seen_at)', 'count({e}.tag)', 'count({e}.score)', 'sum({e}.amount)', 'min({e}.amount)', 'max({e}.amount)', 'avg({e}.val)',
           'sum({e}.val)', 'min({e}.id)', 'max({e}.id)', 'count(DISTINCT {e}.kind)', 'count(DISTINCT {e}.org)', 'sum({e}.score)', 'max({e}.seen_at)',
           'count(*) FILTER (WHERE {e}.deleted_at IS NULL)', 'sum({e}.amount) FILTER (WHERE {e}.tag IS NOT NULL)']


def gen_nullzone(r, ctx):
    where_ = [nz_pred(r, 'e') for _ in range(r.choice([1, 1, 2, 3]))]
    if len(where_) > 1 and r.random() < 0.2:
        where_[:2] = [f'({where_[0]} OR {where_[1]})']
    ks = list(dict.fromkeys(k.format(e='e') for k in r.sample(NZ_KEYS, r.choice([0, 0, 1, 1, 2]))))
    ag = list(dict.fromkeys(a.format(e='e') for a in r.sample(NZ_AGGS, r.randrange(1, 5))))
    return dict(table='nzf', keys=ks, aggs=ag, where=where_, having=[], ordered=True)


FAMILIES = {
    'preagg': (gen_preagg, render_preagg, 4), 'partition': (gen_partition, render_partition, 3), 'lookup': (gen_lookup, render_lookup, 3),
    'dirty': (gen_dirty, render_dirty, 4), 'union': (gen_union, render_union, 2), 'topn': (gen_topn, render_topn, 2), 'twolevel': (gen_twolevel, render_twolevel, 3),
    'samelookup': (gen_same, render_same, 5), 'keys': (gen_keys, render_preagg, 4),
    'leftlateral': (gen_leftlateral, render_leftlateral, 4), 'lateralagg': (gen_lateralagg, render_lateralagg, 3), 'point': (gen_point, render_point, 3),
    'keycut': (gen_keycut, render_keycut, 4), 'latest': (gen_latest, render_latest, 3), 'decor': (gen_decor, render_decor, 4), 'nullzone': (gen_nullzone, render_preagg, 4),
    'distinctrows': (gen_distinctrows, render_distinctrows, 3),
}
# the variants of the pushdown (settings after BASE); the reference is REF
LEADER = ['on', 'off']
PAR = 'max_parallel_workers_per_gather=%d;parallel_setup_cost=0;parallel_tuple_cost=0;min_parallel_table_scan_size=0;min_parallel_index_scan_size=0;parallel_leader_participation=%s'


POINT = 'sift.enable_col_scan=on;sift.enable_agg=off;enable_seqscan=off;max_parallel_workers_per_gather=0;'


def variants(r, fam):
    if fam == 'point':  # the access method: an index scan, a bitmap scan; the reference is the normal plan
        return [('idx', POINT + 'enable_bitmapscan=off'), ('bmp', POINT + 'enable_indexscan=off;enable_indexonlyscan=off')]
    v = [('pre', 'sift.enable_agg=on;max_parallel_workers_per_gather=0'),
         ('scan', 'sift.enable_agg=on;sift.agg_preagg=off;max_parallel_workers_per_gather=0')]
    if fam == 'topn':  # the heap lowered: the sort of every row that passes runs on the rows of the tables (a testing build)
        v.append(('topnsort', 'sift.enable_agg=on;sift.test_limits=' + r.choice(['topn_rows=20', 'topn_keys=1', 'topn_rows=20,topn_keys=2']) + ';max_parallel_workers_per_gather=0'))
    if fam == 'keys':  # the units' key lists and bitmaps off: the pre-aggregate scan reads the key chunks
        v.append(('nodistinct', 'sift.enable_agg=on;sift.agg_preagg_distinct=off;max_parallel_workers_per_gather=0'))
    if fam in ('twolevel', 'union', 'lookup'):
        v.append(('arms', 'sift.enable_agg=on;sift.agg_two_level=off;sift.agg_fuse=off;max_parallel_workers_per_gather=0'))
    extra = [
        ('par', 'sift.enable_agg=on;' + PAR % (r.choice([1, 2, 3, 4]), r.choice(LEADER))),
        ('parscalar', 'sift.enable_agg=on;sift.simd=scalar;' + PAR % (r.choice([2, 3]), r.choice(LEADER))),
        ('plainpar', 'sift.enable_agg=off;' + PAR % (2, 'on')),
        ('nolookup', 'sift.enable_agg=on;sift.agg_lookup_pushdown=off;sift.agg_share_lookups=off;max_parallel_workers_per_gather=0'),
        ('nodense', 'sift.enable_agg=on;sift.agg_dense=off;sift.simd=scalar;max_parallel_workers_per_gather=0'),
        ('notopn', 'sift.enable_agg=on;sift.enable_topn=off;max_parallel_workers_per_gather=0'),
        ('bitset', 'sift.enable_agg=on;sift.test_limits=bits_set_values=4;max_parallel_workers_per_gather=0'),
    ]
    v += r.sample(extra, r.choice([1, 2]))
    return v


# ---------- DML (dirty rows and the write buffer) ----------
def dml(r, ctx):
    """One random statement; ids of new rows come from the counters of ctx."""
    m, k = r.choice([40, 97, 211, 500]), None
    k = r.randrange(m)
    ids = f'id % {m} = {k}'
    e = r.randrange(16)
    t = r.choice(['events', 'events', 'events', 'ev_part'])
    if e == 0:
        return f'UPDATE {t} SET amount = amount + {r.randrange(1, 9)} WHERE {ids}'
    if e == 1:
        return f"UPDATE {t} SET kind = 'k{r.randrange(5)}' WHERE {ids}"
    if e == 2:
        return f'UPDATE {t} SET grp = {r.randrange(12)} WHERE {ids}'
    if e == 3:
        return f'UPDATE {t} SET ts = ts + {r.randrange(1, 40)} WHERE {ids}'
    if e == 4:
        return f'UPDATE {t} SET val = {r.choice(["NULL", "val * 2", "val + 1", "0.1"])} WHERE {ids}'
    if e == 5 and t == 'events':
        return f'UPDATE events SET big = big + {r.randrange(1, 9)} WHERE {ids}'
    if e == 6 and t == 'events':
        return f"UPDATE events SET metrics = jsonb_set(metrics, '{{m{r.randrange(1, 4)}}}', '{r.choice(['7', '1.5', chr(34) + 'zz' + chr(34), 'null', 'true'])}') WHERE metrics IS NOT NULL AND {ids}"
    if e == 7 and t == 'events':
        return f"UPDATE events SET doc = jsonb_set(doc, '{{parts,0,cat}}', '\"cat_{r.randrange(7)}\"') WHERE doc IS NOT NULL AND {ids}"
    if e == 8:
        return f'UPDATE {t} SET parent_id = {r.choice(["NULL", "parent_id + 1"])} WHERE {ids}'
    if e in (9, 10):
        return f'DELETE FROM {t} WHERE {ids}'
    if e in (11, 12, 13):
        n = r.choice([5, 40, 200])
        if t == 'events':
            lo = ctx['next_ev']; ctx['next_ev'] += n
            return f'INSERT INTO events SELECT * FROM fz.gen_events({lo}, {lo + n - 1}, {r.randrange(1, 9)})'
        lo = ctx['next_pt']; ctx['next_pt'] += n
        return f'INSERT INTO ev_part SELECT * FROM fz.gen_part({lo}, {lo + n - 1}, {r.randrange(1, 9)})'
    if e == 14:
        return r.choice([f'UPDATE items SET enabled = NOT enabled WHERE {ids}', f'UPDATE dims SET score = score + 1 WHERE a % {m % 50 + 3} = 0',
                         'DELETE FROM dims WHERE a = %d' % r.randrange(200), "UPDATE lookup SET derived_cat = 'm9' WHERE kind = 'k%d'" % r.randrange(5)])
    return r.choice(['VACUUM events', 'VACUUM ev_part', 'VACUUM (FREEZE) events', 'ANALYZE events'])


KCUT_COLS = {'ki': ['ki + 1', 'NULL', '39 - ki', '7'], 'kd': ["kd + {d}", "'infinity'", "'-infinity'", 'NULL', "date '2025-03-01'"],
             'kt': ["kt + interval '{d} days'", "'infinity'", 'NULL', "timestamp '2025-02-02 02:02:02'"], 'kn': ['kn * 2', "'NaN'", 'NULL', '1.5'],
             'kf': ['kf + 1', "'-0'", "'NaN'", 'NULL', "'Infinity'"]}
KCUT_KEY_OF = {'a': 'ki, kd', 'b': 'kt, kn', 'c': 'kf, ki'}


def dml_kcut(r, ctx):
    """Statements on the key-cut tables: churn of the keys (rows move between units), deletes, inserts, VACUUM, and every way the units are written again: a promotion of the write
    buffer, a merge, a refresh, a REINDEX, and a merge of units written before the key was declared (a key that is not in the order of the segments it merges)."""
    x = r.choice('abc')
    t, idx = f'kcut_{x}', f'kcut_{x}_idx'
    m = r.choice([13, 40, 97, 211])
    ids = f'id % {m} = {r.randrange(m)}'
    e = r.randrange(16)
    if e < 4:
        c = r.choice(KCUT_KEYS)
        return [f'UPDATE {t} SET {c} = {r.choice(KCUT_COLS[c]).format(d=r.randrange(1, 40))} WHERE {ids}']
    if e == 4:
        return [f'UPDATE {t} SET amount = amount + {r.randrange(1, 9)}, val = val + 1 WHERE {ids}']
    if e in (5, 6):
        return [f'DELETE FROM {t} WHERE {ids}']
    if e in (7, 8, 9):
        n = r.choice([5, 40, 200])
        lo = ctx['next_kc']; ctx['next_kc'] += n
        return [f'INSERT INTO {t} SELECT * FROM fz.gen_kcut({lo}, {lo + n - 1}, {r.randrange(1, 9)})']
    if e == 10:
        return [f'VACUUM {t}']
    if e == 11:
        return [f"SELECT sift.col_promote('{idx}')"]
    if e == 12:
        return [r.choice([f"SELECT sift.col_merge('{idx}', true)", f"SELECT sift.col_merge('{idx}', true)", f"SELECT count(*) FROM sift.col_refresh('{idx}')", f'REINDEX INDEX {idx}'])]
    if e == 13:
        # segments written without the key, merged into units cut by it
        lo = ctx['next_kc']; ctx['next_kc'] += 60
        return [f'ALTER INDEX {idx} RESET (key_cut)', f'INSERT INTO {t} SELECT * FROM fz.gen_kcut({lo}, {lo + 59}, {r.randrange(1, 9)})', f"SELECT sift.col_promote('{idx}')",
                f"ALTER INDEX {idx} SET (key_cut = '{KCUT_KEY_OF[x]}')", f"SELECT sift.col_merge('{idx}', true)"]
    if e == 14:
        # another key: the segments cut by the old one stop a merge (the REINDEX hint of the error), so the index is built again
        return [f"ALTER INDEX {idx} SET (key_cut = '{r.choice(['ki', 'kd', 'kt', 'kn', 'kf', 'ki, kd', 'kd, kt', 'kn, kf', KCUT_KEY_OF[x]])}')", f'REINDEX INDEX {idx}']
    return [f'ANALYZE {t}']


def dml_nullzone(r, ctx):
    """Soft deletes and restores, NULLing and filling of the columns the family tests, inserts, deletes and VACUUM on nzf: units whose NULL count changes under the zones."""
    m = r.choice([13, 40, 97, 211])
    ids = f'id % {m} = {r.randrange(m)}'
    e = r.randrange(14)
    if e < 3:
        return f'UPDATE nzf SET deleted_at = now() WHERE {ids}'
    if e == 3:
        return f'UPDATE nzf SET deleted_at = NULL WHERE {ids}'
    if e == 4:
        return f'UPDATE nzf SET deleted_at = timestamptz \'2025-08-01\' WHERE id BETWEEN {r.randrange(1, 800)} AND {r.randrange(800, 2400)}'
    if e == 5:
        return f"UPDATE nzf SET seen_at = {r.choice(['NULL', 'date ' + chr(39) + '2025-05-05' + chr(39)])} WHERE {ids}"
    if e == 6:
        return f"UPDATE nzf SET tag = {r.choice(['NULL', chr(39) + 'zz' + chr(39)])} WHERE {ids}"
    if e == 7:
        return f"UPDATE nzf SET score = {r.choice(['NULL', chr(39) + 'NaN' + chr(39), '1.5'])} WHERE {ids}"
    if e == 8:
        return f'UPDATE nzf SET kind = NULL, amount = amount + 1 WHERE {ids}'
    if e == 9:
        return f'DELETE FROM nzf WHERE {ids}'
    if e in (10, 11):
        n = r.choice([5, 40, 300])
        lo = ctx['next_nz']; ctx['next_nz'] += n
        return f'INSERT INTO nzf SELECT * FROM fz.gen_nzf({lo}, {lo + n - 1}, {r.randrange(1, 9)})'
    if e == 12:
        return r.choice(['VACUUM nzf', 'VACUUM (FREEZE) nzf'])
    return r.choice(["SELECT sift.col_promote('nzf_col')", "SELECT sift.col_merge('nzf_col', true)"])


# ---------- a run ----------
def sqlq(s):
    assert '$fzq$' not in s
    return f'$fzq${s}$fzq$'


def keycols(ordered):
    return 'NULL::int[]' if ordered else "'{}'::int[]"


def query_script(qid, sql, ordered, base, vs):
    out = [f'\\echo FZQ|{qid}', 'SELECT fz.start();', f"SELECT fz.ref('{qid}', {sqlq(sql)}, '{base};{REF}');"]
    for name, spec in vs:
        out.append(f"SELECT fz.var('{qid}', {sqlq(sql)}, {keycols(ordered)}, '{name}', '{base};{spec}');")
    return '\n'.join(out)


def lines_of(out):
    return [l for l in out.splitlines() if l.startswith('FZ|')]


def gen_query(seed, i, ctx, only):
    r = random.Random(seed * 1000003 + i)
    names = [n for n in FAMILIES if not only or n in only]
    fam = r.choices(names, [FAMILIES[n][2] for n in names])[0]
    gen, render, _ = FAMILIES[fam]
    parts = gen(r, ctx)
    return fam, parts, variants(r, fam), r.random() < 0.3


def episodes(seed, nq, ctx, only, dmls=4):
    """[(dml statements, [query entries])]: 4-10 queries after 0-4 statements."""
    r = random.Random(seed * 7919 + 1)
    rk = random.Random(seed * 104729 + 3)
    rn = random.Random(seed * 1299709 + 5)
    done, eps = 0, []
    while done < nq:
        st = [dml(r, ctx) for _ in range(r.randrange(dmls + 1))]
        if not only or 'nullzone' in only:
            st += [dml_nullzone(rn, ctx) for _ in range(rn.randrange(dmls + 1))]
        if not only or 'keycut' in only:
            st += [q for _ in range(rk.randrange(dmls + 1)) for q in dml_kcut(rk, ctx)]
        qs = []
        for _ in range(min(nq - done, r.randrange(4, 11))):
            fam, parts, vs, nl = gen_query(seed, done, ctx, only)
            qs.append((f's{seed}q{done}', fam, parts, vs, nl))
            done += 1
        eps.append((st, qs))
    return eps


def render_query(fam, parts):
    sql, ordered = FAMILIES[fam][1](parts)
    return sql, ordered


CRASH = re.compile(r'server closed the connection|connection to server was lost|terminating connection because of crash|could not send data to server')
PRE = "SET statement_timeout = '10s';\nSET jit = off;\nLOAD 'pg_sift';\n"


def run_script(db, stmts, entries):
    """Runs DML statements then queries; returns {qid: {'ref': status, variant: (status, used, detail)}}."""
    text = PRE + ''.join(s + ';\n' for s in stmts) + '\n'.join(entries) + '\n'
    out, err = psql(db, text, check=False)
    res = {}
    for l in lines_of(out):
        f = l.split('|', 5) + [''] * 6
        q = res.setdefault(f[1], {})
        if f[2] == 'ref':
            q['ref'] = (f[3], f[4])
        else:
            q[f[2]] = (f[3], f[4], f[5])
    return res, err


def diverged(res):
    return {n: v for n, v in res.items() if n != 'ref' and v[0] == 'DIFF'}


# ---------- shrinking ----------
def shrink_parts(db, qid, fam, parts, vs, nl, base):
    """Drops list elements and optional parts while the query still diverges in the state of db."""
    def reproduces(p):
        try:
            sql, ordered = render_query(fam, p)
        except Exception:
            return False
        res, _ = run_script(db, [], [query_script(qid, sql, ordered, base, vs)])
        return bool(diverged(res.get(qid, {})))

    changed = True
    while changed:
        changed = False
        for k, v in list(parts.items()):
            if isinstance(v, list):
                for i in range(len(v) - 1, -1, -1):
                    cand = dict(parts, **{k: v[:i] + v[i + 1:]})
                    if reproduces(cand):
                        parts, v, changed = cand, cand[k], True
            elif isinstance(v, bool) and v and k in ('neg', 'total'):
                cand = dict(parts, **{k: False})
                if reproduces(cand):
                    parts, changed = cand, True
            elif v and k in ('join', 'parent', 'order', 'spine', 'lookup', 'nested', 'grouped', 'offset') and not isinstance(v, list):
                cand = dict(parts, **{k: None if k not in ('join', 'parent') else '', })
                if k == 'nested':
                    cand[k] = False
                if k == 'offset':
                    cand[k] = 0
                if reproduces(cand):
                    parts, changed = cand, True
    return parts


def shrink_dml(base_db, work, hist, check):
    """Delta debugging of the DML statements before the query: drops chunks while check(db) still diverges."""
    n = len(hist) // 2 or 1
    while n >= 1 and hist:
        i, progressed = 0, False
        while i < len(hist):
            cand = hist[:i] + hist[i + n:]
            create_db(work, base_db)
            if check(work, cand):
                hist, progressed = cand, True
            else:
                i += n
        if not progressed:
            n //= 2
    return hist


def repro_sql(seed, rows, parts_n, hist, sql, base, ref_spec, var):
    name, spec = var
    return f"""-- pg_sift differential fuzz reproducer: seed {seed}, rows {rows}, partitions {parts_n}; the query returns other rows under the settings of
-- "{name}" than under the reference (sift.enable_agg off). Run it on a database with the extension, psql -f this file.
{data_sql(seed, rows, parts_n)}
-- the statements that ran before the query
{chr(10).join(s + ';' for s in hist)}
-- the settings, the reference first
CREATE TEMP TABLE r_ref AS
{sql};
\\set ON_ERROR_STOP off
{chr(10).join(f"SET {kv.split('=', 1)[0]} = '{kv.split('=', 1)[1]}';" for kv in (base + ';' + spec).split(';') if kv)}
CREATE TEMP TABLE r_got AS
{sql};
-- exact rows only on one side (floats can differ in the last digits: tolerance 1e-12 + 1e-9 relative)
SELECT 'only in reference' AS side, * FROM (TABLE r_ref EXCEPT ALL TABLE r_got) a
UNION ALL SELECT 'only in variant', * FROM (TABLE r_got EXCEPT ALL TABLE r_ref) b;
"""


def handle(seed, rows, parts_n, qid, fam, parts, vs, nl, hist, res, out_dir, tag):
    """Shrinks a divergence and writes the reproducer."""
    base_db = f'fz_{tag}_base'
    work = f'fz_{tag}_work'
    bad = diverged(res)
    # the variants that differ, the state replayed once
    vs_bad = [(n, s) for n, s in vs if n in bad]
    base = BASE + (';enable_nestloop=off' if nl else '')
    create_db(work, base_db)
    psql(work, ''.join(s + ';\n' for s in hist), check=False)
    p2 = shrink_parts(work, qid, fam, parts, vs_bad, nl, base)

    def check(db, h):
        psql(db, ''.join(s + ';\n' for s in h), check=False)
        sql, ordered = render_query(fam, p2)
        r2, _ = run_script(db, [], [query_script(qid, sql, ordered, base, vs_bad)])
        return bool(diverged(r2.get(qid, {})))

    h2 = hist
    if len(hist) <= 80:
        h2 = shrink_dml(base_db, work, hist, lambda db, h: check(db, h))
    sql, ordered = render_query(fam, p2)
    create_db(work, base_db)
    psql(work, ''.join(s + ';\n' for s in h2), check=False)
    r3, _ = run_script(work, [], [query_script(qid, sql, ordered, base, vs_bad)])
    bad3 = diverged(r3.get(qid, {}))
    name = next(iter(bad3), vs_bad[0][0]) if bad3 else vs_bad[0][0]
    spec = dict(vs)[name]
    text = repro_sql(seed, rows, parts_n, h2, sql, base, REF, (name, spec))
    detail = bad3.get(name, bad.get(name, ('', '', '')))[2] if bad3 else list(bad.values())[0][2]
    path = None
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        path = f'{out_dir}/{qid}.sql'
        open(path, 'w').write(f'-- family {fam}; variant {name}; {detail[:600]}\n' + text)
    psql('postgres', f'DROP DATABASE IF EXISTS {work} WITH (FORCE)')
    return sql, name, detail, path, len(h2), len(hist)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--seeds', default='1')
    ap.add_argument('--queries', type=int, default=40)
    ap.add_argument('--rows', type=int, default=3000)
    ap.add_argument('--parts', type=int, default=4)
    ap.add_argument('--families', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--no-shrink', action='store_true')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--dml', type=int, default=4, help='most DML statements before a group of queries')
    ap.add_argument('--leader-on', action='store_true', help='parallel_leader_participation stays on (a worker crash of leaderless parallel scans hides other findings)')
    ap.add_argument('--show', action='store_true', help='print every query')
    a = ap.parse_args()
    seeds = []
    for part in a.seeds.split(','):
        lo, _, hi = part.partition('-')
        seeds += range(int(lo), int(hi or lo) + 1)
    only = set(filter(None, a.families.split(',')))
    if a.leader_on:
        LEADER[:] = ['on']
    total = {'CRASH': 0, 'ok': 0, 'DIFF': 0, 'GENERR': 0, 'timeout': 0, 'queries': 0}
    pushed = {f: [0, 0] for f in FAMILIES}
    found = []
    t0 = time.time()
    for seed in seeds:
        tag = f'{seed}'
        base_db = f'fz_{tag}_base'
        build_base(base_db, seed, a.rows, a.parts)
        ctx = {'rows': a.rows, 'next_ev': a.rows + 1000, 'next_pt': a.rows + 1000, 'next_kc': a.rows + 1000, 'next_nz': a.rows + 1000}
        eps = episodes(seed, a.queries, ctx, only, a.dml)
        run_db = f'fz_{tag}_run'
        create_db(run_db, base_db)
        hist = []
        for st, qs in eps:
            entries, qinfo = [], {}
            for qid, fam, parts, vs, nl in qs:
                sql, ordered = render_query(fam, parts)
                base = BASE + (';enable_nestloop=off' if nl else '')
                entries.append(query_script(qid, sql, ordered, base, vs))
                qinfo[qid] = (fam, parts, vs, nl, sql)
                if a.show:
                    print(f'-- {qid} {fam}: {sql}')
            res, err = run_script(run_db, st, entries)
            hist += st
            lo = 0
            while CRASH.search(err):
                # a backend died: the first query without all its lines is the one; the queries after it run again
                bad_i = next((i for i in range(lo, len(qs)) if len(res.get(qs[i][0], {})) < 1 + len(qs[i][3])), None)
                if bad_i is None:
                    break
                qid, fam, parts, vs, nl = qs[bad_i]
                got = res.get(qid, {})
                where_ = 'reference' if 'ref' not in got else next((n for n, _ in vs if n not in got), '?')
                print(f'CRASH {qid} {fam} in {where_} (settings {dict(vs).get(where_, REF)}) after {len(hist)} statements\n  {qinfo[qid][4]}')
                total['CRASH'] += 1
                found.append((seed, qid))
                if a.out:
                    os.makedirs(a.out, exist_ok=True)
                    open(f'{a.out}/crash_{qid}.sql', 'w').write('\n'.join(h + ';' for h in hist) + f'\n-- {where_}: {dict(vs).get(where_, REF)}\n{qinfo[qid][4]};\n')
                res[qid] = dict(got, ref=('CRASH', ''))
                lo = bad_i + 1
                if lo >= len(qs):
                    break
                res2, err = run_script(run_db, [], entries[lo:])
                res.update(res2)
            for qid, (fam, parts, vs, nl, sql) in qinfo.items():
                total['queries'] += 1
                q = res.get(qid)
                if q is None or 'ref' not in q:
                    total['timeout'] += 1
                    continue
                if q['ref'][0] == 'CRASH':
                    continue
                if q['ref'][0] == 'GENERR':
                    total['GENERR'] += 1
                    print(f'GENERATOR ERROR {qid} {fam}: {sql}\n  {err.strip()[:300]}')
                    continue
                vars_ = [n for n, _ in vs]
                for n in vars_:
                    if n in q and q[n][0] != 'SKIP':
                        pushed[fam][1] += 1
                        pushed[fam][0] += q[n][1] == '1'
                    elif n not in q:
                        total['timeout'] += 1
                bad = diverged(q)
                if bad:
                    total['DIFF'] += 1
                    print(f'DIVERGENCE {qid} {fam} variants {sorted(bad)}\n  {sql}\n  {list(bad.values())[0][2][:500]}')
                    found.append((seed, qid))
                    if not a.no_shrink:
                        s2, name, detail, path, nh, nh0 = handle(seed, a.rows, a.parts, qid, fam, parts, vs, nl, hist, q, a.out, tag)
                        print(f'  shrunk to ({name}, {nh}/{nh0} statements): {s2}\n  {path or ""}')
                else:
                    total['ok'] += 1
        psql('postgres', f'DROP DATABASE IF EXISTS {run_db} WITH (FORCE)')
        if not a.keep:
            psql('postgres', f'DROP DATABASE IF EXISTS {base_db} WITH (FORCE)')
    print('families, share of variant runs that used SiftAgg: ' + ', '.join(f'{f} {100 * p[0] // max(p[1], 1)}% of {p[1]}' for f, p in pushed.items() if p[1]))
    print(f"diff_fuzz: {total['queries']} queries in {time.time() - t0:.0f}s: {total['ok']} equal, {total['DIFF']} divergent, {total['GENERR']} generator errors, {total['CRASH']} crashes, "
          f"{total['timeout']} timeouts")
    return 1 if total['DIFF'] or total['GENERR'] or total['CRASH'] else 0


if __name__ == '__main__':
    sys.exit(main())
