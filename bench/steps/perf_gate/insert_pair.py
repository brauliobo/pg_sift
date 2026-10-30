# One pair of INSERT timings: the phases, the order of the two revisions and the observations.
# A pair restores the same state for both revisions.

INSERT_PHASES = ('warm_reset','warm','final_reset','preprobes','checkpoint','stream','postcheck')
POST_CONTROL_PHASES = ('fault_removal','final_report','cluster_teardown')
CONTROL_COST_PHASES = ('fault_setup',)+POST_CONTROL_PHASES


@contextlib.contextmanager
def insert_phase(side, name, deadline):
    start = time.monotonic()
    phase = dict(started_at=utc_now(),start_offset_seconds=start-PROCESS_ORIGIN,complete=False)
    side['phases'][name] = phase
    try:
        if start>=deadline:
            raise BudgetExpired('global budget exhausted before INSERT '+name)
        yield
        if time.monotonic()>=deadline:
            raise BudgetExpired('global budget exhausted after INSERT '+name)
        phase['complete'] = True
    except Exception as exc:
        phase['error'] = dict(type=type(exc).__name__,error=str(exc))
        raise
    finally:
        end = time.monotonic()
        phase.update(finished_at=utc_now(),end_offset_seconds=end-PROCESS_ORIGIN,seconds=end-start)


def insert_pair(revisions, case, identity, operations, deadline, sink, *, order=(0,1), role='verdict', control=False):
    """One finite paired restoration -> barrier -> streams -> deferred checks owner."""
    label_c = time.monotonic()
    sides = [dict(phases={},operation_seconds=[],warm_operation_seconds=[]),
             dict(phases={},operation_seconds=[],warm_operation_seconds=[])]
    sink.update(operations=operations,order=list(order),role=role,clean=not control,
                workload_version=case.workload_version,sides=sides,complete=False,started_at=utc_now(),
                before_observation='pre_finalization',after_observation='pair_end',
                start_offset_seconds=label_c-PROCESS_ORIGIN,phase='warm')
    try:
        statements = case.statements(identity['seed_max_id'],operations)
        for index in order:
            conn,side = revisions[index].conn,sides[index]
            if control and index==1:
                side['control_counts'] = []
            with insert_phase(side,'warm_reset',deadline):
                if control and index==1: side['control_counts'].append(insert_control_count(conn))
                prepare(conn,case)
                if control and index==1: side['control_counts'].append(insert_control_count(conn))
            with insert_phase(side,'warm',deadline):
                side['warm_sql_seconds'] = elapsed(conn,case,operations,deadline,statements=statements,raw=side['warm_operation_seconds'])
                if control and index==1: side['control_counts'].append(insert_control_count(conn))
        sink['phase'] = 'final_restore_and_preprobes'
        for index in order:
            conn,side = revisions[index].conn,sides[index]
            with insert_phase(side,'final_reset',deadline):
                reset(conn,case)
            with insert_phase(side,'preprobes',deadline):
                if control and index==1: side['control_counts'].append(insert_control_count(conn))
                side['before'] = insert_footprint(conn)
        sink['phase'] = 'checkpoint'
        for index in order:
            with insert_phase(sides[index],'checkpoint',deadline):
                revisions[index].conn.exec('CHECKPOINT')
        sink.update(paired_ready_at=utc_now(),paired_ready_offset_seconds=time.monotonic()-PROCESS_ORIGIN)
        sink['phase'] = 'streams'
        for index in order:
            side = sides[index]
            with insert_phase(side,'stream',deadline):
                # No SQL other than the two point streams until BOTH streams return.
                start = time.monotonic()
                side['sql_seconds'] = elapsed(revisions[index].conn,case,operations,deadline,
                                              statements=statements,raw=side['operation_seconds'])
                side['loop_wall_seconds'] = time.monotonic()-start
        sink['phase'] = 'deferred_checks'
        for index in order:
            conn,side = revisions[index].conn,sides[index]
            with insert_phase(side,'postcheck',deadline):
                if control and index==1:
                    counts = side['control_counts']
                    counts.append(insert_control_count(conn))
                    first = counts[0]
                    require_equal(counts,[first,first,first+operations,first+operations,first+2*operations],
                                  'real per-new-row CPU counts; seed reset must not perform work')
                    checksum=conn.rows("SELECT current_setting('perf_gate.cpu_checksum')")[0][0]
                    side['control_checksum']=checksum
                    require_equal(checksum,cpu_checksum(identity['seed_max_id']+operations,control['iterations']),'last new-row CPU checksum')
                side['oracle'] = {}
                check_insert(conn,identity,operations,side['oracle'])
                side['after'] = insert_footprint(conn)
        for side in sides:
            phases = side['phases']
            stream_start = phases['stream']['start_offset_seconds']
            side.update(warm_age_seconds=stream_start-phases['warm']['end_offset_seconds'],
                        final_reset_age_seconds=stream_start-phases['final_reset']['end_offset_seconds'],
                        checkpoint_age_seconds=stream_start-phases['checkpoint']['end_offset_seconds'],
                        paired_ready_age_seconds=stream_start-sink['paired_ready_offset_seconds'],
                        deferred_check_age_seconds=phases['postcheck']['start_offset_seconds']-phases['stream']['end_offset_seconds'])
        sink.update(samples=[side['sql_seconds'] for side in sides],complete=True,phase='complete')
        return sink['samples']
    finally:
        phase_sum = sum(p['seconds'] for side in sides for p in side['phases'].values())
        finished_at = utc_now()
        end = time.monotonic()
        wall = end-origin
        sink.update(finished_at=finished_at,end_offset_seconds=end-PROCESS_ORIGIN,
                    wall_seconds=wall,phase_seconds=phase_sum,residual_seconds=wall-phase_sum)


def insert_observation(revisions, case, identity, operations, histories, deadline, sink, *, order=(0,1), role='verdict', control=False):
    """K dependent restored histories form ONE pair; publish only after every check."""
    if not 1<=histories<=INSERT_HISTORY_CAP:
        raise ValueError('invalid frozen INSERT history count')
    start=time.monotonic()
    schedule=[tuple(order if j%2==0 else reversed(order)) for j in range(histories)]
    sink.update(operations=operations,history_count=histories,internal_orders=schedule,histories=[],role=role,
                clean=not control,workload_version=case.workload_version,complete=False)
    try:
        totals=[0.,0.]
        for order in schedule:
            history={}
            sink['histories'].append(history)
            values=insert_pair(revisions,case,identity,operations,deadline,history,order=order,role=role,control=control)
            for side in (0,1): totals[side]+=values[side]
        sink.update(samples=totals,complete=True)
        return totals
    finally:
        sink['history_seconds']=sum(h['wall_seconds'] for h in sink['histories'] if 'wall_seconds' in h)
        sink['wall_seconds']=time.monotonic()-start
        sink['residual_seconds']=sink['wall_seconds']-sink['history_seconds']


def calibrate_insert(revisions, case, identity, deadline, sink):
    sink.update(pilot_operations=INSERT_PILOT,growth_cap_operations=INSERT_GROWTH_CAP,
                target_seconds=TARGET,predicted_wall_limit_seconds=BATCH_WALL_LIMIT,pilot_pair={},
                wall_cap_scope='FINAL reset + measured loop only; warm/probes/both checkpoints/waits/checks excluded')
    insert_pair(revisions,case,identity,INSERT_PILOT,deadline,sink['pilot_pair'],role='pilot')
    sides = sink['pilot_pair']['sides']
    costs = [side['sql_seconds']/INSERT_PILOT for side in sides]
    walls = [side['loop_wall_seconds']/INSERT_PILOT for side in sides]
    resets = [side['phases']['final_reset']['seconds'] for side in sides]
    if any(not math.isfinite(v) or v<=0 for v in costs+walls) or any(not math.isfinite(r) or r<0 for r in resets):
        raise ValueError('invalid INSERT pilot elapsed')
    desired = math.ceil(TARGET/min(costs))
    wall_cap = math.floor(min((BATCH_WALL_LIMIT-r)/w for r,w in zip(resets,walls)))
    sink.update(desired_operations=desired,wall_cap_operations=wall_cap,
                sql_seconds_per_operation=costs,loop_wall_seconds_per_operation=walls)
    if wall_cap<1:
        raise Inconclusive('no admissible INSERT trajectory within predicted reset+loop wall cap')
    operations = min(desired,wall_cap,INSERT_GROWTH_CAP)
    sink.update(operations=operations,limited_by_wall=operations<desired and wall_cap==operations,
                limited_by_growth=operations<desired and INSERT_GROWTH_CAP==operations,
                predicted_sql_seconds=[operations*c for c in costs],
                predicted_reset_loop_seconds=[r+operations*w for r,w in zip(resets,walls)])
    desired_histories=math.ceil(TARGET/min(sink['predicted_sql_seconds']))
    histories=min(desired_histories,INSERT_HISTORY_CAP)
    sink.update(histories=histories,desired_histories=desired_histories,history_cap=INSERT_HISTORY_CAP,
                internal_order_policy='alternate histories; complement entire schedule on BA observations',
                initial_and_confirmation_orders=[[[int((j+i)%2),int(1-(j+i)%2)] for j in range(histories)] for i in range(6)],
                validation_orders=[[j%2,1-j%2] for j in range(histories)],
                predicted_observation_sql_seconds=[histories*v for v in sink['predicted_sql_seconds']],
                predicted_observation_wall_seconds=histories*sink['pilot_pair']['wall_seconds'],
                predicted_max_clean_remaining_wall_seconds=13*histories*sink['pilot_pair']['wall_seconds'],
                clean_remaining_model_scope='13 composites of pilot history wall; N may differ; estimate not bound; pilot already spent',
                target_attained_prediction=histories>=desired_histories,
                limitations='client/encoding/deadline/libpq/autocommit per-row overhead retained; label_a does not remove side bias or correlated noise')
    return sink


def validate_insert(revisions, case, identity, operations, deadline, sink, *, control=False, histories=1):
    sink.update(pair={},size_checks=None,history_size_checks=[],complete=False)
    try:
        insert_observation(revisions,case,identity,operations,histories,deadline,sink['pair'],role='validation',control=control)
    finally:
        # Size decisions belong to completed histories, independently of observation timing.
        for history in sink['pair'].get('histories',[]):
            if not history.get('complete'):
                continue
            sides=history['sides']
            sink['history_size_checks'].append(dict(compare_sizes([side['after']['totals'] for side in sides],
                                                    'mutable_insert_trajectory',MUTABLE_RELATIONS),
                                                   forks=[side['after']['forks'] for side in sides]))
        checks=sink['history_size_checks']
        if checks:
            complete=sink['pair'].get('complete',False) and len(checks)==histories
            # First history represents the state; ANY completed history regression is decisive.
            sink['size_checks']=dict(checks[0],representative_history=0,history_checks=checks,
                                    complete=complete,completed_histories=len(checks),expected_histories=histories,
                                    status='regression' if any(s['status']=='regression' for s in checks) else
                                           'pass' if complete else 'inconclusive')
    sink['complete']=True
    return sink
