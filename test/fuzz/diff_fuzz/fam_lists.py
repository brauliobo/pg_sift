# Families of entity and list reports: pages, transaction mixes, trailing sort keys, two levels, same-table lookups and points.
# Each family is a gen(r, ctx) and a render(parts).

from .columns import EVENTS, aggs, keys, preds
from .fam_basic import grouped

# ---------- a report over one entity or a few: a CTE of their ids as a literal, a lookup table filtered by a column, an unnest of the metrics, grouping sets ----------
def gen_oneentity(r, ctx):
    ids = sorted({r.choice([0, 7, 11, 599, 600, 601]) if r.random() < 0.3 else r.randrange(0, 600) for _ in range(r.choice([1, 1, 1, 2, 4]))})
    return dict(ids=ids, dimw=r.choice([None, 20, 60]), sets=r.choice(['(r.key)', '(r.key), ()', '(r.key, e.kind), (e.kind)']), distinct=r.random() < 0.6, keys=[], aggs=[], having=[],
                ordered=True, where=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 3)))


def render_oneentity(p):
    lst = ', '.join(str(i) for i in p['ids'])
    dim = ' JOIN dims2 d ON d.id = e.grp' + ('' if p['dimw'] is None else f" AND d.w > {p['dimw']}")
    w = ''.join(f' AND {c}' for c in p['where'])
    agg = 'count(DISTINCT e.id)' if p['distinct'] else 'count(*)'
    kind = ', e.kind' if 'e.kind' in p['sets'] else ''
    sql = (f"WITH ids AS MATERIALIZED (SELECT DISTINCT parent_id AS id FROM events WHERE parent_id IN ({lst})), "
           f"ev AS (SELECT e.* FROM events e JOIN ids ON ids.id = e.parent_id{dim} WHERE true{w}) "
           f"SELECT r.key{kind}, {agg} AS n, avg((r.value #>> '{{}}')::float8) AS v, sum((r.value #>> '{{}}')::float8) AS t FROM ev e, jsonb_each(e.metrics) r "
           f"WHERE r.key <> 'm3' GROUP BY GROUPING SETS ({p['sets']}) ORDER BY 1, 2")
    return sql, True


# ---------- a page of a list endpoint: a literal list of parents, a since date, a cursor, lateral lookups into JSON objects, newest first, LIMIT/OFFSET ----------
def gen_listpage(r, ctx):
    n = r.choice([0, 1, 1, 5, 30, 200, 600])
    ids = [r.randrange(0, 620) for _ in range(n)]  # parents past the 600 there are match nothing
    if ids and r.random() < 0.2:
        ids[r.randrange(len(ids))] = None  # a NULL in the list matches nothing and must not drop the others
    return dict(ids=ids, since=r.choice([None, '2025-03-01', '2025-09-15', '2026-01-20', '2027-01-01']), cursor=r.choice([None, None, 50, ctx['rows'] // 2]),
                live=r.random() < 0.4, vers=r.random() < 0.7, dims2=r.random() < 0.7, limit=r.choice([1, 10, 50, 500]), offset=r.choice([0, 0, 0, 7, 120]),
                order=r.choice(['e.ts DESC NULLS LAST', 'e.ts DESC', 'e.ts', 'e.amount DESC NULLS LAST', 'e.id DESC']), keys=[], aggs=[], having=[], ordered=True)


def render_listpage(p):
    lst = ', '.join('NULL' if i is None else str(i) for i in p['ids'])
    cond = [f'e.parent_id IN ({lst})' if p['ids'] else "e.parent_id = ANY ('{}'::int[])"]
    if p['since']:
        cond.append(f"e.ts >= DATE '{p['since']}'")
    if p['cursor'] is not None:
        cond.append(f"e.id > {p['cursor']}")
    if p['live']:
        cond.append('e.flag IS NOT TRUE')
    cols, lat = ['e.id', 'e.parent_id', 'e.kind', 'e.ts', 'e.amount'], ''
    if p['dims2']:
        lat += ' LEFT JOIN LATERAL (SELECT dims2.* FROM dims2 WHERE dims2.id = e.grp) x ON true'
        cols.append("(CASE WHEN x IS NULL THEN NULL ELSE jsonb_build_object('id', x.id, 'tag', x.tag, 'w', x.w) END) AS inner_x")
    if p['vers']:
        lat += (' LEFT JOIN LATERAL (SELECT vers.* FROM vers WHERE vers.a = e.a AND vers.b = e.b ORDER BY vers.ts DESC NULLS LAST, vers.id LIMIT 1) v ON true')
        cols.append("(CASE WHEN v IS NULL THEN NULL ELSE jsonb_build_object('id', v.id, 'ts', v.ts, 'label', v.label) END) AS inner_v")
    sql = (f"SELECT {', '.join(cols)} FROM events e{lat} WHERE {' AND '.join(cond)} "
           f"ORDER BY {p['order']}, e.id LIMIT {p['limit']} OFFSET {p['offset']}")
    return sql, True



# ---------- trailing sort keys (TOPN_TIES): ORDER BY leaves, then columns the index does not hold, the Sort above the scan puts the ties in order ----------
TR_LEAD = ['k', 'd', 't']
TR_TAIL = ['w', 'x', 's', 'w % 7', 'w * 3 - k', 'length(s)', 'x * w']


def gen_txnmix(r, ctx):
    n = r.choice([3, 20, 120, 400])
    lo = ctx['next_ev']; ctx['next_ev'] += 3 * n * 8
    ops = [r.choice('iiocsxud') for _ in range(r.randrange(2, 8))]
    return dict(lo=lo, n=n, salt=r.randrange(1, 9), ops=','.join(ops), keys=[], aggs=[], having=[], ordered=True)


def render_txnmix(p):
    return f"SELECT step, res FROM fz.txn_mix({p['lo']}, {p['n']}, {p['salt']}, '{p['ops']}') ORDER BY step", True


def dml_txn(r, ctx):
    """Transactions that commit or roll back whole or in part: the rows of the write buffer a backend buffers before it writes them, a savepoint and an error among them."""
    t, gen, counter = 'events', 'gen_events', 'next_ev'
    n = r.choice([2, 30, 150])
    lo = ctx[counter]; ctx[counter] += 4 * n
    ins = lambda a, b: f"INSERT INTO {t} SELECT * FROM fz.{gen}({a}, {b}, {r.randrange(1, 9)})"
    cp = lambda a, b: f"SELECT fz.bulk_insert('{t}', '{gen}', {a}, {b}, {r.randrange(1, 9)})"
    return r.choice([
        f"BEGIN; {ins(lo, lo + n - 1)}; {ins(lo + n, lo + 2 * n - 1)}; COMMIT",
        f"BEGIN; {ins(lo, lo)}; {ins(lo + 1, lo + 1)}; {ins(lo + 2, lo + n)}; ROLLBACK",
        f"BEGIN; {cp(lo, lo + n - 1)}; SELECT count(*) FROM {t}; {ins(lo + n, lo + 2 * n - 1)}; COMMIT",
        f"BEGIN; {ins(lo, lo + n - 1)}; SAVEPOINT s; {ins(lo + n, lo + 2 * n - 1)}; ROLLBACK TO s; {ins(lo + 2 * n, lo + 3 * n - 1)}; COMMIT",
        f"BEGIN; {ins(lo, lo + n - 1)}; SAVEPOINT s; {cp(lo + n, lo + 2 * n - 1)}; RELEASE s; SAVEPOINT s2; {ins(lo + 2 * n, lo + 3 * n - 1)}; ROLLBACK TO s2; COMMIT",
        f"BEGIN; {ins(lo, lo + n - 1)}; {ins(lo, lo + 1)}; COMMIT",
        f"{ins(lo, lo + n - 1)} ON CONFLICT (id) DO NOTHING; {ins(lo + n // 2, lo + n + n // 2)} ON CONFLICT (id) DO NOTHING",
        f"BEGIN; {ins(lo, lo + n - 1)}; DELETE FROM {t} WHERE id >= {lo} AND id < {lo + n // 3}; UPDATE {t} SET amount = amount + 1 WHERE id >= {lo + n // 2}; COMMIT",
        f"BEGIN; {ins(lo, lo + n - 1)}; SELECT sift.col_promote('events_col'); {ins(lo + n, lo + 2 * n - 1)}; COMMIT",
    ])


def gen_trail(r, ctx):
    order = [f"g.{c} {r.choice(['', 'DESC'])} {r.choice(['', 'NULLS FIRST', 'NULLS LAST'])}".strip() for c in r.sample(TR_LEAD, r.choice([1, 1, 2]))]
    for c in r.sample(TR_TAIL, r.choice([1, 1, 2])):
        e = c.replace('w', 'g.w').replace('x', 'g.x').replace('s', 'g.s').replace('k', 'g.k').replace('length(g.s)', 'length(g.s)')
        order.append(f"{e} {r.choice(['', 'DESC'])} {r.choice(['', 'NULLS FIRST', 'NULLS LAST'])}".strip())
    return dict(order=order, limit=r.choice([1, 5, 10, 33, 50, 300, 1500]), offset=r.choice([0, 0, 3, 40, 500]),
                where=r.sample(['g.k > 20', 'g.d < DATE \'2024-03-01\'', 'g.t IS NOT NULL', 'g.id % 3 = 1', 'g.k BETWEEN 50 AND 250', 'g.w IS NOT NULL'], r.choice([0, 0, 1, 2])),
                keys=[], aggs=[], having=[], ordered=True)


def render_trail(p):
    w = ' WHERE ' + ' AND '.join(p['where']) if p['where'] else ''
    return f"SELECT g.id, g.k, g.w, g.s FROM trf g{w} ORDER BY {', '.join(p['order'] + ['g.id'])} LIMIT {p['limit']} OFFSET {p['offset']}", True


def dml_trail(r, ctx):
    """Updates that make a tie group the statistics do not know, deletes, inserts, VACUUM, promotion and merges on trf."""
    m = r.choice([7, 13, 40, 97, 211])
    ids = f'id % {m} = {r.randrange(m)}'
    e = r.randrange(12)
    if e < 4:
        return r.choice([f'UPDATE trf SET k = {r.randrange(-3, 3)} WHERE {ids}', f'UPDATE trf SET t = timestamp \'2025-01-01\' WHERE {ids}', f'UPDATE trf SET w = w + 1, d = NULL WHERE {ids}'])
    if e == 4:
        return f'DELETE FROM trf WHERE {ids}'
    if e in (5, 6, 7):
        n = r.choice([5, 40, 300])
        lo = ctx['next_tr']; ctx['next_tr'] += n
        return f'INSERT INTO trf SELECT * FROM fz.gen_trf({lo}, {lo + n - 1}, {r.randrange(1, 9)})'
    if e == 8:
        return r.choice(['VACUUM trf', 'VACUUM (FREEZE) trf'])
    return r.choice(["SELECT sift.col_promote('trf_idx')", "SELECT sift.col_merge('trf_idx', true)", "SELECT sift.col_merge('trf_idx', false)"])


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
                wrapped=r.random() < 0.4,
                # ... joined with LEFT JOIN LATERAL: a part-less row is a NULL tuple, and the category the lookup defaults reads NULL there
                leftmo=r.random() < 0.4)


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
                    f"{'LEFT' if p['leftmo'] else 'INNER'} JOIN LATERAL (SELECT rm.derived_cat AS category, o.score FROM LATERAL (SELECT cat, dir_a, score FROM jsonb_to_recordset(s.doc -> 'parts') q(cat text, dir_a text, score float8) "
                    f"WHERE q.score IS NOT NULL) o LEFT JOIN LATERAL (SELECT derived_cat FROM {p['lookup']} m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true) mo ON true "
                    + ("" if p['leftmo'] or (p['unmatched'] and not cat) else "WHERE mo.category IS NOT NULL ") + g)
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
    elif form == 5:  # a list past 64 keys, which the directory answers up to a sixteenth of the rows (misses and repeats among the keys)
        cond = f'e.{col} IN ({", ".join(val() for _ in range(r.randrange(65, max(66, min(400, n // 16)))))})'
    elif form == 1:
        cond = f'e.{col} IN ({", ".join(val() for _ in range(r.randrange(2, 9)))})'
    elif form == 2:
        cond = f'e.{col} = ANY (ARRAY[{", ".join(val() for _ in range(r.randrange(1, 5)))}])'
    elif form == 3:
        cond = f'e.{col} = {val()} AND e.amount {r.choice(["<", ">"])} {r.randrange(100, 900)}'
    else:
        cond = f'(e.{col} = {val()} OR e.{col} = {val()})'
    # a LIMIT stops the index scan between the chunks of keys it looks up: the number of rows is the same whichever rows come
    return dict(where=[cond], limit=r.choice([0, 0, 1, 5, 50, 300]))


def render_point(p):
    if p['limit']:
        return f"SELECT count(*) FROM (SELECT e.id FROM ev_part e WHERE {p['where'][0]} LIMIT {p['limit']}) l", True
    return f"SELECT e.id, e.kind, e.amount FROM ev_part e WHERE {p['where'][0]} ORDER BY e.id", True


def gen_pointagg(r, ctx):
    """The same lists as gen_point, aggregated by Custom Scan (SiftAgg), which asks the directory too (and narrows each unit to the rows it lists): as a list, as the keys of a semi lookup
    join to a few rows of the table, and with another conjunct; the reference is the normal plan, across the DML of the run."""
    p = gen_point(r, ctx)
    n = ctx['rows'] // 2
    if r.randrange(3) == 0:
        a = r.randrange(1, n)
        col = r.choice(['parent_id', 'id'])
        p['where'] = [f"e.{col} IN (SELECT m.{col} FROM ev_part m WHERE m.id BETWEEN {a} AND {a + r.randrange(0, 30)})"]
    return p


def render_pointagg(p):
    return f"SELECT e.kind, count(*), sum(e.amount) FROM ev_part e WHERE {p['where'][0]} GROUP BY 1 ORDER BY 1", True
