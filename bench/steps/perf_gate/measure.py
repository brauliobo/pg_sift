# Blocks of pairs and the measurement of a case, with the confirmation and extension blocks.
# A suspect block is decided by a fresh one.

def block(revisions, case, operations, deadline, control=False, sink=None, *, pair_count=INITIAL_PAIRS, identity=None, histories=1):
    if pair_count not in T95:
        raise ValueError('unsupported independent pair count')
    start = time.monotonic()
    pairs = []
    saved = sink if sink is not None else {}
    saved.update(samples=pairs,expected_pairs=pair_count,started_at=utc_now())
    if case.lifecycle=='insert_trajectory':
        saved['pairs'] = []
    try:
        for round_no in range(pair_count):
            pair = [None,None]
            pairs.append(pair)
            order = [0,1] if round_no%2 == 0 else [1,0]
            if case.lifecycle=='insert_trajectory':
                evidence = {}
                saved['pairs'].append(evidence)
                values = insert_observation(revisions,case,identity,operations,histories,deadline,evidence,order=order,control=control)
                pair[:] = values  # valid only after BOTH deferred checks
            else:
                for side in order:
                    conn = revisions[side].conn
                    prepare(conn,case)
                    elapsed(conn,case,1,deadline)  # independent warmed batch, excluded
                    pair[side] = elapsed(conn,case,operations,deadline,0.075 if control and side else 0)
        saved.update(interval(pairs))
    finally:
        saved.update(finished_at=utc_now(),runtime_seconds=time.monotonic()-start)
    return saved


def measure(revisions, case, deadline, control=False, sink=None, *, frozen=None, report=None, output=None, force_confirmation=False, reserve=0):
    if report is None:
        report={}
    with run_phase(report,case.name+('_controlled' if control else '_clean')):
        return _measure(revisions,case,deadline,control,sink,frozen=frozen,report=report,output=output,force_confirmation=force_confirmation,reserve=reserve)


def _measure(revisions, case, deadline, control=False, sink=None, *, frozen=None, report=None, output=None, force_confirmation=False, reserve=0):
    start = time.monotonic()
    result = dict(name=case.name,group=case.group,smoke=case.smoke,status='inconclusive',
                  timing_status='inconclusive',complete=False,phase='validation',started_at=utc_now(),
                  lifecycle=case.lifecycle,workload_version=case.workload_version,phase_runtime_seconds={})
    if sink is not None:
        sink.append(result)
    inserted = case.lifecycle=='insert_trajectory'
    try:
        if inserted:
            result['phase'] = 'identity'
            identity = insert_identity(revisions)
            result['identity'] = identity
            result['phase'] = 'calibration'
            calibration_start = time.monotonic()
            result['calibration'] = {}
            if frozen is None:
                calibrate_insert(revisions,case,identity,deadline,result['calibration'])
            else:
                require_equal(identity,frozen['identity'],'frozen INSERT identity')
                result['calibration'] = frozen['calibration']
                result['calibration_reused_from'] = frozen['name']
            result['phase_runtime_seconds']['calibration'] = time.monotonic()-calibration_start
            calibration = result['calibration']
            if control:
                result['operations'] = calibration['operations']
                result['phase'] = 'control_preflight'
                cpu_work=freeze_cpu_work(calibration,report['cpu_calibration'])
                result['control_budget'] = insert_control_budget(frozen,deadline,report.get('post_control_costs',{}),
                    cpu_work=cpu_work,sources=(report['baseline_ref'],report['candidate_ref'],report['config']['tier']),
                    completed_work=report.get('completed_control_work',{}),
                    reserve_seconds=report.get('budget',{}).get('cleanup_report_reserve_seconds',0))
                report['mutation_control_preflight'] = dict(
                    recorded_at=utc_now(),phase='before_fault',fault_installed=False,
                    workload_version=case.workload_version,identity=identity,
                    operations=calibration['operations'],histories=calibration['histories'],calibration=calibration,
                    control_budget=result['control_budget'],
                    clean_validation=frozen['validation'],clean_blocks={name:frozen[name] for name in ('initial','confirmation') if name in frozen})
                # Existing artifacts must be closed on disk before the first fault DDL.
                write_report(output,report)
                fresh_remaining = deadline-time.monotonic()
                result['control_admission'] = dict(remaining_after_persistence_seconds=fresh_remaining,
                    workflow_remaining_after_persistence_seconds=fresh_remaining+result['control_budget']['reserve_seconds'])
                if not result['control_budget']['evidence_complete']:
                    raise Inconclusive('control not admitted: required clean/post-control cost evidence is incomplete')
                if (result['control_budget']['predicted_seconds']>fresh_remaining
                        or result['control_budget']['predicted_total_remaining_seconds']>fresh_remaining+result['control_budget']['reserve_seconds']):
                    raise BudgetExpired('insufficient fresh remaining budget after control preflight persistence')
                result['control'] = dict(kind='per_new_row_cpu_trigger_v1',work=cpu_work,
                                         seed_max_id=identity['seed_max_id'],counter='perf_gate.delayed_rows')
                result['phase'] = 'control_setup'
                # Track ownership before fallible DDL; failed transactional setup rolls back.
                with run_phase(report,'active_fault_setup'):
                    install_insert_control(revisions[1].conn,identity['seed_max_id'],cpu_work['iterations'],sink=result['control'])
            histories=calibration['histories']
            if report is not None and report.get('config',{}).get('insert_feasibility'):
                report['coverage'].update(histories=histories,expected_history_pairs=1+13*histories,expected_restoration_checkpoints=2*(1+13*histories),
                                          expected_resets=4*(1+13*histories))
            validation_start = time.monotonic()
            result['phase'] = 'validation'
            result['validation'] = {}
            validate_insert(revisions,case,identity,calibration['operations'],deadline,result['validation'],control=cpu_work if control else False,histories=histories)
            result['phase_runtime_seconds']['validation'] = time.monotonic()-validation_start
        else:
            try:
                result['validation'] = validate_case(revisions,case)
            except PlanAbsent as exc:
                # a planner change moved a revision off the plan the case is named for: it no longer times the same operation, so it is reported, not timed
                result.update(status='skipped',timing_status='skipped',complete=True,phase='complete',reason=str(exc))
                return result
            result['phase_runtime_seconds']['validation'] = time.monotonic()-start
            result['phase'] = 'calibration'
            calibration_start = time.monotonic()
            calibration = calibrate(revisions,case,deadline)
            result['phase_runtime_seconds']['calibration'] = time.monotonic()-calibration_start
            result['calibration'] = calibration
        operations = calibration['operations']
        result['operations'] = operations
        if force_confirmation:
            write_report(output,report)
        result['phase'] = 'initial'
        result['initial'] = {}
        options = {'identity':identity,'histories':histories} if inserted else {}
        block_control=cpu_work if control and inserted else control
        result['initial'].update(block(revisions,case,operations,deadline,block_control,sink=result['initial'],**options))
        result['status'] = verdict(result['initial'])
        if force_confirmation:
            write_report(output,report)
        if result['status'] == 'suspect' or force_confirmation or (control and inserted):
            result['status'] = 'inconclusive'
            result['phase'] = 'confirmation'
            result['confirmation'] = {}
            pair_count = READ_CONFIRMATION_PAIRS if case.lifecycle=='read' else MUTATION_CONFIRMATION_PAIRS
            result['confirmation'].update(block(revisions,case,operations,deadline,block_control,
                                                sink=result['confirmation'],pair_count=pair_count,**options))
            result['status'] = verdict(result['confirmation'],True)
            if force_confirmation:
                write_report(output,report)
            if result['status'] == 'inconclusive' and not (control or force_confirmation):
                result['extension_budget'] = extension_fits(result['confirmation'],deadline,reserve)
                if result['extension_budget']['fits']:
                    result['phase'] = 'extension'
                    result['extension'] = {}
                    result['extension'].update(block(revisions,case,operations,deadline,False,sink=result['extension'],
                                                     pair_count=EXTENSION_PAIRS,**options))
                    result['status'] = verdict(result['extension'],True)
        result['timing_status'] = result['status']
        if inserted:
            result['achieved_windows']={name:dict(sql_seconds=[result[name][key] for key in ('baseline_median','candidate_median')],
                target_seconds=TARGET,target_fractions=[result[name][key]/TARGET for key in ('baseline_median','candidate_median')],
                target_attained=min(result[name][key] for key in ('baseline_median','candidate_median'))>=TARGET)
                for name in ('initial','confirmation','extension') if name in result and 'n' in result[name]}
        if result['validation']['size_checks'] is not None:
            result['space_status'] = result['validation']['size_checks']['status']
            if result['space_status']=='regression':
                result['status'] = 'regression'
        result['complete'],result['phase'] = True,'complete'
        return result
    finally:
        try:
            if result.get('control',{}).get('infrastructure_committed'):
                primary=sys.exc_info()[1]
                with run_phase(report,'active_fault_removal') as removal:
                    result['control']['removed'] = remove_insert_control(revisions[1].conn,result,primary)
                    if not result['control']['removed']: removal['error']='active SQL cleanup incomplete; see case.cleanup_errors'
        finally:
            result['runtime_seconds'] = time.monotonic()-start
