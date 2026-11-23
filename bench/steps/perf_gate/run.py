# The command line, the checks around the run and main().
# main() returns 0 pass, 1 regression, 2 inconclusive or 3 error.

def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline-ref',required=True)
    p.add_argument('--candidate-ref',required=True)
    p.add_argument('--baseline-tree')
    p.add_argument('--candidate-tree')
    p.add_argument('--groups',default=None)
    p.add_argument('--insert-feasibility',action='store_true',help='equal-source INSERT PR-only diagnostic; always fixed6+6, no controls/normal gate')
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--tier',choices=['pr','nightly'],default='pr')
    p.add_argument('--verify-control',action='store_true',help='acceptance only: unchanged and SQL-visible slowdown; requires equal SHAs')
    p.add_argument('--scratch-root',type=Path,required=True,help='owned DB/build scratch filesystem (dedicated test partition on shared local host)')
    p.add_argument('--pg-config',default=shutil.which('pg_config'))
    p.add_argument('--port-base',type=int,default=56240)
    p.add_argument('--allow-loaded',action='store_true',help='smoke/dev only: time without waiting for a quiet host; a real gate waits (scripts/quiet.sh)')
    p.add_argument('--anchor-ref',help='pinned release anchor (a tag or commit such as v5.1.0 or the last RC): built once into --anchor-cache and timed against the candidate on every read case it can run')
    p.add_argument('--anchor-cache',type=Path,default=anchor_cache_root(),help='where staged anchors are kept, one per commit and PostgreSQL major (also filled by scripts/build-v510-fixture.sh)')
    p.add_argument('--anchor-record',type=Path,help='write the manifest of what the anchor costs per case (needs --anchor-ref; the file --anchor-tsv reads)')
    p.add_argument('--stage-cache',type=Path,help='dev loops: keep the staged build of every revision here, per commit, instead of building it again; no cache is used by default')
    p.add_argument('--build-jobs',type=int,default=2,help='make -j of each build')
    p.add_argument('--anchor-tsv',type=Path,help='manifest of the pinned release anchor (bench/baselines/anchor-manifest.tsv): a case that drifts from it fails, however small each commit was')
    p.add_argument('--no-perf',action='store_true',help='do not count instructions:u (elapsed time, plans, workers and sizes only)')
    p.add_argument('--quiet-max',type=float,default=QUIET_MAX_PERCENT,help='CPU and IO pressure (some avg10, percent) below which the host is quiet')
    p.add_argument('--quiet-timeout',type=float,default=QUIET_TIMEOUT_SECONDS,help='seconds to wait for a quiet host before a block; a block timed after the timeout is noisy')
    p.add_argument('--budget-seconds',type=float,default=300,help='global budget including builds/setup; exhaustion is inconclusive')
    p.add_argument('--workflow-start-seconds',type=float,help='workflow Unix start timestamp; prior elapsed time is deducted from the budget')
    p.add_argument('--cleanup-report-reserve-seconds',type=float,default=0,help='allowance deducted before builds/measurement; not a hard cleanup deadline')
    return p


def new_report(args):
    return dict(schema_version=4,complete=False,size_complete=False,budget_exhausted=False,date=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  baseline_ref=args.baseline_ref,candidate_ref=args.candidate_ref,status='inconclusive',cases=[],vs_anchor=[],anchor_profile={},anchor=None,size_checks=[],size_status='inconclusive',
                  cpu=next((l.split(':',1)[1].strip() for l in Path('/proc/cpuinfo').read_text().splitlines() if l.startswith('model name')),platform.machine()),
                  cpus=os.cpu_count(),runner=os.environ.get('RUNNER_NAME',platform.node()),load=os.getloadavg(),
                  config=dict(tier=args.tier,groups=args.groups,insert_feasibility=args.insert_feasibility,initial_pairs=INITIAL_PAIRS,
                              read_confirmation_pairs=READ_CONFIRMATION_PAIRS,mutation_confirmation_pairs=MUTATION_CONFIRMATION_PAIRS,
                              supported_pairs=list(T95),t_critical_by_pairs=T95,target_seconds=TARGET,threshold=MATERIAL,
                              insert_pilot_operations=INSERT_PILOT,insert_growth_cap_operations=INSERT_GROWTH_CAP,
                              insert_history_cap=INSERT_HISTORY_CAP,modes=sorted({m for c in CASES for m in c.modes}),cpu_pilot_rows=CPU_PILOT_ROWS,cpu_pilot_iterations=CPU_PILOT_ITERATIONS,cpu_iteration_cap=CPU_ITERATION_CAP,budget_seconds=args.budget_seconds,verify_control=args.verify_control,quiet_host_guard=not args.allow_loaded,anchor_ref=args.anchor_ref,anchor_tsv=str(args.anchor_tsv) if args.anchor_tsv else None,perf_instructions=PERF_AVAILABLE,build_workers=2,
                              build_flags='USE_PGXS=1 SIFT_TESTING= SIFT_LTO= SANITIZE= VULKAN=',settings=SETTINGS),runtime_seconds={})


def verify_control(revisions,report,args,deadline):
        report['physical_schedule_version']='acceptance_reads_sizes_infrastructure_then_composite_insert_v4'
        report['acceptance_catalog_validation'] = {}
        for catalog_case in CASES:
            if catalog_case.lifecycle=='insert_trajectory':
                continue
            validation = dict(complete=False)
            report['acceptance_catalog_validation'][catalog_case.name] = validation
            with run_phase(report,'catalog_'+catalog_case.name):
                validation.update(validate_case(revisions,catalog_case))
            validation['complete'] = True
        case = next(c for c in CASES if c.name=='storage_point_lookup')
        unchanged = measure(revisions,case,deadline,sink=report['cases'],report=report,output=args.output)
        unchanged['name'] = 'control_unchanged'
        slowed = measure(revisions,case,deadline,True,sink=report['cases'],report=report,output=args.output)
        slowed['name'] = 'control_sql_slowdown'
        with run_phase(report,'churn_normal_sizes'):
            report['size_checks'].append(churn_sizes(revisions))
        with run_phase(report,'bm25_growth_logical_forks'):
            before = revisions[1].conn.rows("SELECT md5(string_agg(row(docs.*)::text,'|' ORDER BY id)) FROM docs")
            # Explicit acceptance-only SQL fault: force many small BM25 build segments.
            revisions[1].conn.script('ALTER INDEX docs_bm25 SET (build_segments=64); REINDEX INDEX docs_bm25')
            require_equal(before,revisions[1].conn.rows("SELECT md5(string_agg(row(docs.*)::text,'|' ORDER BY id)) FROM docs"),'size control logical rows')
            report['size_control'] = size_snapshot(revisions,'control_build_segments_64')
        prepare_control_costs(revisions,report,args.output)
        insert = next(c for c in CASES if c.lifecycle=='insert_trajectory')
        unchanged_insert = measure(revisions,insert,deadline,sink=report['cases'],report=report,output=args.output)
        unchanged_insert['name'] = 'control_insert_unchanged'
        report['acceptance_catalog_validation'][insert.name] = unchanged_insert['validation']
        write_report(args.output,report)
        refresh_report_cost(report)
        slowed_insert = measure(revisions,insert,deadline,True,sink=report['cases'],frozen=unchanged_insert,
                                report=report,output=args.output)
        slowed_insert['name'] = 'control_insert_per_row_slowdown'
        report['acceptance_pass'] = (unchanged['status']=='pass' and slowed['timing_status']=='regression'
                                     and unchanged_insert['status']=='pass' and slowed_insert['timing_status']=='regression'
                                     and slowed_insert['confirmation']['n']==MUTATION_CONFIRMATION_PAIRS
                                     and slowed_insert['confirmation']['lower']>MATERIAL
                                     and all(s['status']=='pass' for s in normal_size_checks(report))
                                     and report['size_control']['status']=='regression')


def stop_revisions(revisions,scratch,report,args,cleanup_phase):
    for rev in reversed(revisions):
        try:
            rev.stop(args.output)
        except Exception as exc:
            report.setdefault('cleanup_errors',[]).append(f'{type(exc).__name__}: {exc}')
            cleanup_phase['error']='; '.join(report['cleanup_errors'])
            if report['status']!='error':
                report['status'],report['error']='error',f'cleanup failed: {exc}'
        finally:
            report.setdefault('server_logs',{})[rev.name]=rev.server_log_evidence
    if scratch and not any(r.started for r in revisions) and not report.get('cleanup_errors'):
        try:
            shutil.rmtree(scratch)
        except Exception as exc:
            report.setdefault('cleanup_errors',[]).append(f'scratch removal: {exc}')
            cleanup_phase['error']='; '.join(report['cleanup_errors'])
            if report['status']!='error': report['status'],report['error']='error',f'scratch cleanup failed: {exc}'


def write_terminal(report,args,start,end):
    terminal=dict(status=report['status'],baseline_ref=args.baseline_ref,candidate_ref=args.candidate_ref,
                  accounting=reconcile_ledger(report,start,end),phase_ledger=report.get('phase_ledger',[]),
                  driver_seconds=end-start,workflow_seconds=report.get('budget',{}).get('prior_elapsed_seconds',0)+end-start,
                  measurement_deadline_exceeded=end>DEADLINE,
                  workflow_budget_exceeded=report.get('budget',{}).get('prior_elapsed_seconds',0)+end-start>args.budget_seconds,
                  boundary='through closed final results/summary writer; terminal sidecar write, device cleanup and artifact upload outside',
                  errors=report.get('terminal_errors',[]),primary_error=report.get('error'),cleanup_errors=report.get('cleanup_errors',[]))
    (args.output/'terminal-accounting.json').write_text(json.dumps(terminal,indent=2,allow_nan=False)+'\n')


def record_source_identity(args,report):
    report['postgresql'] = command([args.pg_config,'--version'])
    if not report['postgresql'].startswith('PostgreSQL 18.'):
        raise ValueError('performance gate requires PostgreSQL 18')
    report['compiler'] = command(['cc','--version']).splitlines()[0]
    report['pg_config'] = command([args.pg_config,'--configure'])
    files = ('bench/steps/perf_gate.py',*PART_FILES,'bench/perf/catalog.py','bench/perf/fixture.sql')
    blobs = {}
    for name in files:
        blobs[name] = command(['git','rev-parse',args.candidate_ref+':'+name],cwd=ROOT)
        require_equal(command(['git','hash-object',name],cwd=ROOT),blobs[name],'committed candidate harness '+name)
    report['source_identity'] = dict(commit=args.candidate_ref,tree=command(['git','rev-parse',args.candidate_ref+'^{tree}'],cwd=ROOT),
                                     harness_blobs=blobs,harness_sha256={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in files},
                                     fixture_owner='candidate harness, loaded identically into both isolated exact-SHA builds')


def check_anchor_fixture(anchor, candidate):
    """the same logical fixture on an older release: the rows are what its per-row numbers are divided by"""
    digests = [r.conn.rows("SELECT count(*),md5(string_agg(row(seed.*)::text,'|' ORDER BY id)) FROM seed") for r in (anchor,candidate)]
    require_equal(digests[0],digests[1],'logical fixture of the anchor and the candidate')
    return int(digests[1][0][0])


def check_fixture(revisions,report):
    report['fixture_sha256'] = hashlib.sha256((ROOT/'bench/perf/fixture.sql').read_bytes()).hexdigest()
    digests = [r.conn.rows("SELECT count(*),md5(string_agg(row(seed.*)::text,'|' ORDER BY id)) FROM seed") for r in revisions]
    require_equal(digests[0],digests[1],'logical fixture')
    report['fixture_digest'] = digests[0]
    # The padded heap must really cross superblock boundaries.
    for rev in revisions:
        boundaries = int(rev.conn.rows("SELECT count(DISTINCT ((ctid::text::point)[0]::int/256)) FROM docs")[0][0])
        if boundaries < 3:
            raise ValueError('fixture fails multi-superblock boundary')
    report['table_geometry'] = {r.name:table_geometry(r.conn,'store') for r in revisions}
    for rev in revisions:
        geometry = report['table_geometry'][rev.name]
        if geometry['column_segments']<1 or geometry['units']<MIN_FIXTURE_UNITS or geometry['relation_blocks']<256:
            raise ValueError('actual sift_table fixture holds no column part, granule or 256 pages')


def check_anchor_manifest(path):
    try:
        manifest = load_anchor_manifest(path)
    except ValueError as exc:
        print(f'error: {exc}',file=sys.stderr)
        return 3
    print(f"{path}: {len(manifest.rows)} cases of {manifest.meta['anchor_ref']} ({manifest.meta['anchor_sha']}), {manifest.meta['rows']} rows, tier {manifest.meta['tier']}")
    return 0


def main(argv=None):
    global DEADLINE, BATCH_WALL_LIMIT, QUIET, PERF_AVAILABLE, PROFILE, ANCHOR_MANIFEST
    checker = argparse.ArgumentParser(add_help=False,allow_abbrev=False)
    checker.add_argument('--check-anchor-tsv',type=Path,help='validate an anchor manifest and exit; no other argument is needed')
    checked = checker.parse_known_args(argv)[0].check_anchor_tsv
    if checked:
        return check_anchor_manifest(checked)
    args = parser().parse_args(argv)
    if args.anchor_record and not args.anchor_ref:
        parser().error('--anchor-record needs --anchor-ref')
    try:
        ANCHOR_MANIFEST = load_anchor_manifest(args.anchor_tsv) if args.anchor_tsv else None
    except ValueError as exc:
        print(f'error: {exc}',file=sys.stderr)
        return 3
    QUIET = QuietGuard(not args.allow_loaded,args.quiet_max,args.quiet_timeout)
    PERF_AVAILABLE = not args.no_perf and perf_available()
    PROFILE = ANCHOR_MANIFEST is not None
    pr_wall_limit = BATCH_WALL_LIMIT
    if args.tier=='nightly':
        BATCH_WALL_LIMIT = NIGHTLY_BATCH_WALL_LIMIT
    groups_supplied = args.groups is not None
    args.groups = args.groups or ''
    args.output = args.output.resolve()
    args.output.mkdir(parents=True,exist_ok=True)
    report = new_report(args)
    revisions, references = [], []
    scratch = None
    start = time.monotonic()
    DEADLINE = start
    measurement_start = None
    try:
        report['budget'] = budget_context(args.budget_seconds,args.workflow_start_seconds,args.cleanup_report_reserve_seconds)
        DEADLINE = start+report['budget']['measurement_budget_seconds']
        if time.monotonic()>=DEADLINE:
            raise BudgetExpired('workflow budget exhausted before source/build setup')
        if args.insert_feasibility:
            if (groups_supplied or args.verify_control or args.tier!='pr' or args.baseline_tree or args.candidate_tree):
                raise ValueError('insert-feasibility requires PR tier without groups, controls or tree overrides')
            require_equal(args.baseline_ref,args.candidate_ref,'insert-feasibility requires equal exact revisions')
            report.update(mode='insert_feasibility',normal_gate_evaluated=False,acceptance_evaluated=False,
                          coverage_scope='storage_insert_pr_only',
                          coverage=dict(requested_cases=['storage_insert'],omitted_cases=[c.name for c in CASES if c.name!='storage_insert'],
                                        normal_size_states='not_evaluated',pilot_pairs=1,validation_pairs=1,
                                        initial_pairs=6,confirmation_pairs=6,statistical_observations=12,
                                        expected_history_pairs=None,expected_restoration_checkpoints=None,expected_resets=None))
        with run_phase(report,'source_build_fixture_preconditions'):
            exact_sha(args.baseline_ref)
            exact_sha(args.candidate_ref)
            if args.verify_control:
                require_equal(args.baseline_ref,args.candidate_ref,'control requires unchanged revisions')
            if not 56100 <= args.port_base <= 56298-bool(args.anchor_ref):
                raise ValueError('port-base must be in 56100..56298, one lower with an anchor')
            selected = select([g for g in args.groups.split(',') if g],args.tier)
            runs = expand(selected)
            report['required_size_states'] = ['settled_build','churn_vacuum'] + [
                state for lifecycle,state in (('insert_trajectory','mutable_insert_trajectory'),
                                              ('reset_each_operation','mutable_vacuum'))
                if (args.verify_control or any(c.lifecycle==lifecycle and (lifecycle=='insert_trajectory' or c.name=='storage_vacuum_space') for c in selected))]
            if args.insert_feasibility:
                report['required_size_states'] = ['mutable_insert_trajectory']
            record_source_identity(args,report)
            write_report(args.output,report)
            args.scratch_root.mkdir(parents=True,exist_ok=True)
            scratch = Path(tempfile.mkdtemp(prefix='perf-gate-',dir=args.scratch_root))
            for i, (name,sha,tree) in enumerate([('baseline',args.baseline_ref,args.baseline_tree),('candidate',args.candidate_ref,args.candidate_tree)]):
                phase = time.monotonic()
                pg_config = str(Path(args.pg_config).resolve())
                prebuilt = stage_release(sha,args.stage_cache,pg_config,args.build_jobs,args.output/f'{name}-setup.log',Path(tree) if tree else ROOT)[0] if args.stage_cache else None
                rev = Revision(name,sha,tree,scratch,args.output,pg_config,args.port_base+i,args.build_jobs,prebuilt)
                revisions.append(rev)
                report.setdefault('unix_socket_paths',{})[name] = rev.socket_path
            if args.anchor_ref:
                phase = time.monotonic()
                sha,stage,reused = stage_anchor(args.anchor_ref,args.anchor_cache,str(Path(args.pg_config).resolve()),args.build_jobs,args.output/'anchor-setup.log')
                report['anchor'] = dict(ref=args.anchor_ref,sha=sha,stage=str(stage),reused=reused)
                report['runtime_seconds']['anchor_stage'] = time.monotonic()-phase
                anchor = Revision('anchor',sha,None,scratch,args.output,str(Path(args.pg_config).resolve()),args.port_base+2,args.build_jobs,prebuilt=stage,anchor=True)
                references.append(anchor)
                report['unix_socket_paths']['anchor'] = anchor.socket_path
            write_report(args.output,report)
            for rev in revisions+references:
                phase = time.monotonic()
                name = rev.name
                report.setdefault('cluster_config',{})[name] = rev.setup(32768 if args.tier=='nightly' else 8192)
                report.setdefault('setup_phases',{})[name] = rev.timings
                report.setdefault('versions',{})[name] = rev.versions
                report['runtime_seconds'][name+'_build_cluster_fixture'] = time.monotonic()-phase
            report['fixture_checkpoint_barrier'] = {}
            finalize_fixtures(revisions,DEADLINE,report['fixture_checkpoint_barrier'])
            report['runtime_seconds']['fixture_checkpoint_barrier'] = report['fixture_checkpoint_barrier']['runtime_seconds']
            check_fixture(revisions,report)
            for anchor in references:
                report['anchor']['rows'] = check_anchor_fixture(anchor,revisions[1])
            if not args.insert_feasibility:
                report['initial_sizes'] = {r.name:sizes(r.conn) for r in revisions}
                report['size_checks'].append(size_snapshot(revisions,'settled_build'))
        phase = time.monotonic()
        measurement_start = phase
        deadline = DEADLINE
        with run_phase(report,'measurement_validation'):
            if args.insert_feasibility:
                insert = next(c for c in CASES if c.lifecycle=='insert_trajectory')
                write_report(args.output,report)
                measure(revisions,insert,deadline,sink=report['cases'],force_confirmation=True,report=report,output=args.output)
            elif args.verify_control:
                verify_control(revisions,report,args,deadline)
            else:
                for index, case in enumerate(runs):
                    later = sum(c.lifecycle != 'read' for c in runs[index+1:])
                    try:
                        measure(revisions,case,deadline,sink=report['cases'],report=report,output=args.output,reserve=CASE_RESERVE_SECONDS*later)
                    except BudgetExpired:
                        raise
                    except Inconclusive:
                        # a case that does not gate stays inconclusive in the report; the cases after it are still measured
                        if case.gate:
                            raise
                    if references and case.lifecycle=='read':
                        measure_against_anchor(references[0],revisions[1],case,deadline,report,args.output,args.anchor_record)
                    write_report(args.output,report)
                report['size_checks'].append(churn_sizes(revisions))
            if time.monotonic()>=deadline:
                raise BudgetExpired('global runtime budget exhausted after final size checks')
            report['complete'],report['size_complete'] = True,True
            report['runtime_seconds']['measurement_validation'] = time.monotonic()-phase
    except Inconclusive as exc:
        report['budget_exhausted'],report['error'] = isinstance(exc,BudgetExpired),str(exc)
    except Exception as exc:
        report['status'],report['error'] = 'error',f'{type(exc).__name__}: {exc}'
        if getattr(exc,'__notes__',None):
            report['error'] += '; '+'; '.join(exc.__notes__)
    finally:
        if 'fixture_checkpoint_barrier' in report:
            report['runtime_seconds']['fixture_checkpoint_barrier'] = report['fixture_checkpoint_barrier']['runtime_seconds']
        if measurement_start is not None:
            report['runtime_seconds']['measurement_validation'] = time.monotonic()-measurement_start
        phase = time.monotonic()
        with run_phase(report,'final_cluster_teardown') as cleanup_phase:
            stop_revisions(revisions+references,scratch,report,args,cleanup_phase)
        report['runtime_seconds']['cleanup'] = time.monotonic()-phase
        report['runtime_seconds']['total_before_final_reduce_write'] = time.monotonic()-start
        report['terminal_accounting_file']='terminal-accounting.json (includes final reducer/writer; excludes this sidecar write and outer upload)'
        try:
            with run_phase(report,'final_reduce'):
                if args.insert_feasibility:
                    reduce_insert_feasibility(report)
                else:
                    reduce_report(report)
            write_report(args.output,report)
            if args.anchor_record and report['complete'] and report['status']!='error':
                anchor_record(report,args)
        except Exception as exc:
            report.setdefault('terminal_errors',[]).append(f'{type(exc).__name__}: {exc}')
            if report['status'] not in ('error','diagnostic_error'):
                report['status'],report['error']='error',f'final report failed: {exc}'
        end=time.monotonic()
        prior=report.get('budget',{}).get('prior_elapsed_seconds',0)
        if prior+end-start>args.budget_seconds and report['status'] in ('pass','diagnostic_pass'):
            # A late final writer invalidates PASS. One status publication; no sampling retry.
            report.update(status='diagnostic_inconclusive' if args.insert_feasibility else 'inconclusive',
                          complete=False,budget_exhausted=True,error='whole workflow envelope exceeded during final artifacts')
            if args.insert_feasibility: report.update(diagnostic_complete=False,diagnostic_status='inconclusive')
            try:
                write_report(args.output,report)
            except Exception as exc:
                report['status']='error'
                report.setdefault('terminal_errors',[]).append(f'budget status publication failed: {exc}')
            end=time.monotonic()
        write_terminal(report,args,start,end)
    BATCH_WALL_LIMIT = pr_wall_limit
    QUIET = QuietGuard(False)
    PERF_AVAILABLE = PROFILE = False
    ANCHOR_MANIFEST = None
    print(f"{report['status']}: {args.output/'summary.md'}")
    return {'pass':0,'regression':1,'inconclusive':2,'error':3,
            'diagnostic_pass':0,'diagnostic_regression':1,'diagnostic_inconclusive':2,'diagnostic_error':3}[report['status']]
