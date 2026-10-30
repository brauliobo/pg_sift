# The sizes of the relations of a run and their verdict.
# A size changes only past the page tolerance.

FORKS = ('main','fsm','vm','init')
MUTABLE_RELATIONS = {'mutable':'mutable_table_storage','mutable_pk':'mutable_btree_index'}


def relation_forks(conn):
    rows = conn.rows("SELECT c.relname," + ','.join(f"pg_relation_size(c.oid,'{fork}')" for fork in FORKS) +
                     " FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='public' AND c.relkind IN ('r','i') ORDER BY 1")
    return {row[0]:dict(zip(FORKS,map(int,row[1:]))) for row in rows}


def fork_totals(forks):
    if any(value<0 for relation in forks.values() for value in relation.values()):
        raise ValueError('negative relation fork size')
    return {name:sum(values.values()) for name,values in forks.items()}


def sizes(conn):
    return list(fork_totals(relation_forks(conn)).items())


SIZE_RELATIONS = {'docs_bm25':'bm25_index','docs_col':'column_index','store':'table_storage',
                  'store_pk':'btree_index','docs':'heap','seed':'fixture_heap'}
SIZE_PAGE_TOLERANCE = 2


def size_verdict(baseline, candidate):
    if not all(isinstance(x,int) and x>=0 for x in (baseline,candidate)):
        raise ValueError('invalid size measurement')
    threshold = max(math.ceil(baseline*MATERIAL),baseline+SIZE_PAGE_TOLERANCE*8192)
    return dict(baseline_bytes=baseline,candidate_bytes=candidate,
                baseline_pages=math.ceil(baseline/8192),candidate_pages=math.ceil(candidate/8192),
                ratio=candidate/baseline if baseline else None,threshold_bytes=threshold,
                absolute_tolerance_pages=SIZE_PAGE_TOLERANCE,relative_threshold=MATERIAL,
                status='regression' if candidate>threshold else 'pass')


def size_snapshot(revisions,state):
    forks = [relation_forks(r.conn) for r in revisions]
    return dict(compare_sizes([fork_totals(f) for f in forks],state,SIZE_RELATIONS),forks=forks)


def compare_sizes(snapshots,state,relations):
    comparisons = []
    for name,role in relations.items():
        if any(name not in snap for snap in snapshots):
            raise ValueError('missing size relation '+name)
        comparisons.append(dict(relation=name,role=role,state=state,
                                gated=role in ('bm25_index','column_index','table_storage','mutable_table_storage','mutable_btree_index'),
                                **size_verdict(int(snapshots[0][name]),int(snapshots[1][name]))))
    for side,snapshot in enumerate(snapshots):
        if any(int(v)<0 for v in snapshot.values()):
            raise ValueError('negative relation size')
    return dict(state=state,relations=comparisons,
                footprint_bytes=[sum(int(v) for v in snap.values()) for snap in snapshots],
                status='regression' if any(c['gated'] and c['status']=='regression' for c in comparisons) else 'pass')


def churn_sizes(revisions):
    digests = []
    for rev in revisions:
        conn = rev.conn
        conn.exec("UPDATE docs SET body=body || ' churn' WHERE id%11=0")
        for table in ('docs','store'):
            conn.script(f"UPDATE {table} SET amount=amount+1 WHERE id%19=0; DELETE FROM {table} WHERE id%29=0")
            conn.exec(f'VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) {table}')
            conn.exec('SELECT pg_current_xact_id()')
            conn.exec(f'VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) {table}')
        digests.append([conn.rows(f"SELECT count(*),md5(string_agg(row(t.*)::text,'|' ORDER BY id)) FROM {table} t") for table in ('docs','store')])
    require_equal(digests[0],digests[1],'settled churn logical fixture')
    result = size_snapshot(revisions,'churn_vacuum')
    result['logical_digests'] = digests
    return result
