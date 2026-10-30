# The report of a run: reductions, the files written and the summaries.
# write_report() publishes the report, also after an error.

def normal_size_checks(report):
    return report['size_checks'] + [c['validation']['size_checks'] for c in report['cases']
                                   if c.get('validation',{}).get('size_checks')] + [
        v['size_checks'] for v in report.get('acceptance_catalog_validation',{}).values() if v.get('size_checks')]


def reduce_report(report):
    """Evidence survives interruption; coverage determines whether PASS is possible."""
    sizes = normal_size_checks(report)
    size_regression = any(s['status']=='regression' for s in sizes)
    if report['size_complete']:
        report['size_complete'] = bool(sizes) and set(report['required_size_states']) <= {s['state'] for s in sizes}
    report['size_status'] = ('regression' if size_regression else
                             'pass' if report['size_complete'] and sizes else 'inconclusive')
    if report['status']=='error':
        return
    if report['config']['verify_control']:
        report['status'] = ('pass' if report['complete'] and report['size_status']=='pass'
                             and report.get('acceptance_pass',False) else 'inconclusive')
        return
    statuses = {c['timing_status'] for c in report['cases'] if c['name'] in GATING}
    report['status'] = ('regression' if size_regression or 'regression' in statuses else
                        'inconclusive' if not report['complete'] or statuses-{'pass','skipped'} or report['size_status']!='pass' else 'pass')


def reduce_insert_feasibility(report):
    """Scoped unchanged diagnostic; neither normal coverage nor acceptance is evaluated."""
    report.update(normal_gate_evaluated=False,acceptance_evaluated=False,
                  diagnostic_complete=report['complete'] and report['status']!='error',complete=False,size_complete=False,
                  size_status='not_evaluated')
    if report['status']=='error':
        report['diagnostic_status'],report['status'] = 'error','diagnostic_error'
        return
    case = report['cases'][0] if report['cases'] else {}
    sizes = case.get('validation',{}).get('size_checks')
    report['diagnostic_size_status'] = sizes['status'] if sizes else 'inconclusive'
    initial,confirmation = case.get('initial',{}),case.get('confirmation',{})
    costs = []
    try:
        operations = case.get('operations')
        clean_full_n_pair_cost(case.get('calibration',{}).get('pilot_pair',{}),INSERT_PILOT,'pilot')
        histories=case['calibration']['histories']
        costs.append(clean_observation_cost(case.get('validation',{}).get('pair',{}),operations,histories,'validation'))
        for block in (initial,confirmation):
            if len(block.get('pairs',[]))!=6:
                raise Inconclusive('diagnostic requires six complete pair evidence records in each block')
            costs.extend(clean_observation_cost(pair,operations,histories,'verdict') for pair in block['pairs'])
    except Inconclusive as exc:
        report['diagnostic_complete'],report['diagnostic_evidence_error'] = False,str(exc)
    else:
        report['clean_full_N_cost_evidence'] = dict(max_clean_full_N_pair_wall=max(costs),pair_walls=costs,
                                                  provenance='complete clean full-N validation and verdict pairs; pilot excluded')
    report['required_control_costs'] = {name:'NOT_MEASURED' for name in CONTROL_COST_PHASES}
    known_regression = (sizes and sizes['status']=='regression') or (
        'lower' in confirmation and confirmation['lower']>MATERIAL)
    both_pass = all(block.get('n')==6 and block['upper']<=MATERIAL for block in (initial,confirmation))
    status = ('regression' if known_regression else 'pass' if report['diagnostic_complete'] and both_pass
              and report['diagnostic_size_status']=='pass' else 'inconclusive')
    report['diagnostic_status'],report['status'] = status,'diagnostic_'+status
    report['achieved_windows'] = {name:dict(baseline_seconds=b['baseline_median'],candidate_seconds=b['candidate_median'],
                                          target_seconds=TARGET,target_fractions=[b['baseline_median']/TARGET,b['candidate_median']/TARGET],
                                          target_attained=min(b['baseline_median'],b['candidate_median'])>=TARGET)
                                  for name,b in (('initial',initial),('confirmation',confirmation)) if 'n' in b}


def write_report(output, report):
    with run_phase(report,'report_write') as phase:
        _write_report(output,report)
        phase['artifact_bytes']={name:(output/name).stat().st_size for name in ('results.json','summary.md')}


def _write_report(output, report):
    (output/'results.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    lines = ['# INSERT feasibility diagnostic' if report['config'].get('insert_feasibility') else '# Focused performance gate', '', f"Status: **{report['status']}**. {report['date']}.",
             f"Normal size gate: **{report['size_status']}**.",
             f"Complete: **{report['complete']}**. Size coverage complete: **{report['size_complete']}**. Budget exhausted: **{report['budget_exhausted']}**.",
             f"Baseline: `{report['baseline_ref']}`. Candidate: `{report['candidate_ref']}`.",
             f"PostgreSQL: {report.get('postgresql','unknown')}. CPU: {report['cpu']}; CPUs: {report['cpus']}.",
             f"Runner: {report['runner']}. Load: {report['load']}.",
             'Metric: monotonic elapsed persistent SQL end-to-end, including planning and result receipt.',
             'Six initial alternating warmed pairs; one fresh confirmation: 16 read pairs or 6 mutation pairs; a confirmation still inconclusive takes one fresh 16-pair extension when the budget covers it and the cases after it. Ordinary cases confirm suspects; controlled INSERT and INSERT feasibility always confirm and never extend. Fixed common operations; 95% Student-t log-ratio CI; material ratio 1.10.',
             '']
    if report['config'].get('insert_feasibility'):
        lines.extend(['', 'Scope: INSERT PR geometry only. Normal gate and acceptance NOT evaluated.',
                      'Diagnostic coverage: `'+json.dumps(report.get('coverage',{}))+'`.',
                      'Achieved windows: `'+json.dumps(report.get('achieved_windows',{}))+'`.',
                      'A diagnostic pass does not establish adequate SQL window, power or gate readiness.'])
    lines.extend(['', '| Case | Phase | n | Ratio | 95% CI | Base median [min,max] s | Head median [min,max] s | Status |',
                  '|---|---|---:|---:|---|---|---|---|'])
    for case in report['cases']:
        for phase in ('initial','confirmation','extension'):
            if phase in case and 'n' in case[phase]:
                b = case[phase]
                lines.append(f"| {case['name']} | {phase} | {b['n']} | {b['ratio']:.3f} | [{b['lower']:.3f}, {b['upper']:.3f}] | {b['baseline_median']:.4f} {b['baseline_range']} | {b['candidate_median']:.4f} {b['candidate_range']} | {case['status']} |")
    lines.extend(['', '| Case / workload | Phase | Complete | Timing | Combined | Seed / first ID / N |',
                  '|---|---|---|---|---|---|'])
    for case in report['cases']:
        identity = case.get('identity',{})
        details = f"{identity.get('seed_count','-')} / {identity.get('first_id','-')} / {case.get('operations','-')}"
        lines.append(f"| {case['name']} / {case['workload_version']} | {case['phase']} | {case['complete']} | {case['timing_status']} | {case['status']} | {details} |")
    for case in report['cases']:
        if 'control_budget' in case:
            lines.extend(['', 'Mutation control budget: `'+json.dumps(case['control_budget'])+'`.', ''])
        if case.get('lifecycle')=='insert_trajectory':
            lines.extend(['', 'INSERT work: `'+json.dumps({k:case.get('calibration',{}).get(k) for k in
                         ('operations','histories','desired_histories','history_cap','predicted_observation_sql_seconds',
                          'target_attained_prediction','internal_order_policy','limitations')})+'`.',
                         'Actual achieved windows: `'+json.dumps(case.get('achieved_windows',{}))+'`. All histories contribute client SQL sums to one observation; n counts observations only. Per-operation Python/encoding/deadline/libpq/autocommit overhead retained, with all ordinal spikes. No bias-correction or sqrt(K) precision claim.'])
    lines.extend(['', 'Physical schedule: `'+report.get('physical_schedule_version','normal catalog order')+'`.',
                  'Measured phase ledger (nested phases are explanatory): `phase_ledger` in results.json; final closed-writer accounting: `terminal-accounting.json`.',
                  'Same-invocation future cost estimates: `'+json.dumps(report.get('post_control_costs',{}))+'`. Estimates are not bounds.',
                  'Cleanup errors: `'+json.dumps(report.get('cleanup_errors',[]))+'`.',
                  'UNIX socket paths: `'+json.dumps(report.get('unix_socket_paths',{}))+'`.',
                  'Server log preservation: `'+json.dumps(report.get('server_logs',{}))+'`.'])
    lines.extend(['', 'Required size states: `'+json.dumps(report.get('required_size_states',[]))+'`.'])
    lines.extend(['', '| Size state | Relation / role | Base bytes/pages | Head bytes/pages | Ratio | Limit bytes | Status |', '|---|---|---|---|---:|---:|---|'])
    for snapshot in normal_size_checks(report) + ([report['size_control']] if 'size_control' in report else []):
        for r in snapshot['relations']:
            lines.append(f"| {snapshot['state']} | {r['relation']} / {r['role']} | {r['baseline_bytes']} / {r['baseline_pages']} | {r['candidate_bytes']} / {r['candidate_pages']} | {r['ratio']} | {r['threshold_bytes']} | {r['status']} |")
    lines.extend(['', 'Phase runtime (seconds): `'+json.dumps(report['runtime_seconds'])+'`.',
                  'Configuration: `'+json.dumps(report['config'])+'`.',
                  'Budget envelope: `'+json.dumps(report.get('budget',{}))+'`. Cleanup/report reserve is an allowance, not a hard deadline.',
                  'Fixture checkpoint barrier: `'+json.dumps(report.get('fixture_checkpoint_barrier',{}))+'`.',
                  'This is a small warm serial workload on the recorded host; no hardware-counter or release throughput claim.'])
    if 'mutation_control_preflight' in report:
        preflight = report['mutation_control_preflight']
        lines.extend(['', 'Mutation control preflight (persisted before fault setup):',
                      f"N={preflight['operations']}; clean calibration, raw pilots and caps: `mutation_control_preflight.calibration` in results.json.",
                      'Budget: `'+json.dumps(preflight['control_budget'])+'`.'])
    if 'error' in report:
        lines.extend(['', 'Error: '+report['error']])
    (output/'summary.md').write_text('\n'.join(lines)+'\n')
