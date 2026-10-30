# Families of the pushdown shapes: dirty rows, lateral lookups, decorrelation, EXISTS, UNION ALL arms and top-N.
# Each family is a gen(r, ctx) and a render(parts).

import re

from .db import COLLATE_C
from .columns import EVENTS, KEYS, aggs, keys, preds
from .fam_basic import grouped, where

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


def gen_toplat(r, ctx):
    orders = ['f.ts', 'f.amount', 'f.val', 'f.a', 'f.big', 'f.kind', 'f.num', 'f.parent_id']
    key = r.choice([['grp'], ['parent_id'], ['a'], ['flag'], ['big'], ['kind'], ['grp', 'a'], ['ts']])
    ties = [f'f.{k} = e.{k}' for k in key]
    if key == ['ts']:
        ties = [r.choice(['f.ts >= e.ts', 'f.ts < e.ts', 'f.ts = e.ts'])]
    elif key[0] in ('grp', 'parent_id', 'a', 'big') and r.random() < 0.15:
        ties = [f'f.{key[0]} = e.{key[0]} + 0']
    return dict(form=r.choice(['lateral', 'lateral', 'left', 'scalar', 'sum']), ties=ties,
                order=[o + r.choice(['', ' DESC', ' NULLS FIRST', ' DESC NULLS LAST']) for o in r.sample(orders, r.randrange(1, 3))],
                limit=r.choice([1, 1, 2, 3, 5, 12, 400]), offset=r.choice([0, 0, 0, 2, 7]), pick=r.choice([7, 11, 40]), outer=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 2)),
                inner=preds(r, 'f', ctx['rows'], EVENTS, r.randrange(0, 3)))


def render_toplat(p):
    sub = lambda cols: (f"SELECT {cols} FROM events f WHERE {' AND '.join(p['ties'] + p['inner'])} ORDER BY {', '.join(p['order'])}, f.id "
                        f"LIMIT {1 if p['form'] == 'scalar' else p['limit']}" + ('' if p['form'] == 'scalar' else ' OFFSET %d' % p['offset']))
    wh = ' AND '.join([f"e.id % {p['pick']} = 0"] + p['outer'])
    if p['form'] == 'scalar':
        return f"SELECT e.id, ({sub('f.id')}) AS top FROM events e WHERE {wh} ORDER BY e.id", True
    if p['form'] == 'sum':
        return f"SELECT e.id, (SELECT count(*) + coalesce(sum(s.amount), 0) FROM ({sub('f.id, f.amount')}) s) AS t FROM events e WHERE {wh} ORDER BY e.id", True
    join = 'LEFT JOIN' if p['form'] == 'left' else 'JOIN'
    return f"SELECT e.id, x.id AS xid, x.amount FROM events e {join} LATERAL ({sub('f.id, f.amount')}) x ON true WHERE {wh} ORDER BY e.id, x.id", True


# a cast of a jsonb path that raises on a jsonb null, in a condition of the outer rows next to a condition on the sub-select: PostgreSQL may test the sub-select first and never
# reach the cast, or the reverse (a join order), so the error is not the plan's to reproduce
CAST_PATH = re.compile(r"metrics ->>? '\w+'\)::")


def gen_decor(r, ctx):
    tie = r.choice([['f.parent_id = e.parent_id'], ['f.grp = e.grp'], ["f.kind = e.kind"], ['f.parent_id = e.parent_id', 'f.grp = e.grp'], ['f.grp = e.grp', 'f.kind = e.kind']])
    return dict(form=r.choice(['lateral', 'left', 'comma', 'glateral', 'gleft', 'gcomma', 'select', 'where']), tie=tie,
                groups=r.sample(['f.kind', 'f.grp', 'f.flag', 'f.ts', 'f.a % 10', 'f.parent_id', 'f.grp % 4'], r.randrange(0, 3)), having=r.choice(['', '', 'count(*) > 1', 'min(f.amount) < 50']), aggs=r.sample(['count(*)', 'count(f.amount)', 'sum(f.amount)', 'min(f.ts)', 'max(f.kind)', 'avg(f.val)', 'bool_or(f.flag)',
                                                                                      'count(DISTINCT f.grp)', "sum(f.amount) FILTER (WHERE f.grp > 5)", 'array_agg(f.a ORDER BY f.id)', 'coalesce(sum(f.amount), -1)',
                                                                                      'sum(f.amount) / nullif(count(f.amount), 0)'], r.randrange(1, 4)),
                inner=preds(r, 'f', ctx['rows'], EVENTS, r.randrange(0, 3)), outer=[o for o in preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 2)) if CAST_PATH.search(o) is None], pick=r.randrange(4, 12),
                test=r.choice(['', 'IS NULL', 'IS NOT NULL', '> 100', '= 0', 'IS NULL OR {c} > 100']), total=r.random() < 0.3)


NUMERIC_AGG = re.compile(r'^(count|sum|avg|coalesce\(sum)')


def render_decor(p):
    if not NUMERIC_AGG.match(p['aggs'][0]) or 'sum(f.amount) / nullif' in p['aggs'][0]:
        p = dict(p, test=p['test'] if p['test'] in ('', 'IS NULL', 'IS NOT NULL') else 'IS NULL')

    # the outer rows are a few hundred: the normal plan looks up the group of each, the scan groups the table once
    wh = ' AND '.join([f"e.id % {p['pick']} = 0"] + p['outer'])
    sub = lambda i: f"SELECT {p['aggs'][i]} FROM events f WHERE {' AND '.join(p['tie'] + p['inner'])}"
    n = len(p['aggs'])
    if p['form'] in ('lateral', 'left', 'comma', 'glateral', 'gleft', 'gcomma'):
        join = {'lateral': 'JOIN', 'left': 'LEFT JOIN', 'comma': ',', 'glateral': 'JOIN', 'gleft': 'LEFT JOIN', 'gcomma': ','}[p['form']]
        grouped = p['form'][0] == 'g' and p['groups']
        gcols = [f"{g} AS g{j}" for j, g in enumerate(p['groups'] if grouped else [])]
        cols = ', '.join(gcols + [f"{p['aggs'][i]} AS v{i}" for i in range(n)])
        tail = ' GROUP BY ' + ', '.join(p['groups']) + (' HAVING ' + p['having'] if p['having'] else '') if grouped else ''
        frm = f"events e {join} LATERAL (SELECT {cols} FROM events f WHERE {' AND '.join(p['tie'] + p['inner'])}{tail}) x" + ('' if join == ',' else ' ON true')
        outs = [f'x.v{i}' for i in range(n)] + [f'x.g{j}' for j in range(len(gcols))]
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


def gen_exists(r, ctx):
    ties = ['f.parent_id = e.parent_id', 'f.grp = e.grp', 'f.kind = e.kind', 'f.ts = e.ts', 'f.flag = e.flag', 'f.big = e.big']
    inner = lambda: preds(r, 'f', ctx['rows'], EVENTS, r.randrange(0, 3)) + (['f.id < 0'] if r.random() < 0.05 else [])
    return dict(form=r.choice(['select', 'not', 'or', 'ornot', 'case', 'two', 'top', 'topnot']), tie=r.sample(ties, r.choice([1, 1, 2])), tie2=r.sample(ties, 1), inner=inner(), inner2=inner(),
                outer=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 2)) + (['e.id < 0'] if r.random() < 0.03 else []), pick=r.randrange(3, 10), total=r.random() < 0.3)


def render_exists(p):
    ex = lambda tie, inner: f"EXISTS (SELECT 1 FROM events f WHERE {' AND '.join(tie + inner)})"
    a, b = ex(p['tie'], p['inner']), ex(p['tie2'], p['inner2'])
    wh = ' AND '.join([f"e.id % {p['pick']} = 0"] + p['outer'])
    sel = {'select': f'e.id, {a} AS v0', 'not': f'e.id, NOT {a} AS v0', 'case': f"e.id, CASE WHEN {a} THEN 1 WHEN e.grp > 5 THEN 2 ELSE 3 END AS v0",
           'two': f'e.id, {a} AS v0, NOT {b} AS v1'}.get(p['form'], 'e.id')
    cond = {'or': f' AND (e.grp < 4 OR {a})', 'ornot': f' AND (e.grp < 4 OR NOT {a})', 'top': f' AND {a}', 'topnot': f' AND NOT {a}'}.get(p['form'], '')
    if p['total']:
        sel = 'count(*) AS n' + (f', count(*) FILTER (WHERE {a}) AS hits' if p['form'] in ('select', 'not', 'case', 'two') else '')
    return f'SELECT {sel} FROM events e WHERE {wh}{cond}', False


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
    order = [f"{xform[k](f'e.{k}') if k in xform and r.random() < 0.5 else f'e.{k}' + (COLLATE_C if k == 'title' and r.random() < 0.8 else '')} {r.choice(['', 'DESC'])} {r.choice(['', 'NULLS FIRST', 'NULLS LAST'])}".strip() for k in ks]
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


def gen_unusedlat(r, ctx):
    """A LEFT (or INNER) JOIN LATERAL (.. LIMIT n) into a lookup table (NULL keys, duplicates, an empty table) that the statement reads or not."""
    p = dict(tbl=r.choice(['lookup', 'lookup_dup', 'lookup_empty']), lim=r.choice([1, 1, 1, 2]), order=r.choice(['', '', 'ORDER BY l.derived_cat', 'ORDER BY l.cat DESC, l.derived_cat']),
             inner=r.random() < 0.15, read=r.choice(['none', 'none', 'none', 'select', 'where', 'group', 'on']), volatile=r.random() < 0.1,
             keys=keys(r, 'e', EVENTS, r.randrange(0, 3)), aggs=['count(*)'] + aggs(r, 'e', EVENTS, r.randrange(0, 3)), where=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 3)),
             having=[], ordered=True)
    return p


def render_unusedlat(p):
    cond = 'l.kind = e.kind' + (' AND random() < 2' if p['volatile'] else '')
    on = 'x.derived_cat IS NOT NULL' if p['read'] == 'on' and not p['inner'] else 'true'
    frm = f"events e {'JOIN' if p['inner'] else 'LEFT JOIN'} LATERAL (SELECT l.derived_cat FROM {p['tbl']} l WHERE {cond} {p['order']} LIMIT {p['lim']}) x ON {on}"
    q = dict(p, keys=list(p['keys']), aggs=list(p['aggs']), where=list(p['where']))
    if p['read'] == 'select':
        q['aggs'].append('count(x.derived_cat)')
    elif p['read'] == 'where':
        q['where'].append('x.derived_cat IS NOT NULL')
    elif p['read'] == 'group':
        q['keys'].append('x.derived_cat')
    return grouped(q, frm), bool(q['keys'])
