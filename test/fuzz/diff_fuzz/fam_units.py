# Families of what a unit stores: key_cut, TID ranges, dead rows, sorted-run windows, NULL counts and array lengths.
# Each family is a gen(r, ctx) and a render(parts), with the DML that shapes its units.

from .fam_basic import grouped, where

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
    t = r.choice(['kcut_a', 'kcut_b', 'kcut_c', 'kcut_d'])
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


# ---------- units cut by TID range: the overlays of churn in a plain index, the rows of the normal plan after every kind of promotion and merge ----------
def gen_floor(r, ctx):
    p = gen_keycut(r, ctx)
    p['table'] = 'ufl'
    if r.random() < 0.4:
        lo = r.randrange(0, 600)
        p['where'].append(f"e.ctid >= '({lo},1)' AND e.ctid < '({lo + r.randrange(1, 900)},1)'")
    return p


# ---------- a unit with dead rows whose pages are all-visible answers net of them (docs/COL-PREAGG.md 12): the rows of the normal plan after deletes and VACUUM, with the share and without ----------
DEAD_KEYS = ['{e}.kind', '{e}.ki', '{e}.kd', "date_trunc('month', {e}.kd)::date", "date_trunc('day', {e}.kt)::date", '({e}.kn IS NULL)', '{e}.ki % 5']
DEAD_AGGS = ['count(*)', 'count({e}.ki)', 'count({e}.kn)', 'sum({e}.amount)', 'avg({e}.amount)', 'sum({e}.kn)', 'sum({e}.ki)', 'avg({e}.kn)', 'sum({e}.val)',
             'min({e}.amount)', 'max({e}.kn)', 'count(DISTINCT {e}.kind)']


def dead_pred(r, e):
    return r.choice([f"{e}.kind = 'k{r.randrange(5)}'", f"{e}.kind IN ('k{r.randrange(5)}', 'k{r.randrange(5)}')", f'{e}.ki < {r.randrange(40)}', f'{e}.ki IS NULL', f'{e}.kn IS NOT NULL',
                     f"{e}.kd >= date '2025-0{r.randrange(1, 8)}-15'", f'{e}.ki {r.choice(["=", "<>"])} {r.randrange(40)}',
                     f"{e}.ctid >= '({r.randrange(600)},1)' AND {e}.ctid < '({r.randrange(600, 1500)},1)'"])


def gen_dead(r, ctx):
    ks = list(dict.fromkeys(k.format(e='e') for k in r.sample(DEAD_KEYS, r.choice([0, 1, 1, 2]))))
    ag = list(dict.fromkeys(a.format(e='e') for a in r.sample(DEAD_AGGS, r.randrange(1, 5)) if r.random() < 0.9 or len(ks) == 0))
    if not ag:
        ag = ['count(*)']
    return dict(table='udd', form='agg', keys=ks, aggs=ag, where=[dead_pred(r, 'e') for _ in range(r.choice([0, 0, 1, 1, 2]))], having=[], ordered=True)


# ---------- a range on a leaf with a sorted run (order_columns): a unit takes the rows of the range from the run, not from a test of every row ----------
WINDOW_COLS = ['ki', 'kd', 'kt', 'kn']


def window_pred(r, e):
    c = r.choice(WINDOW_COLS + WINDOW_COLS + ['kf', 'amount'])
    if c == 'amount':
        return f'{e}.amount {r.choice([">", "<", ">=", "<="])} {r.randrange(1000)}'
    lit = KCUT_LITERALS[c]
    form = r.randrange(8)
    if form < 3:
        return f'{e}.{c} >= {lit(r)} AND {e}.{c} < {lit(r)}'
    if form == 3:
        return f'{e}.{c} BETWEEN {lit(r)} AND {lit(r)}'
    if form == 4:
        return f'{e}.{c} {r.choice([">", "<", ">=", "<="])} {lit(r)} AND {e}.{c} {r.choice([">", "<", ">=", "<="])} {lit(r)} AND {e}.{c} <> {lit(r)}'
    if form == 5:
        return f'{e}.{c} = {lit(r)}'
    if form == 6:
        return f'{e}.{c} {r.choice([">", "<", ">=", "<="])} {lit(r)}'
    return f'{e}.{c} >= {lit(r)} AND {e}.{c} <= {lit(r)} AND {e}.ki IS NOT NULL'


def gen_window(r, ctx):
    form = r.choice(['agg', 'agg', 'agg', 'rows'])
    where_ = [window_pred(r, 'e') for _ in range(r.choice([1, 1, 1, 2]))]
    if form == 'agg':
        ks = list(dict.fromkeys(k.format(e='e') for k in r.sample(KCUT_KEY_EXPRS, r.choice([0, 0, 1, 2]))))
        ag = list(dict.fromkeys(a.format(e='e') for a in r.sample(KCUT_AGGS, r.randrange(1, 5))))
        return dict(table='kcut_w', form=form, keys=ks, aggs=ag, where=where_, having=[], ordered=True)
    return dict(table='kcut_w', form=form, keys=[], aggs=[], where=where_, having=[], ordered=True, order='e.id', limit=r.choice([1, 5, 20, 100, 400]), offset=0)


def render_window(p):
    if p['form'] == 'agg':
        return grouped(p, f"{p['table']} e"), True
    return f"SELECT e.id, e.ki, e.kd, e.kt, e.kn, e.amount FROM {p['table']} e {where(p)} ORDER BY e.id", True


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


# ---------- the lengths of arrays: cardinality, array_length(x, 1) and IS [NOT] NULL of an array that the index keeps as two integer lanes, across DML ----------
AR_ARRAYS = ['ia', 'ta', 'tb', 'na', 'ma']


def ar_pred(r, e):
    c = r.choice(AR_ARRAYS)
    k = r.randrange(0, 8)
    form = r.randrange(14)
    if form < 3:
        return f'{e}.{c} IS {r.choice(["", "NOT "])}NULL'
    if form < 6:
        return f'cardinality({e}.{c}) {r.choice(["=", "<", ">", ">=", "<>"])} {k}'
    if form < 8:
        return f'array_length({e}.{c}, 1) {r.choice(["=", "<=", ">"])} {k}'
    if form == 8:
        return f'array_length({e}.{c}, {r.choice([0, 2, 2, 3])}) IS {r.choice(["", "NOT "])}NULL'
    if form == 9:
        return f'coalesce(cardinality({e}.{c}), 0) {r.choice(["=", ">"])} {k}'
    if form == 10:
        return f'({e}.{c} IS NOT NULL AND cardinality({e}.{c}) > 0)'
    if form == 11:
        return f'({r.choice(["cardinality({e}.{c})", "array_length({e}.{c}, 1)"]).format(e=e, c=c)} > {k} OR {e}.org = {r.randrange(12)})'
    if form == 12:
        return ar_elem_pred(r, e, k)
    return f'{e}.org {r.choice(["=", "<", ">"])} {r.randrange(12)}'


def ar_elem_pred(r, e, k):
    """The element tests of the arrays that the index lists (ta, tb: text, ia: int4), with and without the image: contains, overlap, contained by, ANY, equality, negations."""
    t = lambda: f"'t{r.randrange(32)}'"
    ts = lambda n: ', '.join(t() for _ in range(n))
    form = r.randrange(21)
    if form == 0:
        return f"{e}.ta @> ARRAY[{ts(r.choice([1, 1, 2, 3]))}]"
    if form == 1:
        return f"{e}.ta && ARRAY[{ts(r.choice([1, 2, 4, 9]))}]"
    if form == 2:
        return f"{e}.ta <@ ARRAY[{ts(r.choice([3, 9, 20]))}]"
    if form == 3:
        return f"{t()} = ANY({e}.ta)"
    if form == 4:
        return f"NOT ({t()} = ANY({e}.ta))"
    if form == 5:
        return f"{e}.tb @> ARRAY['u{k % 4}']"
    if form == 6:
        return f"{e}.tb && ARRAY['u{k % 4}', 'u0']"
    if form == 7:
        return f"{e}.tb = ARRAY['u{k % 4}', NULL, 'u0']"
    if form == 8:
        return f"{e}.ta = ARRAY[{ts(r.choice([1, 2, 3]))}]"
    if form == 9:
        return f"{e}.ia @> ARRAY[{k}, {k + 3}]"
    if form == 10:
        return f"{e}.ia && ARRAY[{k}, {k + 1}, {k * 2}]"
    if form == 11:
        return f"{k} = ANY({e}.ia)"
    if form == 12:
        return f"({e}.ta @> ARRAY[{t()}] OR {e}.ia @> ARRAY[{k}])"
    if form == 13:
        return f"({e}.ta @> ARRAY[{t()}] AND cardinality({e}.ta) {r.choice(['=', '>', '<'])} {r.randrange(1, 8)})"
    if form == 14:
        return f"NOT ({e}.ta && ARRAY[{ts(2)}])"
    if form == 15:
        return f"{e}.ia = ARRAY[{k}]"
    if form == 16:
        return f"{e}.ia = ARRAY[{k}, {k + r.choice([0, 3])}]"
    if form == 17:
        return f"NOT ({e}.ia = ARRAY[{k}, {k + 1}, {k}])"
    if form == 18:
        return f"{e}.tb IS NOT NULL AND 'u{k % 4}' = ANY({e}.tb)"
    lits = ', '.join(str(r.randrange(20)) for _ in range(r.choice([3, 12, 25])))
    return f"{'NOT ' if form == 20 else ''}({e}.ia <@ ARRAY[{lits}])"


AR_KEYS = ["({e}.ta @> ARRAY['t3'])", '({e}.ia && ARRAY[1, 2])', '{e}.org', '{e}.kind', 'cardinality({e}.ia)', 'cardinality({e}.ta)', 'array_length({e}.ma, 1)', 'array_length({e}.ta, 1)', '({e}.tb IS NULL)', '(cardinality({e}.na) > 2)']
AR_AGGS = ['count(*)', "sum(cardinality(e.ta)) FILTER (WHERE e.ta @> ARRAY['t7'])", 'count(*) FILTER (WHERE e.ia && ARRAY[3, 4])', "count(DISTINCT e.org) FILTER (WHERE e.tb @> ARRAY['u1'])", 'count({e}.ia)', 'count(cardinality({e}.ta))', 'sum(cardinality({e}.ia))', 'sum(cardinality({e}.ma))', 'avg(array_length({e}.ta, 1))', 'min(cardinality({e}.tb))',
           'max(array_length({e}.ma, 1))', 'max(cardinality({e}.na))', 'count(DISTINCT cardinality({e}.ta))', 'count(DISTINCT {e}.org)', 'sum({e}.amount)',
           'count(DISTINCT {e}.id) FILTER (WHERE {e}.ia IS NOT NULL AND cardinality({e}.ia) > 0)', 'count(*) FILTER (WHERE cardinality({e}.ta) = 0)',
           'count(*) FILTER (WHERE {e}.tb IS NULL)', 'sum({e}.amount) FILTER (WHERE array_length({e}.ma, 1) > 1)']


def gen_arrlen(r, ctx):
    where_ = [ar_pred(r, 'e') for _ in range(r.choice([0, 1, 1, 2, 3]))]
    if len(where_) > 1 and r.random() < 0.2:
        where_[:2] = [f'({where_[0]} OR {where_[1]})']
    ks = list(dict.fromkeys(k.format(e='e') for k in r.sample(AR_KEYS, r.choice([0, 0, 1, 1, 2]))))
    ag = list(dict.fromkeys(a.format(e='e') for a in r.sample(AR_AGGS, r.randrange(1, 5))))
    p = dict(table='arf', keys=ks, aggs=ag, where=where_, having=[], ordered=True)
    if r.random() < 0.3:
        # the elements of an array unnested: grouped by the element, counted, filtered by it
        c = r.choice(['ta', 'tb', 'ia'])
        txt = c != 'ia'
        p['frm'] = f'arf e, unnest(e.{c}) u'
        p['keys'] = r.choice([['u'], ['u'], [], ['e.kind'], ['u', 'e.kind'], ['u' if txt else 'u % 7']])
        p['aggs'] = list(dict.fromkeys(r.sample(['count(*)', 'count(u)', 'count(DISTINCT e.id)', 'min(u)', 'max(u)', 'sum(e.org)'] + ([] if txt else ['sum(u)', 'avg(u)']), r.randrange(1, 4))))
        if r.random() < 0.5:
            p['where'].append(f"u LIKE 't{r.randrange(4)}%'" if txt else f'u {r.choice(["<", ">", "=", "<>"])} {r.randrange(1000)}')
        if r.random() < 0.15:
            p['where'].append('u IS NULL')
    return p
