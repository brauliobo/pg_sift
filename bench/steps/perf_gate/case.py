# Preparing and timing one case: reset, configure, the plan checks, calibration and the elapsed time.
# A case names its statements, its operations and whether it gates.

def reset(conn, case):
    if case.reset:
        conn.script(case.reset)


def configure(conn, case):
    conn.script('SET sift.agg_preagg=on; SET sift.enable_agg=on')
    if case.settings:
        conn.script(case.settings)


def prepare(conn, case):
    configure(conn,case)
    reset(conn, case)


def table_geometry(conn, relation):
    fields = conn.rows(f"SELECT relation_blocks,free_blocks,reusable_blocks FROM sift.table_space('{relation}')")[0]
    segments,units = conn.rows(f"SELECT count(*),coalesce(sum(units),0) FROM sift.table_segments('{relation}') WHERE kind='columns'")[0]
    return dict(relation_blocks=int(fields[0]),free_blocks=int(fields[1]),
                reusable_blocks=int(fields[2]),column_segments=int(segments),units=int(units))


def plan_nodes(plan):
    if not plan:
        return []
    root = plan[0]['Plan'] if isinstance(plan,list) else plan
    return [root]+[node for child in root.get('Plans',[]) for node in plan_nodes(child)]


def topology(plan):
    return [{k:n[k] for k in ('Node Type','Custom Plan Provider','Index Name','Index','Relation Name','Parent Relationship') if k in n}
            for n in plan_nodes(plan)]


def require_plan(plan,case):
    nodes = plan_nodes(plan)
    if case.plan=='SiftAgg':
        valid = any(n.get('Node Type')=='Custom Scan' and n.get('Custom Plan Provider')=='SiftAgg' for n in nodes)
    else:
        valid = any((n.get('Node Type') in ('Index Scan','Index Only Scan','Bitmap Index Scan') and n.get('Index Name')==case.plan) or (case.plan=='docs_bm25' and n.get('Node Type')=='Custom Scan' and n.get('Custom Plan Provider') in ('SiftScan','SiftCount') and n.get('Index')==case.plan) for n in nodes)
    if not valid:
        raise PlanAbsent(f'{case.name}: required structured plan {case.plan} absent: {topology(plan)}')


def validate_case(revisions, case):
    if case.lifecycle=='insert_trajectory':
        raise ValueError('INSERT validation requires frozen full-trajectory calibration')
    checks, plans, footprints = [], [], []
    for rev in revisions:
        conn = rev.conn
        prepare(conn, case)
        if case.lifecycle=='reset_each_operation':
            before_forks = relation_forks(conn)
            before = list(fork_totals(before_forks).items())
            before_geometry = table_geometry(conn,'mutable')
            if before_geometry['units']<3:
                raise ValueError('mutable fixture lacks actual table unit boundary')
            if case.sql.startswith('VACUUM'):
                conn.exec(case.sql)
                plan = None
                # Advance the xid horizon and check free-space reuse after maintenance.
                conn.exec('SELECT pg_current_xact_id()')
                conn.exec(case.sql)
                geometry = table_geometry(conn,'mutable')
                if geometry['reusable_blocks'] <= before_geometry['reusable_blocks'] and geometry['relation_blocks'] >= before_geometry['relation_blocks']:
                    raise ValueError('VACUUM neither reclaimed nor made space reusable')
            else:
                plan = json.loads(conn.rows('EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON, TIMING OFF) '+case.sql)[0][0])
            got = conn.rows(case.check)
            # Mutable expected state is a plain heap SQL query, independent of table AM.
            oracle = {'storage_update': 'SELECT count(*),sum(id),sum(amount)+count(amount) FILTER (WHERE id BETWEEN 100 AND 200) FROM seed',
                      'storage_vacuum_space': 'SELECT count(*),sum(id),sum(amount) FROM seed WHERE id<=2048'}[case.name]
            require_equal(got, conn.rows(oracle), case.name+' heap oracle')
            after_forks = relation_forks(conn)
            footprints.append(dict(before=before,after=list(fork_totals(after_forks).items()),
                                   before_geometry=before_geometry,after_geometry=table_geometry(conn,'mutable'),
                                   before_forks=before_forks,after_forks=after_forks))
            plans.append(plan)  # VACUUM has no EXPLAIN; validate actual space instead.
        else:
            plan = json.loads(conn.rows('EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON, TIMING OFF) '+case.sql)[0][0])
            require_plan(plan,case)
            plans.append(plan)
            got = conn.rows(case.sql)
            if case.oracle:
                conn.exec('SET sift.enable_agg=off')
                require_equal(got, conn.rows(case.oracle), case.name+' heap oracle')
                conn.exec('SET sift.enable_agg=on')
            if not got:
                raise ValueError(case.name+': unexpectedly empty result')
        checks.append(got)
    require_equal(checks[0], checks[1], case.name+' baseline/candidate')
    plan_changed = topology(plans[0])!=topology(plans[1])
    lifecycle_sizes = None
    if case.name=='storage_vacuum_space':
        lifecycle_sizes = compare_sizes([dict(p['after']) for p in footprints],
                                        'mutable_vacuum',MUTABLE_RELATIONS)
        lifecycle_sizes['forks'] = [p['after_forks'] for p in footprints]
    return dict(results=checks,plans=plans,plan_changed=plan_changed,
                topology=[topology(p) for p in plans],sizes=footprints,size_checks=lifecycle_sizes)


def elapsed(conn, case, operations, deadline, delay=0, *, statements=None, raw=None):
    if case.lifecycle=='insert_trajectory' and (statements is None or len(statements)!=operations):
        raise ValueError('INSERT requires precomputed point statements')
    total = 0.0
    for ordinal in range(operations):
        if time.monotonic() >= deadline:
            raise BudgetExpired('measurement budget exhausted')
        if case.lifecycle=='reset_each_operation':
            reset(conn, case)
        if time.monotonic()>=deadline:
            raise BudgetExpired('global runtime budget exhausted after reset')
        start = time.perf_counter()
        conn.exec(statements[ordinal] if statements is not None else case.sql)
        value = time.perf_counter()-start
        if not math.isfinite(value) or value <= 0:
            raise ValueError('invalid monotonic SQL elapsed time')
        total += value
        if raw is not None:
            raw.append(value)
        if time.monotonic()>=deadline:
            raise BudgetExpired('global runtime budget exhausted after SQL')
    if delay:
        # Explicit acceptance mode: one SQL-visible delay per measured batch.
        start = time.perf_counter()
        conn.exec(f'SELECT pg_sleep({delay:.9f})')
        total += time.perf_counter()-start
        if time.monotonic()>=deadline:
            raise BudgetExpired('global runtime budget exhausted after SQL control')
    return total


def calibrate(revisions, case, deadline):
    sql_costs, wall_costs = [], []
    for rev in revisions:
        prepare(rev.conn,case)
        elapsed(rev.conn,case,1,deadline)
        start = time.monotonic()
        sql_costs.append(elapsed(rev.conn,case,3,deadline)/3)
        wall_costs.append((time.monotonic()-start)/3)
    desired = max(1,math.ceil(TARGET/min(sql_costs)))
    cap = max(1,math.floor(BATCH_WALL_LIMIT/max(wall_costs)))
    operations = min(desired,cap)
    return dict(operations=operations,desired_operations=desired,
                sql_seconds_per_operation=sql_costs,wall_seconds_per_operation=wall_costs,
                target_limited_by_reset=operations<desired)
