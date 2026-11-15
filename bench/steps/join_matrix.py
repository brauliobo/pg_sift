#!/usr/bin/env python3
"""The join matrix: join scenarios of one fact and its dimensions on native PostgreSQL (b-trees), pg_sift and pg_sift with the pushdown off.

    python3 -I join_matrix.py gen                       # rewrite bench/sql/join_matrix/cells.tsv (pairwise cells + shapes.tsv)
    python3 -I join_matrix.py run --rows 100000,1000000 [--cells REGEX] [--profile P] [--out FILE] [--resume] [--timed N]
    python3 -I join_matrix.py report RESULTS.tsv...     # matrix, losers, planner misses, wrong answers (--md for Markdown)

Needs a server (PGHOST / PGPORT, `scripts/pgtest.sh env`) with pg_sift installed; one database jm_<rows> per size (schema.sql, built once).

cells.tsv (generated; the dimensions of the matrix are the AXES below, the named shapes are in shapes.tsv):
    id       slug of the cell, `<axes>@<profile>` or `<shape>@<profile>`
    kind     pairwise (a row of the covering array: every pair of values of any two axes occurs in some cell) or shape (a named shape)
    shape    `single` for pairwise cells, else the shape's name
    rel key join filter agg group scope    the axis values (`-` for a shape)
    profile  the index situation of the cell (PROFILES); `--profile P` runs every cell under P instead
    sql      the statement; f is the fact, d the dimension of the key (ds small, dl large, cal calendar, fx 1:1 extension, link+dl M:N)

Arms (the settings of every arm: parallel workers 4, jit off, work_mem 256MB, a statement timeout):
    native   sift.enable_agg off, no sift_col index in the database (profile `none`): PostgreSQL with the b-trees of schema.sql
    off      sift.enable_agg off, the cell's profile
    on       sift.enable_agg on, the planner's own choice
    force    when `on` did not take the scan: sift.agg_force on (the scan whatever the cost), to price the plan not chosen

results (TSV, one row per cell, rows, arm; columns RESULT_COLS):
    plan         `sift` when the plan has a Custom Scan (SiftAgg), plus the join and scan node types
    detail       the `Lookup ...` / `Join ...` attributes of the scan nodes; `why` is sift.explain_why's slug for each arm that was not used
    est_cost     the planner's total cost of the chosen plan
    ms           median of the timed executions (EXPLAIN ANALYZE, TIMING OFF) after the first; first_ms the first one (a warm cluster, not an evicted cache)
    hit read     shared blocks hit / read of the first execution; warm_hit of the last
    instr_first  M instructions of the first statement of a new backend (postmaster-attached perf: workers included, backend start-up subtracted)
    instr_steady M instructions of an execution after the first in the same backend (per-backend caches warm)
    rows md5     the result, row count and md5 of the sorted rows as text: the arms of a cell must agree
"""
import argparse, csv, itertools, json, os, random, re, statistics, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)
SQL = os.path.join(HERE, '..', 'sql', 'join_matrix')
CELLS = os.path.join(SQL, 'cells.tsv')
RESULT_COLS = ['cell', 'rows', 'profile', 'arm', 'plan', 'detail', 'why', 'est_cost', 'plan_ms', 'ms', 'first_ms', 'hit', 'read', 'warm_hit',
               'workers', 'instr_first', 'instr_steady', 'nrows', 'md5', 'error']

# ---- the axes -------------------------------------------------------------------------------------------------------------------------
# key: the join key (type, and with it the dimension: ds small, dl large, cal calendar, fx 1:1, link M:N through link to dl)
KEYS = {
    'int':    dict(t='ds',  y='ds d',  on='f.ds_id = d.id',                                fk=['f.ds_id'],                 dk=['d.id'],
                   one='f.ds_id = 7'),
    'bigint': dict(t='dl',  y='dl d',  on='f.dl_id = d.id',                                fk=['f.dl_id'],                 dk=['d.id'],
                   one='f.dl_id = 7'),
    'text':   dict(t='ds',  y='ds d',  on='f.ds_code = d.code',                            fk=['f.ds_code'],               dk=['d.code'],
                   one="f.ds_code = 'S0000007'"),
    'uuid':   dict(t='ds',  y='ds d',  on='f.ds_uid = d.uid',                              fk=['f.ds_uid'],                dk=['d.uid'],
                   one="f.ds_uid = md5('s7')::uuid"),
    'date':   dict(t='cal', y='cal d', on='f.event_date = d.d',                            fk=['f.event_date'],            dk=['d.d'],
                   one="f.event_date = DATE '2024-03-05'"),
    'comp':   dict(t='ds',  y='ds d',  on='f.tenant_id = d.tenant_id AND f.ds_id = d.id',  fk=['f.tenant_id', 'f.ds_id'],  dk=['d.tenant_id', 'd.id'],
                   one='f.ds_id = 7'),
    'expr':   dict(t='ds',  y='ds d',  on='f.ds_id + 1 = d.id',                            fk=['f.ds_id + 1'],             dk=['d.id'],
                   one='f.ds_id = 7'),
    'null':   dict(t='ds',  y='ds d',  on='f.nul_id = d.id',                               fk=['f.nul_id'],                dk=['d.id'],
                   one='f.nul_id = 7'),
    'pk11':   dict(t='fx',  y='fx d',  on='f.id = d.id',                                   fk=['f.id'],                    dk=['d.id'],
                   one='f.id = 12345'),
    'link':   dict(t='dl',  y='(link l JOIN dl d ON d.id = l.dl_id)', on='l.f_id = f.id',  fk=['f.id'],                    dk=['l.f_id'],
                   one='f.id = 12345'),
}
REL_KEYS = {'n1': [k for k in KEYS if k not in ('pk11', 'link')], 'fanout': [k for k in KEYS if k not in ('pk11', 'link')],
            'one_one': ['pk11'], 'mn': ['link']}
JOINS = ['inner', 'left', 'right', 'full', 'semi_in', 'semi_exists', 'anti_exists', 'anti_not_in']
FILTERS = {'none': ([], []), 'f_sel': (['f.pct < 1'], []), 'f_unsel': (['f.pct < 50'], []), 'd_sel': ([], ['d.pct < 3']),
           'd_unsel': ([], ['d.pct < 50']), 'both_sel': (['f.pct < 1'], ['d.pct < 3']), 'both_mixed': (['f.pct < 50'], ['d.pct < 3'])}
# agg: (select list, sides it reads)
AGGS = {'count': ('count(*)', ''), 'cd_fact': ('count(DISTINCT f.qty)', 'f'), 'cd_dim': ('count(DISTINCT d.g_high)', 'd'),
        'sum_fact': ('sum(f.val), avg(f.qty), min(f.event_date), max(f.qty)', 'f'), 'sum_dim': ('sum(d.v), min(d.v), max(d.g_low)', 'd'),
        'filter_dim': ("count(*) FILTER (WHERE d.attr = 'a'), sum(f.val) FILTER (WHERE d.pct < 10)", 'fd'),
        'arr_fact': ("array_agg(DISTINCT f.status ORDER BY f.status), string_agg(DISTINCT f.kind, ',' ORDER BY f.kind)", 'f')}
GROUPS = {'none': ('', ''), 'f_low': ('f.kind', 'f'), 'f_high': ('f.qty', 'f'), 'd_low': ('d.g_low', 'd'), 'd_high': ('d.g_high', 'd'),
          'month': ("date_trunc('month', f.event_date)", 'f')}
SCOPES = {'all': [], 'tenant': ['f.tenant_id = 3'], 'source': ['f.lookup_id = 2'], 'tenant_source': ['f.tenant_id = 3', 'f.lookup_id = 2'], 'key': None}
PROFILES = {
    # none: b-trees only (the native baseline). fact: one sift_col on the fact. both: the fact and every dimension. partial: the fact without its join
    # keys, no index on the dimensions (the filters and measures are covered, the keys are not)
    'none': [],
    'fact': ['CREATE INDEX jm_f ON f USING sift_col (tenant_id, lookup_id, ds_id, dl_id, nul_id, ds_code, ds_uid, parent_id, event_date, status, kind,'
             ' qty, val, pct)',
             'CREATE INDEX jm_fp ON fp USING sift_col (tenant_id, lookup_id, ds_id, dl_id, event_date, status, kind, qty, val, pct)'],
    'partial': ['CREATE INDEX jm_f ON f USING sift_col (tenant_id, lookup_id, event_date, status, kind, qty, val, pct)'],
    'both': None,
}
PROFILES['both'] = PROFILES['fact'] + [
    'CREATE INDEX jm_ds ON ds USING sift_col (id, tenant_id, code, uid, g_low, g_high, v, pct, attr)',
    'CREATE INDEX jm_dl ON dl USING sift_col (id, ds_id, g_low, g_high, v, pct, attr)',
    'CREATE INDEX jm_cal ON cal USING sift_col (d, g_low, g_high, v, pct, attr)',
    'CREATE INDEX jm_fx ON fx USING sift_col (id, g_low, g_high, v, pct, attr)',
    'CREATE INDEX jm_link ON link USING sift_col (f_id, dl_id)',
    'CREATE INDEX jm_key_sets ON key_sets USING sift_col (set_id, dl_id)']
INDEX_AXIS = ['fact', 'both', 'partial', 'none']


def where_sides(c):
    """Which sides the statement can read: both for a join, only the driver for a semi or anti join."""
    if c['join'].startswith(('semi', 'anti')):
        return 'd' if c['rel'] == 'fanout' else 'f'
    return 'fd'


def valid(c):
    if c['key'] not in REL_KEYS[c['rel']]:
        return False
    vis = where_sides(c)
    return all(s in vis for s in AGGS[c['agg']][1] + GROUPS[c['group']][1])


def build_sql(c):
    """The statement of a cell: X is the driver (the fact, or the dimension for a fan-out), Y the other side."""
    k = KEYS[c['key']]
    fan = c['rel'] == 'fanout'
    x, y = ('%s d' % k['t'], 'f') if fan else ('f', k['y'])
    xk, yk = (k['dk'], k['fk']) if fan else (k['fk'], k['dk'])
    fpred, dpred = FILTERS[c['filter']]
    scope = [k['one']] if c['scope'] == 'key' else SCOPES[c['scope']]
    fpred = scope + fpred
    xpred, ypred = (dpred, fpred) if fan else (fpred, dpred)
    j, on = c['join'], k['on']
    tuple_ = lambda ks: ks[0] if len(ks) == 1 else '(%s)' % ', '.join(ks)
    if j in ('inner', 'left', 'right', 'full'):
        kw = {'inner': 'JOIN', 'left': 'LEFT JOIN', 'right': 'RIGHT JOIN', 'full': 'FULL JOIN'}[j]
        where, onc = [], [on]
        for side, preds in (('x', xpred), ('y', ypred)):
            preserved = j == 'inner' or (j == 'left' and side == 'x') or (j == 'right' and side == 'y')
            (where if preserved else onc).extend(preds)
        frm = '%s %s %s ON %s' % (x, kw, y, ' AND '.join(onc))
    else:
        sub = {'semi_in': 'IN', 'anti_not_in': 'NOT IN'}.get(j)
        ycond = ypred
        if sub:
            inner = 'SELECT %s FROM %s%s' % (', '.join(yk), y, ' WHERE ' + ' AND '.join(ycond) if ycond else '')
            where = xpred + ['%s %s (%s)' % (tuple_(xk), sub, inner)]
        else:
            neg = 'NOT ' if j == 'anti_exists' else ''
            where = xpred + ['%sEXISTS (SELECT 1 FROM %s WHERE %s)' % (neg, y, ' AND '.join([on] + ycond))]
        frm = x
    grp = GROUPS[c['group']][0]
    sel = ', '.join(([grp] if grp else []) + [AGGS[c['agg']][0]])
    return 'SELECT %s FROM %s%s%s' % (sel, frm, ' WHERE ' + ' AND '.join(where) if where else '', ' GROUP BY 1' if grp else '')


def pairwise(seed=20261108):
    """A covering array over the axes: greedy picks from a pool of valid random combinations until every feasible pair of values is covered."""
    axes = {'rel': list(REL_KEYS), 'key': list(KEYS), 'join': JOINS, 'filter': list(FILTERS), 'agg': list(AGGS), 'group': list(GROUPS),
            'scope': list(SCOPES), 'index': INDEX_AXIS}
    names = list(axes)
    rng = random.Random(seed)
    pool = []
    while len(pool) < 150000:
        c = {a: rng.choice(v) for a, v in axes.items()}
        if valid(c):
            pool.append(c)
    pairs = lambda c: {(a, c[a], b, c[b]) for a, b in itertools.combinations(names, 2)}
    todo = set().union(*(pairs(c) for c in pool))
    chosen = []
    while todo:
        best = max(rng.sample(pool, 4000), key=lambda c: len(pairs(c) & todo))
        chosen.append(best)
        todo -= pairs(best)
    return chosen


def slug(c):
    return '-'.join([c['rel'], c['key'], c['join'], c['filter'], c['agg'], c['group'], c['scope']]).replace('_', '')


def gen():
    rows = []
    for c in sorted(pairwise(), key=lambda c: (c['rel'], c['key'], c['join'], c['filter'], c['agg'], c['group'], c['scope'])):
        rows.append(['%s@%s' % (slug(c), c['index']), 'pairwise', 'single', c['rel'], c['key'], c['join'], c['filter'], c['agg'], c['group'], c['scope'],
                     c['index'], build_sql(c)])
    with open(os.path.join(SQL, 'shapes.tsv')) as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            name, shape, profiles, stmt = line.rstrip('\n').split('\t')
            for p in profiles.split(','):
                rows.append(['%s@%s' % (name, p), 'shape', shape, '-', '-', '-', '-', '-', '-', '-', p, stmt])
    with open(CELLS, 'w') as f:
        f.write('# generated by bench/steps/join_matrix.py gen: pairwise cells (a covering array of the axes) and the shapes of shapes.tsv\n')
        f.write('\t'.join(['id', 'kind', 'shape', 'rel', 'key', 'join', 'filter', 'agg', 'group', 'scope', 'profile', 'sql']) + '\n')
        f.writelines('\t'.join(r) + '\n' for r in rows)
    print('%d cells: %d pairwise, %d shapes' % (len(rows), sum(r[1] == 'pairwise' for r in rows), sum(r[1] == 'shape' for r in rows)))


def load_cells(only=None):
    with open(CELLS) as f:
        for r in csv.DictReader((l for l in f if not l.startswith('#')), delimiter='\t'):
            if not only or re.search(only, r['id']):
                yield r


# ---- the runner -----------------------------------------------------------------------------------------------------------------------
TIMEOUT_S = {100000: 60, 1000000: 120, 10000000: 400}


def psql(db, sql, *flags, check=True):
    r = subprocess.run(['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', *flags, '-d', db, '-c', sql], capture_output=True, text=True)
    if r.returncode and check:
        sys.exit('join_matrix: %s\n%s' % (sql[:200], r.stderr.strip()))
    return r


def db_exists(db):
    return bool(psql('postgres', "SELECT 1 FROM pg_database WHERE datname = '%s'" % db).stdout.strip())


def ensure_db(n):
    db = 'jm_%d' % n
    if not db_exists(db):
        psql('postgres', 'CREATE DATABASE ' + db)
        r = subprocess.run(['psql', '-X', '-q', '-d', db, '-v', 'n=%d' % n, '-f', os.path.join(SQL, 'schema.sql')], capture_output=True, text=True)
        if r.returncode:
            psql('postgres', 'DROP DATABASE ' + db)
            sys.exit('join_matrix: schema at %d rows\n%s' % (n, r.stderr.strip()))
    return db


def apply_profile(db, name):
    """The index situation: the sift_col indexes of the database are exactly those of the profile (the b-trees stay). The statistics are never
    taken again, so a plan differs between profiles only by what the indexes offer."""
    have = {r.split('\t')[0] for r in psql(db, "SELECT i.indexrelid::regclass || E'\\t' FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid "
                                          "JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_col' AND NOT c.relispartition").stdout.splitlines()}
    want = PROFILES[name]
    key = '\n'.join(want)
    cur = psql(db, "SELECT coalesce(obj_description('public'::regnamespace, 'pg_namespace'), '')").stdout.strip()
    if cur == name and (bool(have) == bool(want)):
        return
    for ix in have:
        psql(db, 'DROP INDEX ' + ix)
    for ddl in want:
        psql(db, ddl)
    psql(db, "COMMENT ON SCHEMA public IS '%s'" % name)


def settings(arm, n):
    s = ("SET max_parallel_workers_per_gather = 4; SET jit = off; SET work_mem = '256MB'; SET statement_timeout = '%ds'; SET sift.enable_agg = %s;"
         " SET sift.agg_force = %s;") % (TIMEOUT_S[n], 'on' if arm in ('on', 'force') else 'off', 'on' if arm == 'force' else 'off')
    return s


def nodes(plan):
    yield plan
    for p in plan.get('Plans', []):
        yield from nodes(p)


ABBR = {'Hash Join': 'HashJoin', 'Nested Loop': 'NestLoop', 'Merge Join': 'MergeJoin', 'Seq Scan': 'Seq', 'Index Scan': 'Idx', 'Index Only Scan': 'IdxOnly',
        'Bitmap Heap Scan': 'Bitmap', 'Custom Scan': 'Custom', 'Hash': None, 'Gather': 'Gather', 'Gather Merge': 'GatherMerge', 'HashAggregate': 'HashAgg',
        'GroupAggregate': 'GroupAgg', 'Aggregate': 'Agg', 'Sort': None, 'Limit': None, 'Append': None, 'Result': None, 'Materialize': None,
        'Memoize': 'Memoize', 'Subquery Scan': None, 'CTE Scan': 'CTE', 'WindowAgg': 'Window', 'Unique': None, 'Parallel Hash': None, 'Bitmap Index Scan': None,
        'Incremental Sort': None, 'ProjectSet': None, 'Function Scan': None, 'Values Scan': None, 'Merge Append': None}


def plan_info(top):
    plan = top['Plan']
    kinds, detail, sift, workers = [], [], False, 0
    for nd in nodes(plan):
        t = nd['Node Type']
        if t == 'Custom Scan':
            prov = nd.get('Custom Plan Provider', '')
            sift = sift or 'Sift' in prov
            kinds.append(prov or 'Custom')
            detail += ['%s=%s' % (k, nd[k]) for k in nd if k.startswith(('Lookup', 'Join', 'Cached', 'Shared Lookup'))]
        elif ABBR.get(t) and not nd.get('Parent Relationship') == 'InitPlan':
            kinds.append(ABBR[t])
        workers = max(workers, nd.get('Workers Launched', 0))
    seen = []
    for k in kinds:
        if k not in seen:
            seen.append(k)
    return ('sift ' if sift else '') + '+'.join(seen), '; '.join(detail), plan.get('Total Cost'), workers


def run_json(db, arm, n, stmt, count):
    """`count` EXPLAIN (ANALYZE, BUFFERS) executions in one backend; the JSON documents, or the error text."""
    script = settings(arm, n) + '\n' + ''.join('\\echo @@\nEXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON, FORMAT JSON) %s;\n' % stmt for _ in range(count))
    r = subprocess.run(['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-d', db], input=script, capture_output=True, text=True)
    docs = [json.loads(p)[0] for p in r.stdout.split('@@\n')[1:] if p.strip().startswith('[')]
    if r.returncode or len(docs) < count:
        return None, (r.stderr.strip().splitlines() or ['no output'])[-1][:200]
    return docs, None


def result_of(db, arm, n, stmt):
    out = psql(db, settings(arm, n) + " SELECT count(*), md5(coalesce(string_agg(t::text, E'\\n' ORDER BY t::text), '')) FROM (%s) t" % stmt, check=False)
    if out.returncode:
        return '', '', (out.stderr.strip().splitlines() or ['error'])[-1][:200]
    last = out.stdout.strip().splitlines()[-1]
    nrows, md5 = last.split('|')
    return nrows, md5, ''


def postmaster(db):
    d = psql(db, 'SHOW data_directory').stdout.strip()
    return open(os.path.join(d, 'postmaster.pid')).readline().strip()


def instr(db, arm, n, stmt, k):
    """User instructions (millions) of everything the postmaster forks while one session connects, sets and runs the statement k times."""
    with tempfile.NamedTemporaryFile('w', suffix='.sql', delete=False) as f:
        f.write(settings(arm, n) + '\n\\o /dev/null\n' + (stmt + ';\n') * k)
    try:
        r = subprocess.run(['bash', os.path.join(HERE, 'perf.sh'), postmaster(db), '--', 'psql', '-X', '-q', '-d', db, '-f', f.name], capture_output=True, text=True)
    finally:
        os.unlink(f.name)
    ins = r.stdout.split()[1] if r.stdout.split() else 'NA'
    return int(ins) / 1e6 if ins.isdigit() else None


S0 = {}


def instructions(db, arm, n, stmt, ms):
    key = (db, arm)
    if key not in S0:
        S0[key] = instr(db, arm, n, 'SELECT 1', 1)
    s0 = S0[key]
    one = instr(db, arm, n, stmt, 1)
    steady = None
    if one is not None and ms < 20000:
        four = instr(db, arm, n, stmt, 4)
        steady = (four - one) / 3 if four is not None else None
    return (None if one is None or s0 is None else one - s0), steady


def why(db, n, stmt):
    r = psql(db, settings('on', n) + " SELECT string_agg(arm || ':' || coalesce(slug, 'used') || CASE WHEN scan_cost IS NULL THEN '' ELSE '(scan ' || round(scan_cost) || ' normal ' || round(normal_cost) || ')' END, ' ') FROM sift.explain_why($jm$%s$jm$) WHERE NOT used" % stmt, check=False)
    return r.stdout.strip().splitlines()[-1] if r.returncode == 0 and r.stdout.strip() else ''


def measure(db, n, cell, arm, timed, perf):
    stmt = cell['sql']
    row = dict.fromkeys(RESULT_COLS, '')
    row.update(cell=cell['id'], rows=n, profile=cell['profile'] if arm != 'native' else 'none', arm=arm)
    docs, err = run_json(db, arm, n, stmt, 1 + timed)
    if err:
        row['error'] = err
        return row
    row['plan'], row['detail'], row['est_cost'], row['workers'] = plan_info(docs[0])
    row['plan_ms'] = docs[0].get('Planning Time')
    exe = [d['Execution Time'] for d in docs]
    row['first_ms'], row['ms'] = exe[0], statistics.median(exe[1:])
    row['hit'], row['read'] = docs[0]['Plan'].get('Shared Hit Blocks'), docs[0]['Plan'].get('Shared Read Blocks')
    row['warm_hit'] = docs[-1]['Plan'].get('Shared Hit Blocks')
    row['nrows'], row['md5'], row['error'] = result_of(db, arm, n, stmt)
    if perf and not row['error']:
        first, steady = instructions(db, arm, n, stmt, row['ms'])
        row['instr_first'], row['instr_steady'] = first, steady
    if arm == 'on' and 'sift' not in row['plan']:
        row['why'] = why(db, n, stmt)
    return row


def fmt(v):
    return ('%.2f' % v) if isinstance(v, float) else ('' if v is None else str(v))


def run(a):
    done = set()
    if a.resume and a.out and os.path.exists(a.out):
        with open(a.out) as f:
            done = {(r['cell'], r['rows'], r['arm']) for r in csv.DictReader(f, delimiter='\t')}
    out = open(a.out, 'a' if a.resume and os.path.exists(a.out) else 'w') if a.out else sys.stdout
    if out.tell() == 0:
        out.write('\t'.join(RESULT_COLS) + '\n')
    cells = list(load_cells(a.cells))
    if a.profile:
        for c in cells:
            c['profile'] = a.profile
            c['id'] = c['id'].rsplit('@', 1)[0] + '@' + a.profile
    for n in map(int, a.rows.split(',')):
        db = ensure_db(n)
        for profile in ['none'] + [p for p in PROFILES if p != 'none']:
            mine = [c for c in cells if c['profile'] == profile]
            if not mine and profile != 'none':
                continue
            apply_profile(db, profile)
            if profile == 'none':
                work = [(c, 'native') for c in cells] + [(c, arm) for c in mine for arm in ('on', 'off')]
            else:
                work = [(c, arm) for c in mine for arm in ('on', 'off')]
            for c, arm in work:
                if (c['id'], str(n), arm) in done:
                    continue
                r = measure(db, n, c, arm, a.timed, not a.no_perf)
                out.write('\t'.join(fmt(r[k]) for k in RESULT_COLS) + '\n')
                out.flush()
                if arm == 'on' and 'sift' not in r['plan'] and not r['error'] and a.force:
                    f = measure(db, n, c, 'force', a.timed, not a.no_perf)
                    out.write('\t'.join(fmt(f[k]) for k in RESULT_COLS) + '\n')
                    out.flush()
    if a.out:
        out.close()


# ---- the report -----------------------------------------------------------------------------------------------------------------------
BAND, FLOOR_MS = 1.10, 1.0
ABS_MS = {100000: 10, 1000000: 100, 10000000: 1000}


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def verdicts(arms, n):
    """The verdicts of one cell (the three arms and the forced scan). The baseline is the faster of native and off: both are PostgreSQL's plan, and the
    pair tells the noise of the host (their plans and instructions are equal when the indexes change nothing)."""
    ms = {k: fnum(v['ms']) for k, v in arms.items()}
    ins = {k: fnum(v['instr_steady']) for k, v in arms.items()}
    on, frc = arms.get('on'), arms.get('force')
    out = []
    if len({(v['nrows'], v['md5']) for v in arms.values() if not v['error']}) > 1:
        out.append('WRONG')
    out += ['error:' + k for k, v in arms.items() if v['error']]
    if on is None or on['error']:
        return out
    base = min(x for x in (ms.get('native'), ms.get('off')) if x is not None)
    used = on['plan'].startswith('sift')
    if used and ms['on'] > base * BAND and ms['on'] - base > FLOOR_MS:
        out.append('scan_slower')
    if used and ins.get('on') and ins.get('off') and ins['on'] > ins['off'] * BAND:
        out.append('scan_more_instr')
    if not used and frc and frc['plan'].startswith('sift') and ms['force'] * BAND < ms['on'] and ms['on'] - ms['force'] > FLOOR_MS:
        out.append('planner_miss')
    if ms['on'] > ABS_MS.get(int(n), 1000):
        out.append('above_abs' if used or base <= ABS_MS.get(int(n), 1000) else 'above_abs_native_too')
    return out


def report(a):
    rows = []
    for p in a.files:
        with open(p) as f:
            rows += list(csv.DictReader(f, delimiter='\t'))
    cells = {}
    for r in rows:
        cells.setdefault((r['cell'], r['rows']), {})[r['arm']] = r
    tally = {}
    cols = ['cell', 'rows', 'native_ms', 'off_ms', 'on_ms', 'force_ms', 'on_plan', 'native_Minstr', 'off_Minstr', 'on_Minstr', 'force_Minstr', 'on_read', 'why', 'verdict']
    print('\t'.join(cols))
    for (cell, n), arms in sorted(cells.items(), key=lambda kv: (int(kv[0][1]), kv[0][0])):
        v = verdicts(arms, n)
        for x in v:
            tally[x.split(':')[0]] = tally.get(x.split(':')[0], 0) + 1
        g = lambda k, f: (arms.get(k) or {}).get(f, '')
        print('\t'.join([cell, n, g('native', 'ms'), g('off', 'ms'), g('on', 'ms'), g('force', 'ms'), g('on', 'plan'), g('native', 'instr_steady'),
                         g('off', 'instr_steady'), g('on', 'instr_steady'), g('force', 'instr_steady'), g('on', 'read'), g('on', 'why'), ','.join(v)]))
    print('%d cells; %s' % (len(cells), ', '.join('%s %d' % kv for kv in sorted(tally.items()))), file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('gen')
    r = sub.add_parser('run')
    r.add_argument('--rows', default='100000')
    r.add_argument('--cells')
    r.add_argument('--profile', choices=list(PROFILES))
    r.add_argument('--out')
    r.add_argument('--resume', action='store_true')
    r.add_argument('--timed', type=int, default=3)
    r.add_argument('--no-perf', action='store_true')
    r.add_argument('--no-force', dest='force', action='store_false')
    p = sub.add_parser('report')
    p.add_argument('files', nargs='+')
    a = ap.parse_args()
    {'gen': lambda: gen(), 'run': lambda: run(a), 'report': lambda: report(a)}[a.cmd]()


if __name__ == '__main__':
    main()
