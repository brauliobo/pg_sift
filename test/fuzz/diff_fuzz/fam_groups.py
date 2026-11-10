# Family of the groups of an entity: facts split by the groups of their entity (a lateral sub-select over a link table, the link table joined, or an
# inlined sub-select of the entities with their groups). A gen(r, ctx), a render(parts) and its DML.

GS_SETS = ['tenant', 'tenant', 'exists', 'list', 'none']
GS_AGGS = ['count(*)', 'count(DISTINCT e.id)', 'count(DISTINCT e.dim_a_id)', 'sum(e.amount)', 'max(e.amount)', 'count(e.amount)', 'min(e.day)']
GS_KEYS = ['e.kind', "date_trunc('month', e.day)::date", 'e.dim_a_id % 7']
GS_FORMS = ['lateral_exists', 'lateral_exists', 'lateral_in', 'join_links', 'join_links', 'dim_a']


def gen_groupsplit(r, ctx):
    return dict(form=r.choice(GS_FORMS), join=r.choice(['LEFT JOIN', 'LEFT JOIN', 'JOIN']), live=r.random() < 0.8, below=r.choice([None, None, None, r.randrange(2, 12)]),
                set=r.choice(GS_SETS), tenant=r.randrange(1, 5), list=sorted(r.sample(range(300), r.choice([1, 5, 40]))),
                where=r.choice(['', '', "e.day >= date '2025-%02d-01'" % r.randrange(1, 12), 'e.kind = %d' % r.randrange(5), 'e.amount > %d' % r.randrange(90)]),
                keys=r.sample(GS_KEYS, r.choice([0, 0, 1])), aggs=r.sample(GS_AGGS, r.randrange(1, 4)))


def render_groupsplit(p):
    t = p['tenant']
    sets = dict(tenant=f'e.dim_a_id IN (SELECT dim_a_id FROM gs_tenant WHERE tenant_id = {t} AND deleted_at IS NULL)',
                exists=f'EXISTS (SELECT 1 FROM gs_tenant s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id = {t})',
                list=f"e.dim_a_id IN ({', '.join(map(str, p['list']))})", none='')
    live = ' AND x.deleted_at IS NULL' if p['live'] else ''
    below = f" AND g0.id < {p['below']}" if p['below'] else ''
    where = ' AND '.join(w for w in (sets[p['set']], p['where']) if w)

    def groups_of(entity):
        cond = (f'g0.id IN (SELECT x.group_id FROM gs_links x WHERE x.dim_a_id = {entity}{live})' if p['form'] == 'lateral_in'
                else f'EXISTS (SELECT 1 FROM gs_links x WHERE x.group_id = g0.id AND x.dim_a_id = {entity}{live})')
        return f'(SELECT g0.* FROM gs_groups g0 WHERE {cond}{below})'

    if p['form'] == 'join_links':
        frm, gk = f"gs_ev e {p['join']} gs_links x ON x.dim_a_id = e.dim_a_id{live}", 'x.group_id'
    elif p['form'] == 'dim_a':
        ents = f"SELECT d.dim_a_id AS id, g.id AS group_id FROM (SELECT DISTINCT dim_a_id FROM gs_ev) d LEFT JOIN LATERAL {groups_of('d.dim_a_id')} g ON true"
        frm, gk = f'gs_ev e JOIN ({ents}) ents ON ents.id = e.dim_a_id', 'ents.group_id'
    else:
        frm, gk = f"gs_ev e {p['join']} LATERAL {groups_of('e.dim_a_id')} g ON true", 'g.id'
    cols = [gk] + p['keys']
    sel = ', '.join(f'{k} AS k{i}' for i, k in enumerate(cols)) + ', ' + ', '.join(f'{a} AS a{i}' for i, a in enumerate(p['aggs']))
    pos = ', '.join(str(i + 1) for i in range(len(cols)))
    return f"SELECT {sel} FROM {frm}{' WHERE ' + where if where else ''} GROUP BY {pos} ORDER BY {pos}", True


def dml_groupsplit(r, ctx):
    """Links soft-deleted, restored, doubled and dropped, a set of ids in or out of a tenant, facts inserted, updated and deleted, VACUUM and a merge of the facts' index."""
    g, m = r.randrange(1, 12), r.choice([3, 7, 13, 50])
    k, t = r.randrange(m), r.randrange(1, 5)
    return r.choice([
        f"UPDATE gs_links SET deleted_at = timestamptz '2026-02-01' WHERE group_id = {g} AND dim_a_id % {m} = {k}", f'UPDATE gs_links SET deleted_at = NULL WHERE group_id = {g}',
        f'INSERT INTO gs_links SELECT {g}, i, NULL FROM generate_series({k}, 299, {m}) i', f'INSERT INTO gs_links VALUES ({g}, NULL, NULL)', f'DELETE FROM gs_links WHERE group_id = {g}',
        f'DELETE FROM gs_links WHERE dim_a_id % {m} = {k}', f'INSERT INTO gs_links SELECT {g}, dim_a_id, NULL FROM gs_links WHERE dim_a_id % {m} = {k}',
        f'DELETE FROM gs_tenant WHERE tenant_id = {t} AND dim_a_id % {m} = {k}', f'INSERT INTO gs_tenant SELECT {t}, i, NULL FROM generate_series({k}, 299, {m}) i',
        f'UPDATE gs_ev SET amount = amount + 1 WHERE dim_a_id % {m} = {k}', f'DELETE FROM gs_ev WHERE id % {m * 3} = {k}',
        f"INSERT INTO gs_ev SELECT (SELECT max(id) FROM gs_ev) + i, (i * 7 + {k}) % 310, date '2025-01-01' + i % 300, i % 5, i % 97 FROM generate_series(1, {r.choice([5, 40, 300])}) i",
        'VACUUM gs_links, gs_tenant', 'VACUUM gs_ev', "SELECT sift.col_merge('gs_ev_col', true)", "SELECT sift.col_promote('gs_ev_col')"])
