# The scope family: the facts cut by the scope key of item_scope (events.parent_id -> tenant_id) in every form the planner recognises
# (join, IN, EXISTS, GROUP BY the key, a keys table, a table of keys of a user in the IN, EXISTS and join forms) and in the near misses it refuses; the key may be a parameter of a prepared statement (run under the generic and the custom plan: a scalar, an
# array, a NULL, an array of NULLs, an empty one). The scope is declared on the sift_col index of events (diff_fuzz_data.sql).

from .columns import EVENTS, aggs, keys, preds
from .fam_basic import grouped

PARAM_FORMS = ['pjoin', 'pin', 'pany', 'pnull']
SCOPE_FORMS = ['join', 'join', 'in', 'in', 'exists', 'group', 'keytable', 'ktin', 'ktexists', 'ktjoin', 'ktmany', 'ktloose', 'anyarray', 'joinin', 'left', 'notin', 'other', 'filtered'] + PARAM_FORMS


def gen_scope(r, ctx):
    # safe predicates: a plan driven by the scope relation never evaluates one that raises on the rows of the keys it leaves out, the join's plan does
    ks = r.sample(range(6), r.choice([1, 1, 2, 3]))
    form = r.choice(SCOPE_FORMS)
    return dict(table='events', form=form, tenants=ks, keys=keys(r, 'e', EVENTS, r.choice([0, 1, 1, 2])),
                aggs=aggs(r, 'e', EVENTS, r.randrange(1, 5)), where=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 3), safe=True), having=[], ordered=True,
                cut=r.randrange(100, 600), prep=form in PARAM_FORMS, pid=r.randrange(10 ** 9), user=r.randrange(4), nullarg=r.choice(['NULL', "'{NULL}'", "'{}'", "'{%d,NULL}'" % ks[0]]))


def render_scope_parts(p):
    """(from, extra where, extra keys) of the form"""
    ts = ', '.join(map(str, p['tenants']))
    one, u = p['tenants'][0], p['user']
    mine = f'SELECT tenant_id FROM user_keys WHERE user_id = {u}'
    j = 'events e JOIN item_scope s ON s.item_id = e.parent_id'
    return {
        'join': (j, [f's.tenant_id = {one}'], []),
        'in': ('events e', [f'e.parent_id IN (SELECT item_id FROM item_scope WHERE tenant_id IN ({ts}))'], []),
        'exists': ('events e', [f'EXISTS (SELECT 1 FROM item_scope s WHERE s.item_id = e.parent_id AND s.tenant_id = {one})'], []),
        'group': (j, [f's.tenant_id IN ({ts})'], ['s.tenant_id']),
        'keytable': (j + ' JOIN (VALUES %s) k(t) ON k.t = s.tenant_id' % ', '.join(f'({t})' for t in p['tenants']), [], []),
        'ktin': ('events e', [f'e.parent_id IN (SELECT item_id FROM item_scope WHERE tenant_id IN ({mine}))'], []),
        'ktexists': ('events e', [f'EXISTS (SELECT 1 FROM item_scope s WHERE s.item_id = e.parent_id AND s.tenant_id IN ({mine}))'], []),
        'ktjoin': (j + ' JOIN home_keys k ON k.tenant_id = s.tenant_id', [f'k.user_id = {u}'], []),
        'ktmany': (j + ' JOIN user_keys k ON k.tenant_id = s.tenant_id', [f'k.user_id = {u}'], []),
        'ktloose': (j + ' JOIN loose_keys k ON k.tenant_id = s.tenant_id', [], []),
        'pjoin': (j, ['s.tenant_id = $1'], []),
        'pin': ('events e', ['e.parent_id IN (SELECT item_id FROM item_scope WHERE tenant_id = $1)'], []),
        'pany': ('events e', ['e.parent_id IN (SELECT item_id FROM item_scope WHERE tenant_id = ANY ($1))'], []),
        'pnull': ('events e', ['e.parent_id IN (SELECT item_id FROM item_scope WHERE tenant_id = ANY ($1))'], []),
        'anyarray': ('events e', [f'e.parent_id = ANY (ARRAY(SELECT item_id FROM item_scope WHERE tenant_id = {one}))'], []),
        'joinin': (j, [f's.tenant_id IN ({ts})'], []),
        'left': ('events e LEFT JOIN item_scope s ON s.item_id = e.parent_id AND s.tenant_id = %d' % one, [], ['s.tenant_id IS NULL']),
        'notin': ('events e', [f'e.parent_id NOT IN (SELECT item_id FROM item_scope WHERE tenant_id = {one})'], []),
        'other': (j, [f's.tenant_id = {one}', "e.kind <> 'k0'"], []),
        'filtered': (j, [f's.tenant_id = {one}', f's.item_id < {p["cut"]}'], []),
    }[p['form']]


def param_of(p):
    """(type, argument) of the parameter of a prepared form"""
    return {'pjoin': ('int', str(p['tenants'][0])), 'pin': ('int', str(p['tenants'][0])), 'pany': ('int[]', "'{%s}'" % ','.join(map(str, p['tenants']))),
            'pnull': ('int[]', p['nullarg'])}[p['form']]


def render_scope(p):
    frm, wh, ks = render_scope_parts(p)
    q = dict(p, where=wh + p['where'], keys=ks + p['keys'])
    sql = grouped(q, frm)
    if p['prep']:
        ty, arg = param_of(p)
        sql = f"PREPARE sp{p['pid']}({ty}) AS {sql}\n@@\nEXECUTE sp{p['pid']}({arg})"
    return sql, bool(q['keys'])


def dml_scope(r, ctx):
    """Pairs ended, inserted, moved and dropped (the ids the reader patches), VACUUM of the pairs, and a promotion, merge or rebuild of the index the units are in."""
    m = r.choice([3, 7, 13, 50])
    k, t, u = r.randrange(m), r.randrange(6), r.randrange(6)
    return r.choice([
        f'DELETE FROM user_keys WHERE tenant_id = {t} AND user_id = {k % 4}', f'INSERT INTO user_keys VALUES ({k % 4}, {u}) ON CONFLICT DO NOTHING',
        f'UPDATE home_keys SET tenant_id = {t} WHERE user_id = {k % 4}', f'UPDATE home_keys SET tenant_id = NULL WHERE user_id = {k % 4}',
        f'INSERT INTO loose_keys VALUES ({t})', f'DELETE FROM loose_keys WHERE tenant_id = {u}',
        f'DELETE FROM item_scope WHERE item_id % {m} = {k} AND tenant_id = {t}', f'DELETE FROM item_scope WHERE item_id % {m * 5} = {k}',
        f'INSERT INTO item_scope SELECT i, {t} FROM generate_series({k}, 599, {m}) i ON CONFLICT DO NOTHING',
        f'INSERT INTO item_scope SELECT i, {u} FROM item_scope WHERE tenant_id = {t} AND item_id % {m} = {k} ON CONFLICT DO NOTHING',
        f'UPDATE item_scope s SET tenant_id = (tenant_id + 1) % 6 WHERE item_id % {m} = {k} AND NOT EXISTS (SELECT 1 FROM item_scope o WHERE o.item_id = s.item_id AND o.tenant_id = (s.tenant_id + 1) % 6)',
        'VACUUM item_scope', "SELECT sift.col_promote('events_col')", "SELECT sift.col_merge('events_col', true)", 'REINDEX INDEX events_col'])
