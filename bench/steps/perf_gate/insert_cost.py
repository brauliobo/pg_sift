# The cost ledger of the INSERT cases: what each phase spent and the budget of the controls.
# It reconciles the measured cost with the deadline.

def finite_number(value):
    return type(value) in (int,float) and math.isfinite(value)


def clean_full_n_pair_cost(pair, operations, role):
    """Only complete, finite full-N validation/verdict evidence can fund controls."""
    if (pair.get('workload_version')!=next(c.workload_version for c in CASES if c.lifecycle=='insert_trajectory')
            or not pair.get('complete') or not pair.get('clean') or pair.get('operations')!=operations
            or pair.get('role')!=role or len(pair.get('sides',[]))!=2):
        raise Inconclusive('missing completed clean full-N '+role+' pair cost evidence')
    for side in pair['sides']:
        oracle=side.get('oracle',{})
        if (oracle.get('multiset_difference')!=0 or oracle.get('actual_count')!=oracle.get('expected_count')
                or not isinstance(oracle.get('expected_count'),int) or oracle['expected_count']<=0
                or not oracle.get('expected_digest') or oracle.get('actual_digest')!=oracle['expected_digest']
                or not all(key in side.get(state,{}) for state in ('before','after') for key in ('geometry','forks','totals'))):
            raise Inconclusive('missing completed full-N oracle/geometry/raw-fork evidence')
        if any(len(side.get(key,[]))!=operations or any(not finite_number(v) or v<=0 for v in side[key])
               for key in ('operation_seconds','warm_operation_seconds')):
            raise Inconclusive('missing complete full-N raw operation evidence')
        for metric,raw in (('sql_seconds','operation_seconds'),('warm_sql_seconds','warm_operation_seconds')):
            if not finite_number(side.get(metric)) or side[metric]<=0 or not math.isclose(side[metric],sum(side[raw]),rel_tol=1e-12):
                raise Inconclusive('full-N SQL sum differs from complete raw point evidence')
        if set(side.get('phases',{}))!=set(INSERT_PHASES):
            raise Inconclusive('missing full-N phase cost evidence')
        for phase in side['phases'].values():
            if not phase.get('complete') or any(not finite_number(phase.get(key,math.nan)) for key in
                                               ('seconds','start_offset_seconds','end_offset_seconds')) or phase['seconds']<0 or not math.isclose(phase['end_offset_seconds']-phase['start_offset_seconds'],phase['seconds'],abs_tol=1e-9):
                raise Inconclusive('incomplete/nonfinite full-N phase cost evidence')
    wall,phases,residual = (pair.get(key,math.nan) for key in ('wall_seconds','phase_seconds','residual_seconds'))
    actual = sum(p['seconds'] for side in pair['sides'] for p in side['phases'].values())
    if (not all(finite_number(v) for v in (wall,phases,residual)) or wall<=0 or residual<0
            or not math.isclose(actual,phases,abs_tol=1e-9) or not math.isclose(wall,phases+residual,abs_tol=1e-9)):
        raise Inconclusive('invalid complete full-N wall/phase/residual evidence')
    return wall


def clean_observation_cost(observation, operations, histories, role):
    if (not observation.get('complete') or not observation.get('clean') or observation.get('operations')!=operations
            or observation.get('history_count')!=histories or observation.get('role')!=role
            or observation.get('workload_version')!=next(c.workload_version for c in CASES if c.lifecycle=='insert_trajectory')
            or len(observation.get('histories',[]))!=histories):
        raise Inconclusive('missing complete frozen N/K observation cost evidence')
    history_seconds=sum(clean_full_n_pair_cost(h,operations,role) for h in observation['histories'])
    wall=observation.get('wall_seconds',math.nan)
    residual=observation.get('residual_seconds',math.nan)
    if not finite_number(wall) or not finite_number(residual) or residual<0 or not math.isclose(wall,history_seconds+residual,abs_tol=1e-9):
        raise Inconclusive('invalid composite wall/history/residual evidence')
    return wall


@contextlib.contextmanager
def run_phase(report, name):
    """Invocation-owned measured phases; nested spans explain, never double-charge."""
    entries=report.setdefault('phase_ledger',[])
    parent=next((i for i in range(len(entries)-1,-1,-1) if 'end_offset_seconds' not in entries[i]),None)
    start=time.monotonic()
    entry=dict(name=name,parent=parent,start_offset_seconds=start-PROCESS_ORIGIN,started_at=utc_now(),complete=False)
    entries.append(entry)
    try:
        yield entry
        entry['complete']='error' not in entry
    except Exception as exc:
        entry['error']=f'{type(exc).__name__}: {exc}'
        raise
    finally:
        end=time.monotonic()
        entry.update(end_offset_seconds=end-PROCESS_ORIGIN,seconds=end-start,finished_at=utc_now())


def reconcile_ledger(report, start, end):
    entries=report.get('phase_ledger',[])
    exclusive=sum(e['seconds'] for e in entries if e['parent'] is None)
    total=end-start
    residual=total-exclusive
    if not finite_number(residual) or residual<0:
        raise ValueError('phase ledger overlaps or exceeds driver wall')
    return dict(driver_wall_seconds=total,exclusive_phase_seconds=exclusive,residual_seconds=residual,
                partition='only parent=null inclusive spans + residual; child phases/pair histories are explanatory')


def measured_estimate(entry, report, scope):
    if not entry.get('complete') or not finite_number(entry.get('seconds',math.nan)) or entry['seconds']<=0:
        raise Inconclusive('missing completed finite cost producer: '+scope)
    return dict(complete=True,seconds=entry['seconds'],kind='same_invocation_measured_estimate',
                provenance=dict(phase=entry['name'],baseline=report['baseline_ref'],candidate=report['candidate_ref'],
                                tier=report['config']['tier'],settings=SETTINGS,scope=scope,
                                artifact_bytes=entry.get('artifact_bytes')))


def restart_identity(rev):
    settings=sorted(re.findall(r'SET ([a-z_.]+)=',SETTINGS))
    return dict(seed=rev.conn.rows(SEED_IDENTITY),versions=rev.versions,
                system=rev.conn.rows('SELECT system_identifier FROM pg_control_system()'),
                settings=rev.conn.rows("SELECT name,setting FROM pg_settings WHERE name IN ("+
                                      ','.join("'"+name+"'" for name in settings)+") ORDER BY name"))


def completed_control_schedule(report):
    required={c.name for c in CASES if c.lifecycle!='insert_trajectory'}
    catalog=report.get('acceptance_catalog_validation',{})
    read_controls={c['name']:c for c in report['cases'] if c['name'] in ('control_unchanged','control_sql_slowdown')}
    phases=[dict(entry) for entry in report.get('phase_ledger',[]) if entry['name'] in
            ('churn_normal_sizes','bm25_growth_logical_forks')]
    states={s['state'] for s in normal_size_checks(report)}
    catalog_complete=required<=catalog.keys() and all(catalog[name].get('complete') for name in required)
    reads_complete=set(read_controls)=={'control_unchanged','control_sql_slowdown'} and all(c.get('complete') for c in read_controls.values())
    size_complete='size_control' in report and report['size_control']['state']=='control_build_segments_64'
    coverage=dict(catalog_complete=catalog_complete,read_controls_complete=reads_complete,size_control_complete=size_complete,
                  normal_size_states=sorted(states),phases=phases,sources=[report['baseline_ref'],report['candidate_ref'],report['config']['tier']],
                  scope='completed actuals already spent; never future demand')
    coverage['complete']=valid_completed_control_schedule(coverage,tuple(coverage['sources']))
    return coverage


def valid_completed_control_schedule(coverage, sources):
    phases=coverage.get('phases',[])
    return (coverage.get('sources')==list(sources) and coverage.get('catalog_complete') and coverage.get('read_controls_complete')
            and coverage.get('size_control_complete') and {'settled_build','churn_vacuum','mutable_vacuum'}<=set(coverage.get('normal_size_states',[]))
            and {p['name'] for p in phases}=={'churn_normal_sizes','bm25_growth_logical_forks'}
            and len(phases)==2 and all(p.get('complete') and finite_number(p.get('seconds',math.nan)) and p['seconds']>=0 for p in phases))


def prepare_control_costs(revisions, report, output):
    """One inactive infrastructure cycle and one existing-cluster stop/restart."""
    report['completed_control_work']=completed_control_schedule(report)
    if not report['completed_control_work']['complete']:
        raise Inconclusive('read/catalog/normal-size/BM25 prerequisites incomplete before infrastructure probe')
    identity=insert_identity(revisions)
    report['control_setup_intent']=dict(phase='inactive_infrastructure_probe',complete=False,admitted=False,
        active_fault=False,mutable_writes_permitted=False,seed_max_id=identity['seed_max_id'],
        schedule='all reads/catalog -> normal churn/forks -> BM25 growth proof -> inactive costs/restart -> clean composite INSERT -> admitted CPU control')
    write_report(output,report)  # closed intent before first inactive DDL
    conn=revisions[1].conn
    require_control_absent(conn)
    before=conn.rows(SEED_IDENTITY)
    with run_phase(report,'inactive_fault_setup') as install:
        install_insert_control(conn,identity['seed_max_id'],CPU_PILOT_ITERATIONS,active=False,sink=report['control_setup_intent'])
    primary=None
    try:
        require_equal(conn.rows("SELECT tgenabled FROM pg_trigger WHERE tgname='perf_gate_insert_delay'"),[['D']],'probe trigger disabled')
        report['cpu_calibration']={}
        with run_phase(report,'cpu_helper_calibration'):
            calibrate_cpu_work(conn,report['cpu_calibration'])
        require_equal(insert_control_count(conn),0,'inactive probe has no faulted rows')
        require_equal(before,conn.rows(SEED_IDENTITY),'inactive probe seed identity')
    except Exception as exc:
        primary=exc
        raise
    finally:
        with run_phase(report,'inactive_fault_removal') as removal:
            removed=remove_insert_control(conn,report['control_setup_intent'],primary)
            if not removed: removal['error']='inactive SQL cleanup incomplete; see control_setup_intent.cleanup_errors'
    if not removed: raise Inconclusive('inactive infrastructure removal incomplete')
    require_control_absent(conn)
    identities=[restart_identity(r) for r in revisions]
    with run_phase(report,'cluster_stop_probe') as stop:
        for rev in reversed(revisions): rev.stop(output)
    with run_phase(report,'cluster_restart'):
        for rev in revisions: rev.start()
        for rev,expected in zip(revisions,identities):
            require_equal(restart_identity(rev),expected,'same cluster/binary/settings after restart')
            require_control_absent(rev.conn)
    report['post_control_costs']=dict(
        fault_setup=measured_estimate(install,report,'inactive install/disable/init; future enable and varying DDL wall unknown'),
        fault_removal=measured_estimate(removal,report,'same DROP trigger/functions; future duration varies'),
        cluster_teardown=measured_estimate(stop,report,'same two clusters/log copies; final dirty state and rmtree differ'))
    report['control_setup_intent']['complete']=True
    write_report(output,report)
    refresh_report_cost(report)


def refresh_report_cost(report):
    closed=[e for e in report['phase_ledger'] if e['name']=='report_write' and e.get('complete')]
    if not closed: raise Inconclusive('no closed report writer cost evidence')
    report['post_control_costs']['final_report']=measured_estimate(max(closed,key=lambda e:e['seconds']),report,
        'largest closed real report at current volume; final growth, terminal sidecar and upload unknown')


def insert_control_budget(unchanged, deadline, cost_evidence, *, cpu_work, sources, completed_work, reserve_seconds=0):
    operations,histories=(unchanged['calibration'][key] for key in ('operations','histories'))
    population=[]
    try:
        if not unchanged.get('complete'):
            raise Inconclusive('clean INSERT case incomplete before control')
        for name in ('initial','confirmation'):
            if name=='confirmation' and name not in unchanged: continue
            block=unchanged.get(name,{})
            if block.get('n')!=6 or sum(bool(pair.get('complete')) for pair in block.get('pairs',[]))!=6:
                raise Inconclusive('clean INSERT '+name+' block lacks six completed observations')
        population.append(dict(source='validation',wall_seconds=clean_observation_cost(
            unchanged.get('validation',{}).get('pair',{}),operations,histories,'validation')))
        for block_name in ('initial','confirmation'):
            for index,pair in enumerate(unchanged.get(block_name,{}).get('pairs',[])):
                if pair.get('complete'):
                    population.append(dict(source=f'{block_name}.pairs[{index}]',wall_seconds=clean_observation_cost(pair,operations,histories,'verdict')))
    except Inconclusive as exc:
        clean_error=str(exc)
    else:
        clean_error=None
    missing=[]
    estimates={}
    for name in CONTROL_COST_PHASES:
        entry=cost_evidence.get(name)
        if (not entry or not entry.get('complete') or not entry.get('provenance')
                or not finite_number(entry.get('seconds',math.nan)) or entry['seconds']<=0
                or not isinstance(entry['provenance'],dict)
                or tuple(entry['provenance'].get(k) for k in ('baseline','candidate','tier'))!=sources
                or entry['provenance'].get('settings')!=SETTINGS):
            missing.append(name)
        else:
            estimates[name]=entry
    if not valid_completed_control_schedule(completed_work,sources): missing.append('completed_read_catalog_churn_BM25_work')
    visits=INITIAL_PAIRS+MUTATION_CONFIRMATION_PAIRS+1
    rows=2*operations*histories*visits
    maximum=max(p['wall_seconds'] for p in population) if population and clean_error is None else None
    cpu=cpu_work['predicted_added_seconds_per_row']
    if not finite_number(cpu) or cpu<=0: raise Inconclusive('invalid frozen CPU work cost evidence')
    added=rows*cpu
    sampling=visits*maximum+added+estimates['fault_setup']['seconds'] if maximum is not None and not missing else None
    tail=sum(estimates[k]['seconds'] for k in POST_CONTROL_PHASES) if not missing else None
    return dict(controlled_pairs=visits,histories=histories,expected_checkpoint_calls=2*histories*visits,
                candidate_delayed_operations=rows,candidate_cpu_operations=rows,cpu_work=cpu_work,
                completed_work=completed_work,sources=list(sources),
                clean_full_N_population=population,clean_evidence_error=clean_error,max_clean_full_N_pair_wall=maximum,
                predicted_clean_pair_seconds=visits*maximum if maximum is not None else None,
                predicted_added_cpu_seconds=added,post_control_cost_evidence=estimates,
                missing_post_control_costs=missing,post_control_seconds=tail,
                predicted_seconds=sampling,predicted_total_remaining_seconds=sampling+tail if sampling is not None else None,
                evidence_complete=sampling is not None,remaining_seconds=deadline-time.monotonic(),reserve_seconds=reserve_seconds,
                cost_scope='13 * completed clean N/K observation max; pilot excluded; all history phases/residual once; completed tail already spent',
                limitations=['same-run measured estimates, not bounds','trigger seed predicate/counter/enable overhead not isolated',
                             'CPU helper slope includes client calls; per-row trigger cost may differ',
                             'future dirty-stop/report-growth/rmtree/upload variability covered by allowance, not measured zero'])
