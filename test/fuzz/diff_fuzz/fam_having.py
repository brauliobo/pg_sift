# The HAVING family (opt-in: weight 0 in the default mix; `--families having` runs it alone, `--having-weight N` adds it to the mix): conditions on aggregates (one or several, an expression of
# them, a FILTER one, one that the select list does not hold), on the keys, and sub-selects tied to a key, over WHERE, NULL keys and values, empty groups,
# ROLLUP / CUBE / GROUPING SETS with grouping(), top-N, DISTINCT, a window over the groups, UNION ALL of two grouped arms, a grouped sub-query read by an outer
# WHERE, a join to a lookup, and prepared statements (generic and custom plans). The reference is the normal plan (sift.enable_agg off); a reference that
# raises is the generator's fault. The thresholds are drawn from the scale of the groups (the rows over the keys' values), floats only in < and >.
# Spec: HAVING-FUZZ-SPEC of the coverage lane (test/sql/agg_having_*.sql are its fixed cases).

from .columns import COLUMNS, EVENTS, preds

# (columns, expression, number of values, bare column or None, type)
KEYS = [
    ({'kind'}, '{e}.kind', 6, 'kind', 'text'), ({'grp'}, '{e}.grp', 13, 'grp', 'int'), ({'flag'}, '{e}.flag', 3, 'flag', 'bool'), ({'grp'}, '{e}.grp % 4', 5, None, 'int'),
    ({'parent_id'}, '{e}.parent_id % 20', 21, None, 'int'), ({'parent_id'}, '{e}.parent_id', 601, 'parent_id', 'int'), ({'ts'}, "date_trunc('month', {e}.ts)::date", 14, None, 'date'),
    ({'a'}, '{e}.a % 7', 8, None, 'int'), ({'kind', 'grp'}, "{e}.kind || {e}.grp", 66, None, 'text'),
    ({'#join'}, 'i.kind', 5, 'i.kind', 'text'), ({'#join'}, 'i.grp', 13, 'i.grp', 'int'),
]
# (columns, template, kind of value): the kind picks the thresholds
AGGS = [
    (set(), 'count(*)', 'cnt'), ({'amount'}, 'count({e}.amount)', 'cnt'), ({'grp'}, 'count(DISTINCT {e}.grp)', 'd12'), ({'kind'}, 'count(DISTINCT {e}.kind)', 'd5'),
    ({'parent_id'}, 'count(DISTINCT {e}.parent_id)', 'cnt'), ({'amount'}, 'sum({e}.amount)', 'sum'), ({'amount'}, 'avg({e}.amount)', 'avg'), ({'amount'}, 'min({e}.amount)', 'min'),
    ({'amount'}, 'max({e}.amount)', 'max'), ({'amount'}, 'sum(DISTINCT {e}.amount)', 'sum'), ({'amount'}, 'avg(DISTINCT {e}.amount)', 'avg'), ({'num'}, 'sum({e}.num)', 'sum'),
    ({'num'}, 'avg({e}.num)', 'avg'), ({'val'}, 'sum({e}.val)', 'fsum'), ({'val'}, 'avg({e}.val)', 'favg'), ({'val'}, 'min({e}.val)', 'fmin'), ({'val'}, 'max({e}.val)', 'fmax'),
    ({'flag'}, 'bool_and({e}.flag)', 'bool'), ({'flag'}, 'bool_or({e}.flag)', 'bool'), ({'kind'}, 'min({e}.kind)', 'text'), ({'kind'}, 'max({e}.kind)', 'text'),
    ({'amount'}, 'stddev({e}.amount)', 'sd'), ({'amount'}, 'percentile_cont(0.5) WITHIN GROUP (ORDER BY {e}.amount)', 'avg'),
    ({'kind'}, "length(string_agg({e}.kind, ',' ORDER BY {e}.kind))", 'strlen'), ({'amount'}, 'cardinality(array_agg({e}.amount))', 'cnt'), ({'id'}, 'count({e})', 'cnt'),
    ({'#join'}, 'count(DISTINCT i.id)', 'cnt'), ({'#join'}, 'max(i.id)', 'max600'), ({'#join'}, 'sum(i.grp)', 'cnt'), ({'#join'}, 'count(i.grp)', 'cnt'),
]
INTS = ('cnt', 'sum', 'min', 'max', 'd12', 'd5', 'strlen', 'max600')
FLOATS = ('fsum', 'favg', 'fmin', 'fmax', 'sd')


class Q:
    """The literals of one query: a number or, in a prepared statement, the next parameter ($1 of the declared type)"""

    def __init__(self, prep):
        self.prep, self.types, self.args = prep, [], []

    def lit(self, v, ty='int'):
        s = f"'{v}'" if ty == 'text' else (f'{v:.3f}' if ty == 'float8' else str(v))
        if not self.prep:
            return s
        self.types.append(ty)
        self.args.append(s)
        return f'${len(self.types)}'


def agg_threshold(r, kind, s, q):
    if kind == 'cnt':
        return q.lit(max(0, int(s * r.choice([0, 0.3, 0.7, 1, 1.4, 3]) + r.randrange(-1, 2))))
    if kind == 'sum':
        return q.lit(int(s * 500 * r.uniform(0.1, 1.9)))
    if kind == 'avg':
        return q.lit(r.randrange(300, 700))
    if kind == 'min':
        return q.lit(r.randrange(0, 40))
    if kind == 'max':
        return q.lit(r.randrange(900, 1001))
    if kind == 'max600':
        return q.lit(r.randrange(550, 600))
    if kind == 'd12':
        return q.lit(r.randrange(0, 14))
    if kind == 'd5':
        return q.lit(r.randrange(0, 7))
    if kind == 'strlen':
        return q.lit(int(s * 3 * r.uniform(0.3, 1.7)))
    if kind == 'text':
        return q.lit('k%d' % r.randrange(6), 'text')
    if kind == 'fsum':
        return q.lit(round(r.uniform(-1, 1) * 30 * max(s, 1) ** 0.5, 3), 'float8')
    if kind == 'favg':
        return q.lit(round(r.uniform(-8, 8), 3), 'float8')
    if kind == 'fmin':
        return q.lit(round(r.uniform(-50, -35), 3), 'float8')
    if kind == 'fmax':
        return q.lit(round(r.uniform(35, 50), 3), 'float8')
    return q.lit(round(r.uniform(150, 420), 3), 'float8')       # sd


def agg_cond(r, e, a, s, q):
    """a condition on one aggregate: a comparison, BETWEEN, IN, IS [NOT] NULL, IS DISTINCT FROM, NOT (..)"""
    x, kind = a[1].format(e=e), a[2]
    if kind == 'bool':
        return r.choice([x, f'NOT {x}', f'{x} IS NULL', f'{x} IS NOT TRUE', f'{x} IS DISTINCT FROM {r.choice(["true", "false"])}'])
    ops = ['<', '<=', '>=', '>'] + (['=', '<>'] if kind in INTS or kind == 'text' else [])
    w = r.randrange(9)
    if w < 4:
        return f'{x} {r.choice(ops)} {agg_threshold(r, kind, s, q)}'
    if w == 4:
        lo = agg_threshold(r, kind, s, q)
        return f'{x} BETWEEN {lo} AND {agg_threshold(r, kind, s, q)}' if kind not in INTS and kind != 'text' else f'{x} >= {lo}'
    if w == 5 and kind in INTS:
        return f"{x} {r.choice(['', 'NOT '])}IN ({', '.join(agg_threshold(r, kind, s, q) for _ in range(r.randrange(2, 4)))})"
    if w == 6:
        return f"{x} IS {r.choice(['', 'NOT '])}NULL"
    if w == 7 and kind in INTS:
        return f'{x} IS DISTINCT FROM {agg_threshold(r, kind, s, q)}'
    return f'NOT ({x} {r.choice(ops)} {agg_threshold(r, kind, s, q)})'


def expr_cond(r, e, cols, s, q):
    """an expression of aggregates (integer division, greatest, a sum of aggregates, a ratio of two FILTER counts)"""
    c = [f'count(*) FILTER (WHERE {e}.amount > {r.randrange(300, 800)}) * 2 > count(*)'] if 'amount' in cols else []
    c += [f'sum({e}.amount) / nullif(count(*), 0) {r.choice(["<", ">"])} {q.lit(r.randrange(350, 650))}', f'sum({e}.amount) * 2 + count(*) > {q.lit(int(s * 1000 * r.uniform(0.3, 1.7)))}',
          f'coalesce(max({e}.amount), -1) - coalesce(min({e}.amount), -1) > {q.lit(r.randrange(500, 1000))}'] if 'amount' in cols else []
    if 'grp' in cols:
        c.append(f'greatest(count(*), count(DISTINCT {e}.grp)) > {q.lit(int(s * r.uniform(0.3, 1.5)))}')
    if 'kind' in cols and 'amount' in cols:
        c.append(f"coalesce(sum({e}.amount) FILTER (WHERE {e}.kind IN ('k{r.randrange(5)}', 'k{r.randrange(5)}')), 0) > {q.lit(int(s * 500 * r.uniform(0.1, 0.9)))}")
    return r.choice(c) if c else 'count(*) > 0'


def filter_cond(r, e, cols, s, q):
    ops = ['<', '>', '>=', '<=', '=', '<>']
    c = [f"count(*) FILTER (WHERE {e}.amount {r.choice(['>', '<'])} {r.randrange(1000)}) {r.choice(ops)} {q.lit(int(s * r.uniform(0.1, 0.8)))}"] if 'amount' in cols else []
    if 'kind' in cols:
        c.append(f"count(*) FILTER (WHERE {e}.kind = 'k{r.randrange(5)}') {r.choice(ops)} {q.lit(int(s * r.uniform(0.05, 0.5)))}")
    if 'val' in cols and 'grp' in cols:
        c.append(f"max({e}.val) FILTER (WHERE {e}.grp = {r.randrange(13)}) IS {r.choice(['', 'NOT '])}NULL")
    if 'amount' in cols and 'grp' in cols:
        c.append(f"sum({e}.amount) FILTER (WHERE {e}.grp IN ({r.randrange(6)}, {r.randrange(6, 13)})) {r.choice(['<', '>'])} {q.lit(int(s * 500 * r.uniform(0.05, 0.6)))}")
    return r.choice(c) if c else 'count(*) > 0'


def key_cond(r, k, q):
    x, ty = k[1].format(e='e'), k[4]
    if k[3] is None:
        x = f'({x})'
    if ty == 'bool':
        return r.choice([x, f'NOT {x}', f'{x} IS NULL', f'{x} IS NOT TRUE'])
    if ty == 'text':
        return r.choice([f"{x} = {q.lit('k%d' % r.randrange(6), 'text')}", f"{x} IN ('k1', 'k3', 'k9')", f'{x} IS NULL', f"{x} LIKE 'k%'", f"{x} <> {q.lit('k%d' % r.randrange(5), 'text')}",
                         f'{x} IS NOT NULL', f"{x} < 'k{r.randrange(6)}'"])
    if ty == 'date':
        d = f"date '2025-{r.randrange(1, 13):02d}-01'"
        return r.choice([f'{x} >= {d}', f'{x} < {d}', f'{x} IS NULL', f"{x} BETWEEN {d} AND date '2025-{r.randrange(6, 13):02d}-28'"])
    n = r.randrange(13)
    return r.choice([f"{x} = {q.lit(n)}", f'{x} IN ({n}, {r.randrange(13)}, {r.randrange(13)})', f'{x} IS NULL', f'{x} BETWEEN {n // 2} AND {n}', f'{x} > {q.lit(n)}', f'NOT ({x} < {q.lit(n)})',
                     f'{x} IS NOT NULL', f'{x} % 2 = {r.randrange(2)}'])


def sub_cond(r, k):
    """a sub-select tied to a bare key column: IN, NOT IN (with and without a NULL in the list), EXISTS, NOT EXISTS, scalar"""
    x, bare = k[1].format(e='e'), k[3]
    if bare is None:
        return None
    col = bare.split('.')[-1]
    if col == 'kind':
        return r.choice([f'{x} IN (SELECT kind FROM items WHERE enabled)', f'{x} NOT IN (SELECT kind FROM items WHERE grp = {r.randrange(12)})', f'{x} NOT IN (SELECT kind FROM items WHERE grp > 7 UNION ALL SELECT NULL)',
                         f"EXISTS (SELECT 1 FROM items s WHERE s.kind = {x} AND s.grp = {r.randrange(12)})", f"NOT EXISTS (SELECT 1 FROM items s WHERE s.kind = {x} AND s.grp = {r.randrange(12)})",
                         f"{x} = (SELECT max(kind) FROM items WHERE grp = {r.randrange(12)})"])
    if col == 'flag':
        return r.choice([f'{x} IN (SELECT enabled FROM items WHERE grp = {r.randrange(12)})', f'NOT EXISTS (SELECT 1 FROM items s WHERE s.enabled = {x} AND s.grp = {r.randrange(12)})'])
    return r.choice([f'{x} IN (SELECT {"id" if col == "parent_id" else "grp"} FROM items WHERE enabled AND id % 7 = {r.randrange(7)})',
                     f'{x} NOT IN (SELECT {"id" if col == "parent_id" else "grp"} FROM items WHERE kind = \'k{r.randrange(5)}\' AND id < {r.randrange(1, 100)})',
                     f'EXISTS (SELECT 1 FROM items s WHERE s.{"id" if col == "parent_id" else "grp"} = {x} AND s.kind = \'k{r.randrange(5)}\')',
                     f'NOT EXISTS (SELECT 1 FROM items s WHERE s.{"id" if col == "parent_id" else "grp"} = {x} AND NOT s.enabled)',
                     f'{x} < (SELECT count(*) FROM items WHERE enabled) / {r.choice([50, 100, 200])}'])


def having_of(r, e, cols, keys, s, q, sets):
    """the HAVING conditions: -> list (render_having joins them)"""
    parts = []
    for _ in range(r.choice([1, 1, 2, 2, 3])):
        w = r.randrange(20)
        usable = [a for a in AGGS if a[0] <= cols]
        if w < 9:
            parts.append(agg_cond(r, e, r.choice(usable), s, q))
        elif w < 11:
            parts.append(expr_cond(r, e, cols, s, q))
        elif w < 13:
            parts.append(filter_cond(r, e, cols, s, q))
        elif w < 15 and keys:
            parts.append(key_cond(r, r.choice(keys), q))
        elif w < 17 and keys:
            c = sub_cond(r, r.choice(keys))
            parts.append(c or key_cond(r, r.choice(keys), q))
        elif w == 17:
            parts.append(r.choice(['random() < 2', 'pg_backend_pid() > 0', 'true', 'false', 'NULL::bool', '1 = 1']))
        elif w == 18:
            parts.append(f'count(*) > (SELECT count(*) FROM items WHERE grp = {r.randrange(12)}) {r.choice(["- 40", "", "/ 3"])}')
        else:
            parts.append(agg_cond(r, e, r.choice(usable), s, q))
    if sets and keys:
        names = ', '.join(k[1].format(e=e) for k in keys[:r.randrange(1, len(keys) + 1)])
        parts.append(r.choice([f'grouping({names}) = 0', f'grouping({names}) <> 0', f'grouping({names}) < 2 OR count(*) > {q.lit(int(s))}']))
    return parts


def gen_having(r, ctx):
    shape = r.choices(['plain', 'sets', 'orderlimit', 'distinct', 'window', 'union', 'outer', 'join', 'empty', 'nokeys'], [8, 5, 4, 2, 2, 3, 2, 3, 1, 2])[0]
    t, cols, n = ('events', EVENTS, ctx['rows']) if r.random() < 0.85 else ('ev_part', COLUMNS, ctx['rows'] // 2)
    join = shape == 'join'
    have = cols | ({'#join'} if join else set())
    nk = 0 if shape == 'nokeys' else r.choice([1, 1, 2, 3]) if shape != 'empty' else r.choice([0, 1])
    pool = [k for k in KEYS if k[0] <= have]
    keys = []
    while len(keys) < nk:
        k = r.choice(pool)
        if k not in keys:
            keys.append(k)
    values = 1
    for k in keys:
        values *= k[2]
    where = preds(r, 'e', n, cols, r.choice([0, 0, 1, 1, 2]))
    if shape == 'empty':
        where = [r.choice(['e.id < 0', 'false', 'e.id < 0 AND e.kind IS NULL'])]
    s = max(n / values, 1) * 0.5 ** len(where) if shape != 'empty' else 1
    prep = r.random() < 0.2 and shape != 'empty'
    q = Q(prep)
    sets = shape == 'sets' and bool(keys)
    sel_aggs = [a for a in r.sample([a for a in AGGS if a[0] <= have], r.randrange(0, 4))]
    having = having_of(r, 'e', have, keys, s, q, sets)
    having2 = having if prep or shape != 'union' else having_of(r, 'e', have, keys, s, Q(False), False)
    if prep and r.random() < 0.5 and where:
        where = where + [f'e.amount >= {q.lit(r.randrange(0, 300))}'] if 'amount' in cols else where
    prep = prep and bool(q.types)         # a statement without a parameter is not prepared
    return dict(table=t, shape=shape, join=join, keys=[k[1].format(e='e') for k in keys], aggs=[a[1].format(e='e') for a in sel_aggs], where=where, having=having, having2=having2, glue=r.choice(['AND', 'AND', 'AND', 'OR', 'CASE']),
                sets=r.choice(['rollup', 'cube', 'gsets']) if sets else None, lim=r.choice([3, 10, 25]), off=r.choice([0, 0, 2]), prep=prep, ptypes=q.types, pargs=q.args,
                pid=r.randrange(10 ** 9), ties=r.random() < 0.3, left=r.random() < 0.4, ocut=int(s * r.choice([0.3, 0.6, 1])))


def having_text(conds, glue):
    if not conds:
        return 'true'
    if len(conds) == 1:
        return conds[0]
    if glue == 'CASE':
        return f'CASE WHEN {conds[0]} THEN {conds[1]} ELSE {conds[-1]} END'
    return f' {glue} '.join(f'({c})' for c in conds)


def select_of(p):
    ks = [f'{k} AS k{i}' for i, k in enumerate(p['keys'])]
    ag = [f'{a} AS a{i}' for i, a in enumerate(p['aggs'])]
    # grouping() before the aggregates: the comparison sorts rows by every column in order, and a float that differs in its last digits must not decide which of two groups of the same keys comes first
    items = ks + ([f"grouping({', '.join(p['keys'])}) AS g"] if p['sets'] else []) + ag or ['count(*) AS a0']
    if p['shape'] == 'outer':
        items.append('count(*) AS ac')
    if p['shape'] == 'window':
        items.append('rank() OVER (ORDER BY count(*) DESC) AS rk')
    return ', '.join(items)


def one_arm(p, having, w=None):
    keys = p['keys']
    frm = f"{p['table']} e"
    if p['join']:
        frm += f" {'LEFT JOIN' if p['left'] else 'JOIN'} items i ON i.id = e.parent_id"
    where = ' WHERE ' + ' AND '.join(p['where'] if w is None else w) if (p['where'] if w is None else w) else ''
    if not keys:
        g = ''
    elif p['sets'] == 'rollup':
        g = f" GROUP BY ROLLUP ({', '.join(keys)})"
    elif p['sets'] == 'cube':
        g = f" GROUP BY CUBE ({', '.join(keys)})"
    elif p['sets'] == 'gsets':
        g = f" GROUP BY GROUPING SETS (({', '.join(keys)}), ({keys[0]}), ())"
    else:
        g = f" GROUP BY {', '.join(keys)}"
    return f"SELECT {select_of(p)} FROM {frm}{where}{g} HAVING {having_text(having, p['glue'])}"


def render_having(p):
    sh, keys = p['shape'], p['keys']
    ordered = bool(keys) and not p['sets'] and sh in ('plain', 'join', 'empty', 'orderlimit')
    sql = one_arm(p, p['having'])
    if sh == 'union':
        sql = f"{sql} UNION ALL {one_arm(p, p['having2'], p['where'][:1])}"
    elif sh == 'distinct':
        sql = sql.replace('SELECT ', 'SELECT DISTINCT ', 1)
    elif sh == 'orderlimit' and keys and not p['sets']:
        measure = p['aggs'][0] if p['aggs'] else 'count(*)'
        if p['ties']:
            sql += f" ORDER BY {measure} DESC FETCH FIRST {p['lim']} ROWS WITH TIES"
            ordered = False
        else:
            sql += f" ORDER BY {measure} DESC NULLS LAST, {', '.join(keys)} LIMIT {p['lim']} OFFSET {p['off']}"
    elif sh == 'outer':
        sql = f"SELECT * FROM ({sql}) g WHERE g.ac > {p['ocut']}"
    elif ordered:
        sql += ' ORDER BY ' + ', '.join(str(i + 1) for i in range(len(keys)))
    if p['prep']:
        name = f"h{p['pid']}"
        return f"PREPARE {name}({', '.join(p['ptypes'])}) AS {sql}\n@@\nEXECUTE {name}({', '.join(p['pargs'])})", ordered
    return sql, ordered
