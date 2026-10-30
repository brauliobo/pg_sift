# The predicates, keys and aggregates the families draw from, by the columns a table has.
# Each entry names the columns it needs.


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
