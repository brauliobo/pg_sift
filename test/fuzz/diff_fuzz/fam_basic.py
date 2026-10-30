# Families of the basic shapes: pre-aggregates, partitions, lookups, placeholders and per-leaf aggregates.
# Each family is a gen(r, ctx) and a render(parts).

from .columns import COLUMNS, EVENTS, _in, aggs, keys, preds

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
    return grouped(p, p.get('frm', f"{p['table']} e")), p['ordered'] and bool(p['keys'])


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



# a spine of periods (one matches no row) joined from the left to the parsed rows under grouping sets: the aggregates' arguments and FILTERs reach
# the scan as placeholders; a strict expression of the scanned columns is read as such, a constant or a non-strict one keeps the placeholder
PH_STRICT = ['e.val', 'e.amount * 2', "(e.metrics ->> 'm1')::float8", 'e.val + e.amount', "(r.value #>> '{}')::float8", "(r.value #>> '{}')::float8 * 10 - e.grp"]
PH_OTHER = ['coalesce(e.val, 0)', 'CASE WHEN e.val IS NULL THEN -1 ELSE e.val END', '1', 'greatest(e.amount, 500)', "coalesce((r.value #>> '{}')::float8, -1)",
            '(e.val IS NULL)::int']


def gen_placeholder(r, ctx):
    expr = r.choice(PH_STRICT + PH_OTHER)
    c = r.randrange(10)
    return dict(expr=expr, unnest='r.' in expr or r.random() < 0.3, m3=r.random() < 0.15, key=r.choice(['x.kind', 'x.key', 'x.grp']),
                sets=r.random() < 0.7, aggs=['count(*)'] + r.sample(['count(v)', 'sum(v)', 'avg(v)', 'min(v)', 'max(v)', f'count(*) FILTER (WHERE v > {c})',
                                                                    f'count(DISTINCT x.id) FILTER (WHERE v >= {c})', f'sum(v) FILTER (WHERE v < {c})',
                                                                    f'avg(v) FILTER (WHERE x.kind IS NOT NULL)'], r.randrange(1, 5)),
                where=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 2)))


def render_placeholder(p):
    key = p['key'] if p['unnest'] or p['key'] != 'x.key' else 'x.kind'
    fn = ', jsonb_each(e.metrics) r' if p['unnest'] else ''
    cond = [] if not p['unnest'] or p['m3'] else ["r.key <> 'm3'"]
    w = ' WHERE ' + ' AND '.join(p['where'] + cond) if p['where'] or cond else ''
    rk = ', r.key' if p['unnest'] else ''
    grp = f'GROUPING SETS ((p.period, {key}), (p.period))' if p['sets'] else f'p.period, {key}'
    sql = (f"WITH periods(period, lo, hi) AS (VALUES ('a', date '2025-01-01', date '2025-06-30'), ('b', date '2025-07-01', date '2026-02-04'), "
           f"('z', date '2030-01-01', date '2030-12-31')), parsed AS (SELECT e.id, e.kind, e.grp, e.ts{rk}, {p['expr']} AS v FROM events e{fn}{w}) "
           f"SELECT p.period, {key} AS k, {', '.join(f'{a} AS a{i}' for i, a in enumerate(p['aggs']))} FROM periods p LEFT JOIN parsed x ON x.ts BETWEEN p.lo AND p.hi GROUP BY {grp}")
    return sql, False
ONELEAF = {'kind': 'ok_kind', 'big': 'ok_big', 'val': 'ok_val', 'grp': 'ok_grp', 'ts': 'ok_ts'}

def gen_oneleaf(r, ctx):
    col = r.choice(sorted(ONELEAF))
    return dict(col=col, jointype=r.choice(['LEFT JOIN', 'LEFT JOIN', 'JOIN']), lateral=r.random() < 0.3,
                keys=r.sample(['d.tag', 'e.grp', 'e.kind', 'd.w % 3', f'e.{col}'], r.choice([0, 1, 1, 2])),
                aggs=['count(*)'] + r.sample(['count(d.tag)', 'max(d.w)', 'sum(d.w)', 'sum(e.amount)', 'count(DISTINCT d.tag)',
                                              'count(*) FILTER (WHERE d.w > 4)', 'avg(e.val) FILTER (WHERE d.tag IS NOT NULL)'], r.randrange(1, 4)),
                where=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 3)), having=[], ordered=True)


def oneleaf_from(p):
    t, c = ONELEAF[p['col']], p['col']
    if p['lateral']:
        return f"events e {p['jointype']} LATERAL (SELECT x.tag, x.w FROM {t} x WHERE x.k = e.{c} ORDER BY x.w, x.tag LIMIT 1) d ON true"
    return f"events e {p['jointype']} {t} d ON d.k = e.{c}"


def render_oneleaf(p):
    return grouped(p, oneleaf_from(p)), bool(p['keys'])


# sums and averages of numeric programs (+ - * of numeric(12,2), int columns and constants: 38 digits and more, large and negative operands, NULLs, NaN-free): the int64 vector blocks and the int128 interpreter
NUMSUM = ['sum(e.num * (100 - e.b) * (100 + e.a))', 'sum(e.num * (1 - e.b) * (1 + e.a))', 'sum(e.num * (1 - e.num) * (1 + e.num))', 'sum(e.num)', 'avg(e.num * e.amount)', 'sum(e.amount * e.amount * e.a)',
          'sum(e.num * e.num * e.num)', 'sum(e.num - e.amount * 2)', 'sum(-e.num * 3)', 'avg(e.num + e.a - e.b)', 'sum(e.num * (e.amount + 1)) FILTER (WHERE e.a > 3)', 'sum(DISTINCT e.num * 2)',
          'sum(e.num * 4294967296)', 'sum(e.amount * 2147483647 * e.a)', 'sum(e.wnum * e.a)', 'sum(e.wnum * 3 - e.num)']


def gen_numsum(r, ctx):
    return dict(table='events', keys=keys(r, 'e', EVENTS, r.choice([0, 1, 1, 2])), aggs=['count(*)'] + r.sample(NUMSUM, r.randrange(1, 5)),
                where=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 3)), having=[], ordered=True)


# count(DISTINCT) of a leaf with no set of its own (float, uuid, bytea, interval, 64-bit integer): ids from a combo, the workers send the values
DISTLEAF = ['count(DISTINCT e.val)', 'count(DISTINCT e.ref)', 'count(DISTINCT e.blob)', 'count(DISTINCT e.ivl)', 'count(DISTINCT e.big)', 'count(DISTINCT e.val) FILTER (WHERE e.a > 3)',
            'count(DISTINCT e.ref) FILTER (WHERE e.grp < 5)', 'count(e.val)', 'sum(e.amount)']


def gen_distleaf(r, ctx):
    return dict(table='events', keys=keys(r, 'e', EVENTS, r.choice([0, 1, 1, 2])), aggs=['count(*)'] + r.sample(DISTLEAF, r.randrange(1, 4)),
                where=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 3)), having=[], ordered=True)


# wide leaves (a numeric without a precision, a uuid, an interval) in the workers: counts, sums of an expression, variances and the key of a uuid go to them (the key of the numeric and its min, max and lists show the spelling of an equal value, which no plan keeps: left out)
WIDEW = ['count(e.inum)', 'sum(e.inum)', 'avg(e.inum)', 'sum(e.inum * 2)', 'sum(e.inum * e.a)', 'stddev(e.inum)', 'variance(e.inum)', 'sum(CASE WHEN e.a > 3 THEN e.inum ELSE 0 END)',
         'count(e.ref)', 'count(DISTINCT e.ref)', 'array_length(array_agg(e.ref), 1)', 'count(DISTINCT e.ivl)', 'count(e.ivl)', 'count(e.blob)', 'sum(e.wnum)', 'stddev(e.wnum)']


def gen_widew(r, ctx):
    return dict(table='events', keys=r.sample(['e.ref', 'e.blob', 'e.kind', 'e.grp', 'e.ts'], r.choice([0, 1, 1, 2])), aggs=['count(*)'] + r.sample(WIDEW, r.randrange(1, 4)),
                where=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 3)), having=[], ordered=True)


# two columns of one row compared (`a < b`): the cmp2 kernels of every SIMD level; the int4 columns of the events table pair up (the int8 one is the generic expression's; the types are in test agg_leaf_cmp)
COLCMP = ['e.a', 'e.b', 'e.amount', 'e.grp', 'e.parent_id', 'e.big']


def gen_colcmp(r, ctx):
    cmps = []
    for _ in range(r.randrange(1, 4)):
        x, y = r.sample(COLCMP, 2)
        cmps.append(f"{x} {r.choice(['<', '<=', '=', '<>', '>=', '>'])} {y}" if r.random() < 0.8 else f"NOT ({x} {r.choice(['<', '>'])} {y})")
    return dict(table='events', keys=keys(r, 'e', EVENTS, r.choice([0, 1, 1, 2])), aggs=['count(*)'] + r.sample(['sum(e.amount)', 'max(e.a)', 'avg(e.val)', 'count(DISTINCT e.grp)'], r.randrange(0, 3)),
                where=cmps + preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 2)), having=[], ordered=True)


# grouping sets over keys of many values (the group table is a hash table) and a lookup's column: the sets that read the same tuples share their key ids
SETKEYS = ['e.kind', 'e.grp', 'e.a', 'e.b', 'd.tag', 'e.ts', 'e.a % 7']


def gen_setkeys(r, ctx):
    p = gen_oneleaf(r, ctx)
    ks = r.sample(SETKEYS, r.randrange(3, 6))
    sets = [r.sample(ks, r.randrange(0, len(ks) + 1)) for _ in range(r.randrange(2, 5))]
    ks = [k for k in ks if any(k in st for st in sets)]
    return dict(p, keys=ks, sets=sets, aggs=['count(*)'] + r.sample(['sum(e.amount)', 'count(d.tag)', 'avg(e.val)', 'max(d.w)', 'count(DISTINCT e.id)',
                                                                     'count(*) FILTER (WHERE e.amount > 500)'], r.randrange(1, 4)))


def render_setkeys(p):
    sets = ', '.join('(' + ', '.join(sorted(st, key=p['keys'].index)) + ')' for st in p['sets'])
    w = ' WHERE ' + ' AND '.join(p['where']) if p['where'] else ''
    return f"SELECT {select_list(p)} FROM {oneleaf_from(p)}{w} GROUP BY GROUPING SETS ({sets})", False
