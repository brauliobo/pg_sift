# Families of exceptions, flat keys, key lists, shared CTE copies, global order, zone keys, key tables and dictionary counts.
# Each family is a gen(r, ctx) and a render(parts), with its DML.

from . import tablemode
from .columns import keys
from .fam_basic import grouped, where
from .dml import bulk_insert

# ---------- exceptions: IS NULL on a stamp that is set in a few rows of a unit, answered as the unit net of them (float sums within the tolerance of the others) ----------
EX_WHERE = ['e.stamp IS NULL'] * 6 + ['e.stamp IS NOT NULL', 'e.stamp IS NULL AND e.org < {k}', 'e.stamp IS NULL AND e.id < {n}', 'e.stamp IS NULL AND e.id > {n}',
                                       "e.stamp IS NULL AND e.kind = 'k{k4}'", 'e.stamp IS NULL AND e.amount >= {k}00', '(e.stamp IS NULL OR e.org = {k})']
EX_KEYS = ['e.org', 'e.kind', 'e.org % 3', '(e.id / 256)']
EX_AGGS = ['count(*)', 'count(e.val)', 'sum(e.amount)', 'avg(e.amount)', 'sum(e.val)', 'avg(e.val)', 'sum(e.dec)', 'avg(e.dec)', 'min(e.val)', 'max(e.dec)', 'count(e.stamp)',
           'sum(e.val) FILTER (WHERE e.org = {k})', 'count(DISTINCT e.kind)']
EX_ELEM_KEYS = ['r.key', "r.key || e.kind", 'e.org']
EX_ELEM_AGGS = ['count(*)', "sum((r.value #>> '{{}}')::float8)", "avg((r.value #>> '{{}}')::float8)", "min((r.value #>> '{{}}')::float8)", "sum((r.value #>> '{{}}')::float8) FILTER (WHERE e.org = {k})"]
EX_LIST_KEYS = ['p.cat', 'p.cat || e.kind', 'e.org']
EX_LIST_AGGS = ['count(*)', 'sum(p.score)', 'avg(p.score)', 'max(p.score)', 'count(p.score)']


def gen_exceptions(r, ctx):
    if not tablemode.TABLE and r.randrange(4) == 0:
        pred = r.choice(["(e.marker->>'x')::bigint", 'e.stamp'])
        return dict(table='exr', frm='exr e', keys=r.choice([[], ['e.kind'], ['e.org'], ['e.kind', 'e.org']]),
                    aggs=r.sample(['count(*)', 'sum(e.amount)', 'avg(e.amount)', 'min(e.amount)', 'max(e.amount)'], r.randrange(1, 4)),
                    where=['e.org >= 2', pred + r.choice([' IS NULL', ' IS NOT NULL'])], having=[], ordered=True)
    form = r.choice(['rows', 'rows', 'map', 'map', 'list'])
    fill = lambda t: t.format(k=r.randrange(1, 6), k4=r.randrange(5), n=r.randrange(100, max(ctx['rows'], 101)))
    where_ = [fill(r.choice(EX_WHERE))]
    if form == 'rows':
        ks = list(dict.fromkeys(r.sample(EX_KEYS, r.choice([0, 1, 1, 2]))))
        ag = list(dict.fromkeys(fill(a) for a in r.sample(EX_AGGS, r.randrange(1, 5))))
        frm = 'exf e'
    elif form == 'map':
        ks = list(dict.fromkeys(r.sample(EX_ELEM_KEYS, r.choice([1, 1, 2]))))
        ag = list(dict.fromkeys(fill(a).replace('{{}}', '{}') for a in r.sample(EX_ELEM_AGGS, r.randrange(1, 4))))
        frm = 'exf e, jsonb_each(e.metrics_a) r'
    else:
        ks = list(dict.fromkeys(r.sample(EX_LIST_KEYS, r.choice([1, 1, 2]))))
        ag = list(dict.fromkeys(fill(a) for a in r.sample(EX_LIST_AGGS, r.randrange(1, 4))))
        frm = "exf e, jsonb_to_recordset(e.parts) p(cat text, score float8)"
    return dict(table='exf', frm=frm, keys=ks, aggs=ag, where=where_, having=[], ordered=True)


def render_exceptions(p):
    return grouped(p, p['frm']), bool(p['keys'])


def dml_exceptions(r, ctx):
    """Stamps and restores (a stamped row makes its unit dirty: scanned until VACUUM and a merge write its exceptions again), inserts, deletes, VACUUM and merges on exf."""
    if not tablemode.TABLE and r.randrange(5) == 0:
        return r.choice(['REINDEX INDEX exr_col', "SELECT sift.col_verify('exr_col', true)"])
    m = r.choice([13, 40, 97, 211, 509])
    ids = f'id % {m} = {r.randrange(m)}'
    e = r.randrange(12)
    if e < 3:
        return f"UPDATE exf SET stamp = timestamptz '2025-09-01' WHERE {ids}"
    if e == 3:
        return f'UPDATE exf SET stamp = NULL WHERE {ids}'
    if e == 4:
        return f'UPDATE exf SET val = val * 3 + 1 WHERE {ids}'
    if e == 5:
        return f'DELETE FROM exf WHERE {ids}'
    if e in (6, 7):
        n = r.choice([5, 40, 300])
        lo = ctx['next_ex']; ctx['next_ex'] += n
        return f'INSERT INTO exf SELECT * FROM fz.gen_exf({lo}, {lo + n - 1}, {r.randrange(1, 9)})'
    if e == 8:
        return r.choice(['VACUUM exf', 'VACUUM (FREEZE) exf'])
    if tablemode.TABLE:
        return r.choice(["SELECT sift.table_maintain('exf')", "SELECT sift.table_maintain('exf')", 'VACUUM FULL exf', bulk_insert(r, ctx, 'exf'), bulk_insert(r, ctx, 'exf')])
    return r.choice(["SELECT sift.col_promote('exf_col')", "SELECT sift.col_merge('exf_col', true)", "SELECT sift.col_merge('exf_col', true)"])


# ---------- group shapes: the events of the keys a CTE picks from a table with a column index (IN-subqueries over links and assignments, COALESCE of a NULL member), grouped by the label a small table maps their metrics to ----------
FK_MEMBER = ['', '', "AND m.kind = 'k1'", 'AND m.name IS NOT NULL', "AND m.kind <> 'k0'"]
FK_AGGS = ['count(*)', 'count(DISTINCT {id})', 'count(DISTINCT {pid})', 'count({score})', 'round(avg({score})::numeric, 6)', 'round(sum({score})::numeric, 6)', 'max({score})']
FK_SCORE = "CASE WHEN jsonb_typeof(r.value) = 'number' THEN (r.value #>> '{}')::float8 END"


def gen_flatkeys(r, ctx):
    spans = [(lo, lo + r.choice([0, 5, 30, 90])) for lo in (r.randrange(380), r.randrange(380))]
    return dict(form=r.choice(['periods', 'periods', 'total', 'rows']), groups=sorted({r.randrange(7) for _ in range(r.choice([1, 1, 2]))}), sub=r.choice([9, 9, 9, 8, 7]),
                amax=r.randrange(7), mat=r.choice(['MATERIALIZED ', '', 'NOT MATERIALIZED ']), member=r.choice(FK_MEMBER), spans=spans, outer_map=r.random() < 0.4,
                aggs=['count(*)'] + r.sample(FK_AGGS[1:], r.randrange(1, 4)))


def render_flatkeys(p):
    picked = (f"picked AS {p['mat']}(SELECT DISTINCT m.id FROM fk_members m WHERE m.id IN (SELECT coalesce(a.dim_b_id, l.dim_b_id) FROM fk_assign a LEFT JOIN fk_links l ON l.grp = a.grp "
              f"WHERE a.sub = {p['sub']} AND a.grp <= {p['amax']} AND coalesce(a.dim_b_id, l.dim_b_id) IS NOT NULL) "
              f"AND m.id IN (SELECT dim_b_id FROM fk_links WHERE grp IN ({', '.join(map(str, p['groups']))})) {p['member']})")
    fill = lambda **k: ', '.join(f'{a.format(**k)} AS a{i}' for i, a in enumerate(p['aggs']))
    if p['form'] == 'rows':
        return (f"WITH {picked} SELECT e.kind AS k0, {fill(id='e.id', pid='e.parent_id', score='e.val')} FROM events e JOIN picked ON picked.id = e.parent_id "
                f"WHERE e.ts >= date '2025-01-01' + {p['spans'][0][0]} AND e.ts <= date '2025-01-01' + {p['spans'][0][1]} GROUP BY 1 ORDER BY 1"), True
    # the label is mapped inside the lateral sub-select, or after it
    if p['outer_map']:
        lat = f"INNER JOIN LATERAL (SELECT r.key, {FK_SCORE} AS score FROM jsonb_each(e.metrics) r) s ON true LEFT JOIN fk_map l ON l.src = e.grp AND l.key = s.key"
        cat = 'coalesce(l.label, s.key)'
    else:
        lat = f"INNER JOIN LATERAL (SELECT coalesce(l.label, r.key) AS category, {FK_SCORE} AS score FROM jsonb_each(e.metrics) r LEFT JOIN fk_map l ON l.src = e.grp AND l.key = r.key) s ON true"
        cat = 's.category'
    day = lambda n: f"date '2025-01-01' + {n}"
    if p['form'] == 'total':
        lo, hi = p['spans'][0]
        return (f"WITH {picked} SELECT {cat} AS k0, {fill(id='e.id', pid='e.parent_id', score='s.score')} FROM events e JOIN picked ON picked.id = e.parent_id "
                f"{lat} WHERE e.ts >= {day(lo)} AND e.ts <= {day(hi)} GROUP BY 1 ORDER BY 1"), True
    periods = ', '.join(f"('p{i}', daterange({day(a)}, {day(b)}, '[]'))" for i, (a, b) in enumerate(p['spans']))
    return (f"WITH {picked}, periods AS (SELECT * FROM (VALUES {periods}) p(period, range)), "
            f"parsed AS (SELECT e.id, e.parent_id, p.period, {cat} AS category, s.score FROM events e JOIN periods p ON e.ts <@ p.range JOIN picked ON picked.id = e.parent_id "
            f"{lat}) "
            f"SELECT d.period AS k0, x.category AS k1, {fill(id='x.id', pid='x.parent_id', score='x.score')} FROM periods d LEFT JOIN parsed x ON x.period = d.period "
            f"GROUP BY 1, 2 ORDER BY 1, 2"), True


def dml_flatkeys(r, ctx):
    """Links, links_a, mappings and dim_b change between runs: a group grows, shrinks, loses its dim_b, gets a NULL member; dim_b are updated, deleted and restored; the index is merged."""
    g, m = r.randrange(7), r.choice([3, 7, 13, 50])
    k = r.randrange(m)
    return r.choice([
        f'DELETE FROM fk_links WHERE grp = {g} AND dim_b_id % {m} = {k}', f'INSERT INTO fk_links SELECT {g}, i FROM generate_series({k}, {k + r.choice([0, 4, 40, 300])}) i', f'INSERT INTO fk_links VALUES ({g}, NULL)',
        f'UPDATE fk_links SET dim_b_id = (dim_b_id + {r.randrange(1, 9)}) % 600 WHERE grp = {g}', f'DELETE FROM fk_links WHERE grp = {g}',
        f'UPDATE fk_assign SET sub = 9 - sub + 8 WHERE grp = {g}', f'DELETE FROM fk_assign WHERE grp = {g} AND sub = {r.choice([7, 8, 9])}', f'INSERT INTO fk_assign VALUES ({g}, {r.choice(["NULL", k])}, {r.choice([7, 8, 9])})',
        f'UPDATE fk_map SET label = NULL WHERE src % {m} = {k}', f"UPDATE fk_map SET label = 'lab{r.randrange(5)}' WHERE src % {m} = {k}", f'DELETE FROM fk_map WHERE src % {m} = {k}',
        f"INSERT INTO fk_map SELECT s, 'm{r.randrange(1, 4)}', 'new' FROM generate_series(0, 11) s WHERE s % {m} = {k}",
        f"UPDATE fk_members SET kind = 'k{r.randrange(4)}' WHERE id % {m} = {k}", f'UPDATE fk_members SET name = NULL WHERE id % {m} = {k}', f'DELETE FROM fk_members WHERE id % {m} = {k}',
        f"INSERT INTO fk_members SELECT i, 'k1', 'r' || i FROM generate_series(0, 599) i WHERE i % {m} = {k} ON CONFLICT DO NOTHING",
        'VACUUM fk_members', 'VACUUM fk_links, fk_assign, fk_map', "SELECT sift.col_merge('fk_members_col', true)", "SELECT sift.col_promote('fk_members_col')"])


# ---------- key lists: the rows of the entities a CTE of ids lists, joined and again in an IN sub-select of the same column (scopes of 0, 1, 5, 60, 200 and all entities, a duplicate and a NULL one) ----------
KL_KEYS = ['e.dim_a_id', "date_trunc('month', e.day)::date", 'e.unit_id', 'e.ccy']
KL_AGGS = ['count(*)', 'count(DISTINCT e.id)', 'count(e.price)', 'round(avg(e.price)::numeric, 6)', 'round(sum(e.price)::numeric, 6)', 'max(e.price)', 'min(e.day)']


def gen_keylist(r, ctx):
    return dict(form=r.choice(['twice', 'twice', 'once', 'periods', 'total']), groups=sorted({r.randrange(7) for _ in range(r.choice([1, 1, 2]))}), mat=r.choice(['MATERIALIZED ', '', 'NOT MATERIALIZED ']),
                dim_c=r.choice(['', '', "'o1', 'o2', 'o3'", "'o4'", "'o9', 'none'"]), units=r.random() < 0.5,
                latest=r.random() < 0.5, lo=r.randrange(380), width=r.choice([0, 20, 90, 400]), keys=sorted(r.sample(range(4), r.choice([0, 1, 2, 3]))),
                aggs=['count(*)'] + r.sample(KL_AGGS[1:], r.randrange(1, 4)), cmp=r.choice(['IN', 'IN', '= ANY']))


def render_keylist(p):
    scope = f"s AS {p['mat']}(SELECT dim_a_id AS id FROM kl_links WHERE grp IN ({', '.join(map(str, p['groups']))}))"
    day = lambda n: f"date '2025-01-01' + {n}"
    where = [f"e.day >= {day(p['lo'])} AND e.day <= {day(p['lo'] + p['width'])}"]
    if p['latest']:
        where.append('e.latest')
    sub = '= ANY (SELECT id FROM s)' if p['cmp'] == '= ANY' else 'IN (SELECT id FROM s GROUP BY id)'
    if p['form'] != 'once':
        where.append(f'e.dim_a_id {sub}')
    label_c = f"JOIN kl_origins o ON o.id = e.foreign_id AND o.slug IN ({p['dim_c']}) " if p['dim_c'] else ''
    units = 'LEFT JOIN kl_units u ON u.id = e.unit_id ' if p['units'] else ''
    if p['units']:
        where.append('u.flag IS NOT TRUE')
    cond = ' AND '.join(where)
    fill = lambda x: ', '.join(f'{a} AS a{i}' for i, a in enumerate(x))
    if p['form'] == 'periods':
        periods = f"('p0', daterange({day(p['lo'])}, {day(p['lo'] + p['width'])}, '[]')), ('p1', daterange({day(p['lo'] + 400)}, {day(p['lo'] + 440)}, '[]'))"
        return (f"WITH {scope}, periods AS (SELECT * FROM (VALUES {periods}) q(period, range)), "
                f"parsed AS (SELECT e.id, e.dim_a_id, e.price, e.day, q.period FROM kl_ev e JOIN periods q ON e.day <@ q.range JOIN s ON s.id = e.dim_a_id WHERE e.dim_a_id {sub}) "
                f"SELECT d.period AS k0, count(x.id) AS a0, count(DISTINCT x.dim_a_id) AS a1, count(x.price) AS a2, round(sum(x.price)::numeric, 6) AS a3 FROM periods d LEFT JOIN parsed x ON x.period = d.period "
                f"GROUP BY 1 ORDER BY 1"), True
    keys = [KL_KEYS[k] for k in p['keys']] if p['form'] != 'total' else []
    head = ''.join(f'{k} AS k{i}, ' for i, k in enumerate(keys))
    tail = f" GROUP BY {', '.join(str(i + 1) for i in range(len(keys)))} ORDER BY {', '.join(str(i + 1) for i in range(len(keys)))}" if keys else ''
    return (f"WITH {scope} SELECT {head}{fill(p['aggs'])} FROM kl_ev e JOIN s ON s.id = e.dim_a_id {label_c}{units} WHERE {cond}{tail}"), True


def dml_keylist(r, ctx):
    """The scopes and the rows change between runs: a scope grows, shrinks, loses its dim_a, gets a duplicate or a NULL one; rows move, vanish, lose their price or entity; the index is merged."""
    g, m = r.randrange(7), r.choice([3, 7, 13, 50])
    k = r.randrange(m)
    return r.choice([
        f'DELETE FROM kl_links WHERE grp = {g} AND dim_a_id % {m} = {k}', f'INSERT INTO kl_links SELECT {g}, i FROM generate_series({k}, {k + r.choice([0, 4, 40, 200])}) i',
        f'INSERT INTO kl_links VALUES ({g}, NULL), ({g}, {k}), ({g}, {k})', f'DELETE FROM kl_links WHERE grp = {g}',
        f'UPDATE kl_ev SET day = day + {r.randrange(1, 90)} WHERE id % {m} = {k}', f'DELETE FROM kl_ev WHERE id % {m} = {k}', f'UPDATE kl_ev SET price = NULL WHERE id % {m} = {k}',
        f'UPDATE kl_ev SET dim_a_id = NULL WHERE id % {m} = {k}', f'UPDATE kl_ev SET latest = NOT latest WHERE id % {m} = {k}',
        f"INSERT INTO kl_ev SELECT 100000 + i, i % 300, 1 + i % 60, 1 + i % 8, date '2025-01-01' + i % 400, 10 + i % 90, true, 'usd' FROM generate_series({k}, {k + 300}) i WHERE i % {m} = 0 ON CONFLICT DO NOTHING",
        f'UPDATE kl_units SET flag = NOT flag WHERE id % {m} = {k}', 'VACUUM kl_ev', 'VACUUM kl_links', "SELECT sift.col_merge('kl_ev_col', true)", "SELECT sift.col_promote('kl_ev_col')"])


# ---------- shared CTE copies: two aggregates over a CTE of the rows of the keys a small CTE picks (read through a column index), joined to a heavy CTE that the statement reads 1 to 3 times (inlined: one copy each) ----------
CC_KEYS = ['cf.id = 17', 'cf.id <= 3', 'cf.id <= 40', 'cf.id = 0', 'cf.id % 97 = 5', 'true']


def gen_ctecopies(r, ctx):
    return dict(copies=r.choice([1, 2, 2, 3, 3]), keys=r.choice(CC_KEYS), mat=r.choice(['', '', 'MATERIALIZED ', 'NOT MATERIALIZED ']), cut=r.randrange(2, 16), agg=r.choice(['sum({v})', 'count({v})', 'max({v})', 'sum({v}) FILTER (WHERE d.v % 2 = 0)']),
                heavy=r.choice(['count(*) AS n, sum(w) AS sw', 'count(DISTINCT w) AS n, max(w) AS sw']), op=r.choice(['>=', '>', '<>']))


def render_ctecopies(p):
    val = 'x.value::float8'
    agg = p['agg'].format(v=val)
    arm = lambda cond: (f"SELECT d.grp, {agg} AS s FROM r d CROSS JOIN LATERAL jsonb_each_text(d.doc) x WHERE d.v {cond} GROUP BY d.grp")
    joins = ''.join(f" JOIN heavy h{i} ON h{i}.grp = a{1 + i % 2}.grp AND h{i}.n {p['op']} h{i - 1}.n" if i > 1 else " JOIN heavy h1 ON h1.grp = a1.grp" for i in range(1, p['copies'] + 1))
    cols = ', '.join(f'h{i}.n AS n{i}' for i in range(1, p['copies'] + 1))
    return (f"WITH keys AS (SELECT cf.grp FROM cc_fat cf WHERE {p['keys']}), r AS {p['mat']}(SELECT cd.* FROM cc_docs cd JOIN keys USING (grp)), "
            f"heavy AS (SELECT grp, {p['heavy']} FROM cc_fat GROUP BY grp, w % 97), a1 AS ({arm('< ' + str(p['cut']))}), a2 AS ({arm('>= ' + str(p['cut']))}) "
            f"SELECT a1.grp AS k0, a1.s AS s1, a2.s AS s2, {cols} FROM a1 JOIN a2 USING (grp){joins} ORDER BY {', '.join(str(i) for i in range(1, 4 + p['copies']))}"), True


def dml_ctecopies(r, ctx):
    """Documents become NULL, empty or lose a member, rows move between groups, vanish or appear; the heavy table changes; the index is merged."""
    m = r.choice([3, 7, 11, 19])
    k = r.randrange(m)
    return r.choice([
        f'UPDATE cc_docs SET doc = NULL WHERE id % {m} = {k}', f"UPDATE cc_docs SET doc = '{{}}' WHERE id % {m} = {k}", f"UPDATE cc_docs SET doc = doc || '{{\"a\": null}}' WHERE id % {m} = {k} AND doc IS NOT NULL",
        f'UPDATE cc_docs SET grp = (grp + {r.randrange(1, 5)}) % 2000 WHERE id % {m} = {k}', f'DELETE FROM cc_docs WHERE id % {m} = {k}',
        f"INSERT INTO cc_docs SELECT 100000 + i, i % 2000, i % 17, jsonb_build_object('a', i % 4, 'b', i % 5) FROM generate_series({k}, {k + 300}) i WHERE i % {m} = 0 ON CONFLICT DO NOTHING",
        f'UPDATE cc_fat SET w = w + 1 WHERE id % {m} = {k}', f'DELETE FROM cc_fat WHERE id % {m} = {k}', 'VACUUM cc_docs', 'VACUUM cc_fat', "SELECT sift.col_merge('cc_docs_col', true)", "SELECT sift.col_promote('cc_docs_col')"])


# ---------- global order (order_array_columns): a single-key ORDER BY .. LIMIT over the sorted arrays of the segments and the write buffer; only the key is selected, the rows of a tie may come in any order ----------
GO_COLS = ['id', 'a', 'b', 'c', 'd', 't', 'z']


def go_const(r, col):
    """A constant inside (and a little beyond) the domain of the column in fz.gen_gof, as a literal of its type."""
    if col == 'id':
        return str(r.randrange(0, 3000))
    if col == 'a':
        return str(r.randrange(-110, 110))
    if col == 'b':
        return str(r.randrange(-5, 710))
    if col == 'c':
        return str(r.randrange(-1000000000, 1100000000, 100000))
    if col == 'd':
        return f"DATE '2024-01-01' + {r.randrange(-5, 310)}"
    ts = 'TIMESTAMP' if col == 't' else 'TIMESTAMPTZ'
    return f"{ts} '2025-01-01{'' if col == 't' else '+00'}' + {r.randrange(-100, 60100)} * interval '1 second'"


GG_COLS = ['b', 't']


def gen_gorder_group(r, ctx):
    """A group of gog (the arrays of b and t are grouped by a) pinned by an equality, with a range on the key or the page after one, and a second key: the entries of the group only."""
    col = r.choice(GG_COLS)
    cond = f"g.a = {r.choice([go_const(r, 'a'), go_const(r, 'a'), str(r.randrange(-5, 5)), 'NULL'])}"
    kind = r.random()
    if kind < 0.35:
        cond += f" AND g.{col} {r.choice(['<', '<=', '>', '>='])} {go_const(r, col)}"
    elif kind < 0.5:
        cond += f" AND g.{col} BETWEEN {go_const(r, col)} AND {go_const(r, col)}"
    elif kind < 0.6:
        cond += f" AND g.id {r.choice(['<', '>'])} {go_const(r, 'id')}"
    return dict(col=col, dir=r.choice(['', ' DESC']), nulls=r.choice(['', ' NULLS FIRST', ' NULLS LAST']), limit=r.choice([1, 5, 10, 50, 300]), cond=cond, tbl='gog',
                second=r.random() < 0.3, dir2=r.choice(['', ' DESC']), agg=r.choice(['', '', 'min', 'max']), where=[], keys=[], aggs=[], having=[], ordered=True)


def gen_gorder(r, ctx):
    if r.random() < 0.3:
        return gen_gorder_group(r, ctx)
    col = r.choice(GO_COLS)
    cond = ''
    keyset = ''
    if r.random() < 0.3:  # a second key (id, which is unique, so the order is total), with the page after a key as a row comparison or its expansion
        keyset = r.choice(['', '', 'rc', 'rc', 'or'])
    if keyset == 'rc':
        cond = f"(g.{col}, g.id) {r.choice(['>', '>=', '<', '<='])} ({go_const(r, col)}, {go_const(r, 'id')})"
    elif keyset == 'or':
        op = r.choice(['>', '<'])
        cond = f"g.{col} {op} {go_const(r, col)} OR (g.{col} = {go_const(r, col)} AND g.id {op} {go_const(r, 'id')})"
    elif r.random() < 0.5:  # a condition on the key bounds the part of the array that is read
        def cmp():
            return f"g.{col} {r.choice(['<', '<=', '>', '>=', '=', '<>'])} {go_const(r, col)}"
        cond = r.choice([cmp, lambda: f"{cmp()} AND {cmp()}", lambda: f"({cmp()} OR {cmp()})", lambda: f"g.{col} BETWEEN {go_const(r, col)} AND {go_const(r, col)}"])()
    return dict(col=col, dir=r.choice(['', ' DESC']), nulls=r.choice(['', ' NULLS FIRST', ' NULLS LAST']), limit=r.choice([1, 5, 10, 50, 300, 1500]),
                cond=cond, second=keyset != '' or (col != 'id' and r.random() < 0.15), dir2=r.choice(['', ' DESC']), agg=r.choice(['', '', 'min', 'max']),
                where=[], keys=[], aggs=[], having=[], ordered=True)


def render_gorder(p):
    where = f" WHERE {p['cond']}" if p['cond'] else ''
    tbl = p.get('tbl', 'gof')
    if p['agg']:  # min(k) / max(k): PostgreSQL plans the first row, in the order of k, of the rows where k is not NULL
        return f"SELECT {p['agg']}(g.{p['col']}) FROM {tbl} g{where}", True
    if p['second'] and p['col'] != 'id':
        return f"SELECT g.{p['col']}, g.id FROM {tbl} g{where} ORDER BY g.{p['col']}{p['dir']}{p['nulls']}, g.id{p['dir2']} LIMIT {p['limit']}", True
    return f"SELECT g.{p['col']} FROM {tbl} g{where} ORDER BY g.{p['col']}{p['dir']}{p['nulls']} LIMIT {p['limit']}", True


def dml_gorder(r, ctx):
    """The same change to gof and to gog (the arrays grouped by a column)."""
    st = dml_gorder_one(r, ctx)
    return st + '; ' + st.replace('fz.gen_gof', '@gen@').replace('gof', 'gog').replace('@gen@', 'fz.gen_gof')


def dml_gorder_one(r, ctx):
    """Updates of an indexed key, deletes, inserts, VACUUM, promotion and merges on gof: the arrays of old segments keep dead rows, the write buffer holds the new ones."""
    m = r.choice([13, 40, 97, 211, 509])
    ids = f'id % {m} = {r.randrange(m)}'
    e = r.randrange(12)
    if e < 3:
        c = r.choice(['a', 'b', 'c', 'd', 't', 'z'])
        return f"UPDATE gof SET {c} = NULL WHERE {ids}" if r.random() < 0.3 else f"UPDATE gof SET b = coalesce(b, 0) + 1, c = coalesce(c, 0) + 1 WHERE {ids}"
    if e == 3:
        return f'DELETE FROM gof WHERE {ids}'
    if e in (4, 5, 6):
        n = r.choice([5, 40, 300])
        lo = ctx['next_go']; ctx['next_go'] += n
        return f'INSERT INTO gof SELECT * FROM fz.gen_gof({lo}, {lo + n - 1}, {r.randrange(1, 9)})'
    if e == 7:
        return r.choice(['VACUUM gof', 'VACUUM (FREEZE) gof'])
    return r.choice(["SELECT sift.col_promote('gof_idx')", "SELECT sift.col_merge('gof_idx', true)", "SELECT sift.col_merge('gof_idx', false)"])


# ---------- calendar keys of a timestamptz in the session's zone: the steps of the zone's clock, cut where its offset changes ----------
ZK_ZONES = ['UTC', 'America/New_York', 'Europe/London', 'Asia/Kolkata', 'Asia/Kathmandu', 'Australia/Lord_Howe', 'Pacific/Chatham', 'America/Sao_Paulo', 'Europe/Amsterdam', 'UTC+3', '+05:45', 'PST8PDT']
ZK_KEYS = ["date_trunc('{u}', {e}.z)" for u in ('second', 'minute', 'hour', 'day', 'week', 'month', 'quarter', 'year', 'decade', 'century', 'millennium')] + \
    ["extract({f} from {e}.z)" for f in ('hour', 'minute', 'day', 'dow', 'isodow', 'doy', 'week', 'month', 'year', 'isoyear', 'century')] + \
    ['{e}.z::date', "date_trunc('day', {e}.z)::date", "extract(dow from date_trunc('week', {e}.z))", "date_trunc('hour', {e}.z)::time", "date_trunc('day', {e}.z, 'Etc/UTC')",
     "date_trunc('day', {e}.z, 'UTC')", "date_trunc('hour', {e}.z AT TIME ZONE 'GMT')", "({e}.z AT TIME ZONE 'UTC')::date", "date_trunc('day', date_trunc('day', {e}.z) AT TIME ZONE 'UTC')"]


def gen_zonekey(r, ctx):
    ks = [(k.replace('{u}', r.choice(['second', 'minute', 'hour', 'day', 'week', 'month', 'quarter', 'year', 'decade', 'century', 'millennium']))
           .replace('{f}', r.choice(['hour', 'minute', 'day', 'dow', 'isodow', 'doy', 'week', 'month', 'year', 'isoyear', 'century']))).replace('{e}', 'e')
          for k in r.sample(ZK_KEYS, r.choice([1, 1, 2]))]
    where = [r.choice(['e.org = %d' % r.randrange(5), 'e.amount < %d' % r.randrange(10, 100), "e.z >= timestamptz '2024-03-10+00'", 'e.z IS NOT NULL', "e.z < timestamptz '2100-01-01+00'"])
             for _ in range(r.choice([0, 0, 1, 2]))]
    return dict(table='zkf', keys=ks, aggs=['count(*)', 'sum(e.amount)'] + r.sample(['min(e.z)', 'max(e.amount)', 'count(e.z)', 'count(DISTINCT e.org)'], r.choice([0, 1, 2])),
                where=where, having=[], ordered=True, tz=r.choice(ZK_ZONES))


# ---------- key tables: the aggregates of a segment's table (column format minor 22) WHERE the key (grp_id: 300 values) is tested, against the normal plan ----------
KT_WHERE = ['e.grp_id = {k}'] * 3 + ['e.grp_id IN ({k}, {k2}, {k3})', 'e.grp_id BETWEEN {k} AND {k2}', 'e.grp_id < {k}', 'e.grp_id >= {k}', 'e.grp_id <> {k}', 'e.grp_id IS NULL', 'e.grp_id IS NOT NULL',
                                   'e.grp_id = {k} AND e.val < {v}', "e.grp_id = {k} AND e.ts >= timestamp '2024-06-01'", '(e.grp_id = {k} OR e.grp_id = {k2})', 'e.grp_id = 99999', 'e.val < {v}']
KT_KEYS = ['e.grp_id', 'e.grp_id % 7', '(e.grp_id < 100)']
KT_AGGS = ['count(*)', 'count(e.grp_id)', 'count(e.amt)', 'sum(e.amt)', 'avg(e.amt)', 'min(e.amt)', 'max(e.amt)', 'min(e.ts)', 'max(e.ts)', 'sum(e.val)', 'min(e.val)', 'max(e.val)', 'count(DISTINCT e.grp_id)',
           'sum(e.amt) FILTER (WHERE e.val < {v})']


def gen_keytable(r, ctx):
    fill = lambda t: t.format(k=r.randrange(-40, 260), k2=r.randrange(-40, 260), k3=r.randrange(-40, 260), v=r.randrange(50, 1000))
    ks = list(dict.fromkeys(r.sample(KT_KEYS, r.choice([0, 0, 1, 1, 2]))))
    ag = list(dict.fromkeys(fill(a) for a in ['count(*)'] + r.sample(KT_AGGS, r.randrange(1, 5))))
    return dict(table='ktf', keys=ks, aggs=ag, where=[fill(r.choice(KT_WHERE)) for _ in range(r.choice([1, 1, 1, 2]))], having=[], ordered=True)


def dml_keytable(r, ctx):
    """Updates and deletes (a segment with a dead row is read from its units), inserts into the write buffer, VACUUM, promotion, merge and the option on ktf."""
    m = r.choice([13, 40, 97, 211, 509])
    ids = f'id % {m} = {r.randrange(m)}'
    e = r.randrange(12)
    if e < 3:
        return r.choice([f'UPDATE ktf SET grp_id = grp_id + {r.randrange(1, 40)} WHERE {ids}', f'UPDATE ktf SET amt = coalesce(amt, 0) + 1, val = val + 1 WHERE {ids}', f'UPDATE ktf SET grp_id = NULL WHERE {ids}'])
    if e == 3:
        return f'DELETE FROM ktf WHERE {ids}'
    if e in (4, 5, 6):
        n = r.choice([5, 40, 300])
        lo = ctx['next_kt']; ctx['next_kt'] += n
        return f'INSERT INTO ktf SELECT * FROM fz.gen_ktf({lo}, {lo + n - 1}, {r.randrange(1, 9)})'
    if e == 7:
        return r.choice(['VACUUM ktf', 'VACUUM (FREEZE) ktf'])
    if e == 8:
        return f"ALTER INDEX ktf_idx SET (key_table = {r.choice(['on', 'off'])})"
    return r.choice(["SELECT sift.col_promote('ktf_idx')", "SELECT sift.col_merge('ktf_idx', true)"])


# ---------- row counts of a text dictionary (column format minor 24): count(*) WHERE s is tested by a prefix, an equality or a range, against the normal plan ----------
def dc_value(r, ctx):
    n = ctx['rows'] // 8 + 1
    return r.choice(['k' + __import__('hashlib').md5(str(r.randrange(n)).encode()).hexdigest()[:r.randrange(1, 14)], 'k', '', 'a', 'a_b', 'a_b1', '100', '100%', 'k0', 'kf', 'zz'])


def gen_dictcount(r, ctx):
    v = dc_value(r, ctx)
    q = lambda s: "'" + s.replace("'", "''") + "'"
    op = r.choice(['=', '<', '<=', '>', '>=', 'LIKE', 'LIKE', 'LIKE'])
    esc = v.replace('\\', '\\\\').replace('_', '\\_').replace('%', '\\%')
    test = f'e.s COLLATE "C" LIKE {q(esc + "%")}' if op == 'LIKE' else f'e.s COLLATE "C" {op} {q(v)}'
    where = [test] + ([r.choice([f'e.n < {r.randrange(50, 950)}', f'e.id > {r.randrange(ctx["rows"])}', 'e.n IS NOT NULL'])] if r.randrange(5) == 0 else [])
    ag = ['count(*)'] + (r.sample(['sum(e.n)', 'max(e.id)', 'count(e.s)'], r.randrange(0, 3)) if r.randrange(4) == 0 else [])
    return dict(table='dcf', keys=[], aggs=ag, where=where, having=[], ordered=False)


def dml_dictcount(r, ctx):
    """Updates, deletes, inserts into the write buffer, VACUUM, promotion, merge, REINDEX and the option on dcf."""
    m = r.choice([13, 40, 97, 211, 509, 997])
    ids = f'id % {m} = {r.randrange(m)}'
    e = r.randrange(13)
    if e < 3:
        return r.choice([f"UPDATE dcf SET s = 'k' || md5(id::text) WHERE {ids}", f'UPDATE dcf SET s = NULL WHERE {ids}', f"UPDATE dcf SET s = 'a_b' || (id % 50) WHERE {ids}", f'UPDATE dcf SET n = n + 1 WHERE {ids}'])
    if e in (3, 4, 5):
        return f'DELETE FROM dcf WHERE {ids}'
    if e in (6, 7):
        n = r.choice([5, 40, 300, 1500])
        lo = ctx['next_dc']; ctx['next_dc'] += n
        return f'INSERT INTO dcf SELECT * FROM fz.gen_dcf({lo}, {lo + n - 1}, {r.randrange(1, 9)})'
    if e == 8:
        return r.choice(['VACUUM dcf', 'VACUUM (FREEZE) dcf'])
    if e == 9:
        return r.choice([f"ALTER INDEX dcf_idx SET (count_columns = {r.choice(['s', 's', ''])!r})", 'REINDEX INDEX dcf_idx'])
    return r.choice(["SELECT sift.col_promote('dcf_idx')", "SELECT sift.col_merge('dcf_idx', true)"])
