# The scope family: the facts cut by the scope key of item_scope (events.parent_id -> tenant_id) in every form the planner recognises
# (join, IN, EXISTS, GROUP BY the key, a keys table, a table of keys of a user in the IN, EXISTS and join forms) and in the near misses it refuses; item_grants is a scope relation of
# soft deleted pairs (the pairs are its rows not soft deleted): the statements that state the predicate and those that do not; item_links, item_members are a scope of two paths (the
# direct grants of a tenant on an item and the grants on its groups): its pairs written as the UNION of the two, in IN, EXISTS and join forms, through views, in the near misses (a path alone, other keys in the
# arms, a path without its predicate, UNION ALL under a join) that keep the join; the key may be a parameter of a prepared statement (run under the generic and the custom plan: a scalar, an
# array of 1 to 70 keys (the runtime choice between the scope read and the join as written is made for the keys of each execution), a NULL, an array of NULLs, an empty one). The scope is declared on the sift_col index of events (diff_fuzz_data.sql).
# The btree of the pairs is unique in an even seed and not unique in an odd one, where the joins run as written (a pair may be several rows) and IN and EXISTS count a fact once.

from .columns import EVENTS, aggs, keys, preds
from .fam_basic import grouped

PARAM_FORMS = ['pjoin', 'pin', 'pany', 'pnull', 'pmany', 'pjoinmany', 'upin', 'upjoin', 'upany']
UNION_FORMS = ['uin', 'uin', 'uinall', 'uexists', 'uview', 'ubarrier', 'ujoin', 'ujoin', 'ujoinm', 'ujoinall', 'ujoinkeys', 'ubranch', 'ukeys', 'ubare']
SCOPE_FORMS = ['join', 'join', 'in', 'in', 'exists', 'group', 'keytable', 'ktin', 'ktexists', 'ktjoin', 'ktmany', 'ktloose', 'anyarray', 'joinin', 'left', 'notin', 'other', 'filtered', 'softjoin', 'softin', 'softexists', 'softbare', 'softinbare'] + UNION_FORMS + PARAM_FORMS


def gen_scope(r, ctx):
    # safe predicates: a plan driven by the scope relation never evaluates one that raises on the rows of the keys it leaves out, the join's plan does
    ks = r.sample(range(6), r.choice([1, 1, 2, 3]))
    form = r.choice(SCOPE_FORMS)
    return dict(table='events', form=form, tenants=ks, keys=keys(r, 'e', EVENTS, r.choice([0, 1, 1, 2])),
                aggs=aggs(r, 'e', EVENTS, r.randrange(1, 5)), where=preds(r, 'e', ctx['rows'], EVENTS, r.randrange(0, 3), safe=True), having=[], ordered=True,
                cut=r.randrange(100, 600), many=r.sample(range(80), r.choice([1, 3, 10, 30, 70])), prep=form in PARAM_FORMS, pid=r.randrange(10 ** 9), user=r.randrange(4), nullarg=r.choice(['NULL', "'{NULL}'", "'{}'", "'{%d,NULL}'" % ks[0]]))


def render_scope_parts(p):
    """(from, extra where, extra keys) of the form"""
    ts = ', '.join(map(str, p['tenants']))
    one, u = p['tenants'][0], p['user']
    mine = f'SELECT tenant_id FROM user_keys WHERE user_id = {u}'
    j = 'events e JOIN item_scope s ON s.item_id = e.parent_id'
    g = 'events e JOIN item_grants s ON s.item_id = e.parent_id'
    live = 'deleted_at IS NULL'
    kin, other = f'IN ({ts})', (one + 1) % 6
    direct = lambda k: f'SELECT l.item_id FROM item_links l WHERE l.{live} AND l.tenant_id {k}'
    groups = lambda k: f'SELECT m.item_id FROM item_links l JOIN item_members m ON m.group_id = l.group_id WHERE l.{live} AND m.{live} AND l.tenant_id {k}'
    uj = 'events e JOIN item_pairs v ON v.member = e.parent_id'
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
        'pmany': ('events e', ['e.parent_id IN (SELECT item_id FROM item_scope WHERE tenant_id = ANY ($1))'], []),
        'pjoinmany': (j, ['s.tenant_id = ANY ($1)'], []),
        'anyarray': ('events e', [f'e.parent_id = ANY (ARRAY(SELECT item_id FROM item_scope WHERE tenant_id = {one}))'], []),
        'joinin': (j, [f's.tenant_id IN ({ts})'], []),
        'left': ('events e LEFT JOIN item_scope s ON s.item_id = e.parent_id AND s.tenant_id = %d' % one, [], ['s.tenant_id IS NULL']),
        'notin': ('events e', [f'e.parent_id NOT IN (SELECT item_id FROM item_scope WHERE tenant_id = {one})'], []),
        'other': (j, [f's.tenant_id = {one}', "e.kind <> 'k0'"], []),
        'filtered': (j, [f's.tenant_id = {one}', f's.item_id < {p["cut"]}'], []),
        'softjoin': (g, [f's.tenant_id = {one}', f's.{live}'], []),
        'softbare': (g, [f's.tenant_id = {one}'], []),
        'softin': ('events e', [f'e.parent_id IN (SELECT item_id FROM item_grants WHERE tenant_id IN ({ts}) AND {live})'], []),
        'softinbare': ('events e', [f'e.parent_id IN (SELECT item_id FROM item_grants WHERE tenant_id IN ({ts}))'], []),
        'softexists': ('events e', [f'EXISTS (SELECT 1 FROM item_grants s WHERE s.item_id = e.parent_id AND s.tenant_id = {one} AND s.{live})'], []),
        'uin': ('events e', [f'e.parent_id IN ({direct(kin)} UNION {groups(kin)})'], []),
        'uinall': ('events e', [f'e.parent_id IN ({direct(kin)} UNION ALL {groups(kin)})'], []),
        'uexists': ('events e', [f'EXISTS (SELECT 1 FROM item_pairs v WHERE v.member = e.parent_id AND v.tenant_id = {one})'], []),
        'uview': ('events e', [f'e.parent_id IN (SELECT member FROM item_pairs WHERE tenant_id {kin})'], []),
        'ubarrier': ('events e', [f'e.parent_id IN (SELECT member FROM item_pairs_barrier WHERE tenant_id {kin})'], []),
        'ujoin': (uj, [f'v.tenant_id = {one}'], []),
        'ujoinm': (f'events e JOIN ({direct(kin)} UNION {groups(kin)}) v ON v.item_id = e.parent_id', [], []),
        'ujoinall': ('events e JOIN item_pairs_all v ON v.member = e.parent_id', [f'v.tenant_id = {one}'], []),
        'ujoinkeys': (uj, [f'v.tenant_id {kin}'], []),
        'ubranch': ('events e', [f'e.parent_id IN ({(direct, groups)[one % 2](f"= {one}")})'], []),
        'ukeys': ('events e', [f'e.parent_id IN ({direct(f"= {one}")} UNION {groups(f"= {other}")})'], []),
        'ubare': ('events e', [f'e.parent_id IN (SELECT l.item_id FROM item_links l WHERE l.tenant_id = {one} UNION {groups(f"= {one}")})'], []),
        'upin': ('events e', ['e.parent_id IN (SELECT member FROM item_pairs WHERE tenant_id = $1)'], []),
        'upjoin': (uj, ['v.tenant_id = $1'], []),
        'upany': ('events e', ['e.parent_id IN (SELECT member FROM item_pairs WHERE tenant_id = ANY ($1))'], []),
    }[p['form']]


def param_of(p):
    """(type, argument) of the parameter of a prepared form"""
    return {'pjoin': ('int', str(p['tenants'][0])), 'pin': ('int', str(p['tenants'][0])), 'pany': ('int[]', "'{%s}'" % ','.join(map(str, p['tenants']))),
            'pnull': ('int[]', p['nullarg']), 'pmany': ('int[]', "'{%s}'" % ','.join(map(str, p['many']))), 'pjoinmany': ('int[]', "'{%s}'" % ','.join(map(str, p['many']))),
            'upin': ('int', str(p['tenants'][0])), 'upjoin': ('int', str(p['tenants'][0])), 'upany': ('int[]', "'{%s}'" % ','.join(map(str, p['tenants'])))}[p['form']]


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
        f'INSERT INTO item_scope SELECT item_id, tenant_id FROM item_scope WHERE item_id % {m} = {k} ON CONFLICT DO NOTHING',   # a duplicate row of a pair: none when the btree is unique
        f'DELETE FROM item_scope WHERE ctid IN (SELECT ctid FROM (SELECT ctid, row_number() OVER (PARTITION BY item_id, tenant_id ORDER BY ctid) n FROM item_scope '
        f'WHERE item_id % {m} = {k}) x WHERE n = 2)',   # one of two rows of a pair
        f'UPDATE item_scope s SET tenant_id = (tenant_id + 1) % 6 WHERE item_id % {m} = {k} AND NOT EXISTS (SELECT 1 FROM item_scope o WHERE o.item_id = s.item_id AND o.tenant_id = (s.tenant_id + 1) % 6)',
        f'UPDATE item_grants SET deleted_at = now() WHERE item_id % {m} = {k} AND tenant_id = {t}', f'UPDATE item_grants SET deleted_at = NULL WHERE item_id % {m * 3} = {k}',
        f'INSERT INTO item_grants SELECT i, {t}, CASE WHEN i % 2 = 0 THEN now() END FROM generate_series({k}, 599, {m}) i ON CONFLICT DO NOTHING',
        f'DELETE FROM item_grants WHERE item_id % {m * 5} = {k}',
        f'UPDATE item_links SET deleted_at = now() WHERE item_id % {m} = {k} AND tenant_id = {t}', f'UPDATE item_links SET deleted_at = NULL WHERE id % {m * 3} = {k}',
        f"INSERT INTO item_links SELECT 20000 + i * 10 + {t}, {t}, i, NULL, NULL, 'live' FROM generate_series({k}, 599, {m}) i ON CONFLICT DO NOTHING",
        f"INSERT INTO item_links SELECT 30000 + g * 10 + {t}, {t}, NULL, g, NULL, 'live' FROM generate_series(0, 19) g WHERE g % {m % 7 + 2} = {k % 3} ON CONFLICT DO NOTHING",
        f'DELETE FROM item_links WHERE group_id = {k % 20} AND tenant_id = {t}', f'DELETE FROM item_links WHERE item_id % {m * 5} = {k}',
        f'UPDATE item_members SET deleted_at = now() WHERE item_id % {m} = {k}', f'UPDATE item_members SET deleted_at = NULL WHERE item_id % {m * 3} = {k}',
        f"INSERT INTO item_members SELECT {k % 20}, i, NULL, 'live' FROM generate_series({k}, 599, {m}) i ON CONFLICT DO NOTHING", f'DELETE FROM item_members WHERE item_id % {m * 5} = {k}',
        'VACUUM item_scope', 'VACUUM item_grants', 'VACUUM item_links', 'VACUUM item_members', "SELECT sift.col_promote('events_col')", "SELECT sift.col_merge('events_col', true)", 'REINDEX INDEX events_col'])
