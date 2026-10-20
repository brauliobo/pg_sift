"""Release label_a and measured-admission contracts, without PostgreSQL."""
import contextlib
import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from test_restoration_pair import gate, CASE, IDENTITY, Revision
import test_restoration_pair as restoration


class InsertThroughput(unittest.TestCase):
    def test_composition_sums_all_histories_not_ratios_and_complements_schedule(self):
        schedules=[]
        def history(revisions,case,identity,n,deadline,sink,**kw):
            j=len(schedules)%5
            schedules.append(kw['order'])
            values=[j+1,(j+1)**2]
            sink.update(complete=True,samples=values,sides=[{'after':{'forks':{}}}]*2)
            return values
        with patch.object(gate,'insert_pair',side_effect=history):
            for outer in ((0,1),(1,0)):
                saved={}
                self.assertEqual(gate.insert_observation([],CASE,IDENTITY,2,5,math.inf,saved,order=outer),[15,55.])
                self.assertEqual(len(saved['histories']),5)
                self.assertTrue(saved['complete'])
        self.assertEqual(schedules,[(0,1),(1,0),(0,1),(1,0),(0,1),
                                    (1,0),(0,1),(1,0),(0,1),(1,0)])
        def biased(*args,**kwargs):
            values=history(*args,**kwargs)
            return [values[0],1.09*values[0]]
        with patch.object(gate,'insert_pair',side_effect=biased):
            block=gate.block([],CASE,2,math.inf,identity=IDENTITY,histories=5)
        self.assertEqual(block['n'],6)
        self.assertEqual(block['degrees_freedom'],5)
        self.assertAlmostEqual(block['ratio'],1.09)

    def test_middle_history_failure_keeps_partial_raw_but_no_sample_or_ci(self):
        calls=[]
        def history(revisions,case,identity,n,deadline,sink,**kw):
            calls.append(sink)
            sink.update(complete=False,sides=[{'operation_seconds':[.1]}, {'operation_seconds':[]}])
            if len(calls)==3: raise ValueError('middle oracle primary')
            sink.update(complete=True,samples=[1,1])
            return [1,1]
        saved={}
        with patch.object(gate,'insert_pair',side_effect=history):
            with self.assertRaisesRegex(ValueError,'middle oracle primary'):
                gate.block([],CASE,2,math.inf,identity=IDENTITY,histories=5,sink=saved)
        self.assertNotIn('n',saved)
        self.assertEqual(saved['samples'],[[None,None]])
        observation=saved['pairs'][0]
        self.assertFalse(observation['complete'])
        self.assertEqual(len(observation['histories']),3)
        self.assertEqual(observation['histories'][-1]['sides'][0]['operation_seconds'],[.1])

    def test_composed_real_history_owner_keeps_every_oracle_fork_and_stream_order(self):
        events=[]
        revisions=[Revision('A',events),Revision('B',events)]
        def reset(conn,case): events.append((conn.side,'RESET'))
        def footprint(conn):
            events.append((conn.side,'FORKS'))
            return {'forks':{'mutable':{'main':8192}},'totals':{'mutable':8192,'mutable_pk':8192}}
        def oracle(conn,*args): events.append((conn.side,'ORACLE'))
        saved={}
        with patch.object(gate,'reset',side_effect=reset), patch.object(gate,'configure'), patch.object(gate,'insert_footprint',side_effect=footprint), patch.object(gate,'check_insert',side_effect=oracle):
            gate.insert_observation(revisions,CASE,IDENTITY,2,5,math.inf,saved)
        self.assertEqual(sum(sql=='RESET' for _,sql in events),20)
        self.assertEqual(sum(sql=='CHECKPOINT' for _,sql in events),10)
        self.assertEqual(sum(sql=='ORACLE' for _,sql in events),10)
        for h in saved['histories']:
            self.assertTrue(h['complete'])
            self.assertTrue(all('forks' in s['after'] for s in h['sides']))
            ends=[s['phases']['checkpoint']['end_offset_seconds'] for s in h['sides']]
            starts=[s['phases']['stream']['start_offset_seconds'] for s in h['sides']]
            self.assertLessEqual(max(ends),min(starts))
        # The two point streams are consecutive; no probe/configuration between them.
        for i,(side,sql) in enumerate(events):
            if sql=='CHECKPOINT' and i and events[i-1][1]=='CHECKPOINT':
                self.assertTrue(all(s.startswith('INSERT INTO mutable VALUES') for _,s in events[i+1:i+5]))

    def test_real_middle_history_oracle_failure_preserves_all_point_arrays_no_ci(self):
        events=[]
        revisions=[Revision('A',events),Revision('B',events)]
        oracle_calls=[]
        def oracle(conn,identity,n,sink):
            oracle_calls.append(conn.side)
            restoration.PairedRestoration.oracle(conn,identity,n,sink)
            if len(oracle_calls)==6: raise ValueError('third history second oracle primary')
        saved={}
        with patch.object(gate,'reset'), patch.object(gate,'configure'), patch.object(gate,'insert_footprint',return_value=restoration.PairedRestoration.footprint()), patch.object(gate,'check_insert',side_effect=oracle):
            with self.assertRaisesRegex(ValueError,'third history second oracle primary'):
                gate.block(revisions,CASE,2,math.inf,histories=5,identity=IDENTITY,sink=saved)
        self.assertNotIn('n',saved)
        self.assertEqual(saved['samples'],[[None,None]])
        histories=saved['pairs'][0]['histories']
        self.assertEqual(len(histories),3)
        self.assertEqual([h['complete'] for h in histories],[True,True,False])
        self.assertEqual([len(side['operation_seconds']) for h in histories for side in h['sides']],[2]*6)
        self.assertEqual(histories[-1]['sides'][1]['phases']['postcheck']['error']['type'],'ValueError')
        self.assertTrue(all('after' in s for h in histories[:2] for s in h['sides']))

    def test_representative_pilot_freezes_n_k_without_changing_growth_cap(self):
        def pilot(revisions,case,identity,n,deadline,sink,**kwargs):
            self.assertEqual(n,2048)
            sink.update(wall_seconds=1.36,sides=[dict(sql_seconds=v,loop_wall_seconds=v+.001,
                           phases={'final_reset':{'seconds':.19}}) for v in (.035,.033)])
        saved={}
        with patch.object(gate,'insert_pair',side_effect=pilot) as owner:
            gate.calibrate_insert([],CASE,IDENTITY,math.inf,saved)
        self.assertEqual(owner.call_count,1)
        self.assertEqual(saved['operations'],2048)
        self.assertEqual(saved['histories'],5)
        self.assertEqual(saved['history_cap'],5)
        self.assertGreaterEqual(min(saved['predicted_observation_sql_seconds']),.150)
        self.assertEqual(saved['internal_order_policy'],'alternate histories; complement entire schedule on BA observations')
        self.assertAlmostEqual(saved['predicted_observation_wall_seconds'],6.8)

    def test_all_validation_histories_gate_separate_sizes_not_only_last(self):
        def observation(revisions,case,identity,n,k,deadline,sink,**kw):
            sink.update(complete=True,histories=[{'complete':True,'sides':[{'after':{'totals':{'mutable':v,'mutable_pk':8192},'forks':{}}}
                                           for v in (8192,32768 if j==1 else 8192)]} for j in range(k)])
        saved={}
        with patch.object(gate,'insert_observation',side_effect=observation):
            gate.validate_insert([],CASE,IDENTITY,2,math.inf,saved,histories=5)
        self.assertEqual(saved['size_checks']['status'],'regression')
        self.assertEqual(len(saved['history_size_checks']),5)
        self.assertTrue(saved['size_checks']['complete'])
        self.assertEqual(saved['size_checks']['representative_history'],0)

    def test_cpu_control_is_fixed_per_row_not_sleep_and_install_is_inactive(self):
        class Conn:
            def __init__(self): self.sql=[]
            def exec(self,sql): self.sql.append(sql)
        conn=Conn()
        gate.install_insert_control(conn,8192,17,active=False)
        sql='\n'.join(conn.sql)
        self.assertNotIn('pg_sleep',sql)
        self.assertIn('IF NEW.id > 8192 THEN',sql)
        self.assertIn('perf_gate_cpu_work(NEW.id,17)',sql)
        self.assertIn('FOR i IN 1..iterations LOOP',sql)
        self.assertEqual(conn.sql[0],'BEGIN')
        self.assertIn('DISABLE TRIGGER perf_gate_insert_delay',sql)
        self.assertNotIn('ENABLE TRIGGER',sql)
        self.assertEqual(conn.sql[-1],'COMMIT')
        self.assertFalse(any(s.startswith('INSERT') for s in conn.sql))

    def test_measured_ledger_reconciles_nested_work_and_retains_primary_error(self):
        report={}
        clock=[0.]
        with patch.object(gate.time,'monotonic',side_effect=lambda:clock[0]):
            with self.assertRaisesRegex(ValueError,'primary'):
                with gate.run_phase(report,'outer'):
                    clock[0]=1
                    with gate.run_phase(report,'inner'): clock[0]=3
                    clock[0]=4
                    raise ValueError('primary')
            accounting=gate.reconcile_ledger(report,0,5)
        self.assertEqual(accounting['exclusive_phase_seconds'],4)
        self.assertEqual(accounting['residual_seconds'],1)
        self.assertFalse(report['phase_ledger'][0]['complete'])
        self.assertTrue(report['phase_ledger'][1]['complete'])
        self.assertIn('primary',report['phase_ledger'][0]['error'])

    def report(self):
        return dict(status='inconclusive',date='unit',size_status='inconclusive',complete=False,
                    size_complete=False,budget_exhausted=False,baseline_ref='a'*40,candidate_ref='a'*40,
                    cpu='unit',cpus=1,runner='unit',load=[],config={'verify_control':True,'tier':'pr'},
                    runtime_seconds={},size_checks=[],cases=[])

    def inactive_producer(self, failure=None):
        events=[]
        clock=[0.]
        test=self
        class Rev:
            def __init__(self,name):
                self.name,self.conn,self.versions=name,self,[['PG18','3']]
                self.installed=False
                self.restarted=False
            def exec(self,sql):
                events.append((self.name,sql))
                test.assertFalse(sql.startswith('INSERT'))
                if sql.startswith('CREATE FUNCTION perf_gate_cpu_work'):
                    saved=json.loads((output/'results.json').read_text())
                    test.assertFalse(saved['control_setup_intent']['admitted'])
                    test.assertFalse(saved['control_setup_intent']['active_fault'])
                    test.assertNotIn('mutation_control_preflight',saved)
                    if failure=='ddl': raise ValueError('inactive DDL failed')
                    self.installed=True
                if sql.startswith('DROP FUNCTION perf_gate_cpu_work'): self.installed=False
            def rows(self,sql):
                if sql==gate.SEED_IDENTITY: return [['8192','8192','seed']]
                if sql.startswith('SELECT perf_gate_cpu_work('):
                    args=sql.split('(')[1].split(')')[0].split(',')
                    row,iterations=map(int,args)
                    clock[0]+=.000005+iterations*.000001
                    return [[gate.cpu_checksum(row,iterations)]]
                if 'tgenabled' in sql: return [['D']]
                if 'current_setting' in sql: return [['1' if failure=='counter' else '0']]
                if 'pg_proc' in sql or 'pg_trigger' in sql: return [[str(int(self.installed))]]
                if failure=='identity' and self.restarted and 'system_identifier' in sql: return [['changed']]
                return [['identity']]
            def stop(self,output):
                events.append((self.name,'STOP'))
                if failure=='stop': raise ValueError('probe stop failed')
            def start(self):
                events.append((self.name,'START'))
                self.restarted=True
                if failure=='restart': raise ValueError('probe restart failed')
        with tempfile.TemporaryDirectory() as tmp:
            output=Path(tmp)
            report=self.report()
            report['acceptance_catalog_validation']={c.name:{'complete':True,'size_checks':None} for c in gate.CASES if c.lifecycle!='insert_trajectory'}
            report['cases']=[dict(name=name,complete=True,validation={},workload_version='read',phase='complete',timing_status='pass',status='pass') for name in ('control_unchanged','control_sql_slowdown')]
            report['size_checks']=[dict(state=state,status='pass',relations=[]) for state in ('settled_build','churn_vacuum','mutable_vacuum')]
            report['size_control']=dict(state='control_build_segments_64',status='regression',relations=[])
            report['phase_ledger']=[dict(name=name,complete=True,seconds=.1,parent=None,end_offset_seconds=0)
                for name in ('churn_normal_sizes','bm25_growth_logical_forks')]
            error=None
            with patch.object(gate.time,'perf_counter',side_effect=lambda:clock[0]):
                try:
                    gate.prepare_control_costs([Rev('A'),Rev('B')],report,output)
                except Exception as exc:
                    error=exc
        return events,report,error

    def test_real_inactive_producer_writes_intent_then_measures_cpu_drop_and_restart_once(self):
        events,report,error=self.inactive_producer()
        self.assertIsNone(error)
        self.assertEqual(sum(sql=='STOP' for _,sql in events),2)
        self.assertEqual(sum(sql=='START' for _,sql in events),2)
        self.assertEqual(sum(sql.startswith('CREATE FUNCTION perf_gate_cpu_work') for _,sql in events),1)
        self.assertFalse(any('ENABLE TRIGGER' in sql for _,sql in events))
        self.assertTrue(report['control_setup_intent']['complete'])
        self.assertTrue(report['cpu_calibration']['complete'])
        self.assertAlmostEqual(report['cpu_calibration']['seconds_per_iteration'],.000001)
        self.assertEqual(set(report['post_control_costs']),set(gate.CONTROL_COST_PHASES))
        for entry in report['post_control_costs'].values():
            self.assertTrue(entry['complete'])
            self.assertGreater(entry['seconds'],0)
            self.assertEqual(entry['kind'],'same_invocation_measured_estimate')
        frozen=gate.freeze_cpu_work({'sql_seconds_per_operation':[.000017,.000018]},report['cpu_calibration'])
        self.assertGreaterEqual(frozen['iterations'],18)
        self.assertLessEqual(frozen['iterations'],19)  # float roundoff may round a ceil upward
        self.assertEqual(frozen['iteration_cap'] if 'iteration_cap' in frozen else gate.CPU_ITERATION_CAP,1024)

    def test_inactive_ddl_counter_stop_restart_identity_failures_cannot_fund_control(self):
        for failure in ('ddl','counter','stop','restart','identity'):
            with self.subTest(failure=failure):
                events,report,error=self.inactive_producer(failure)
                self.assertIsNotNone(error)
                self.assertFalse(report['control_setup_intent']['complete'])
                self.assertFalse(report['control_setup_intent']['admitted'])
                self.assertNotIn('post_control_costs',report)
                self.assertFalse(any('ENABLE TRIGGER' in sql for _,sql in events))
                self.assertFalse(any(sql.startswith('INSERT') for _,sql in events))
                if failure in ('ddl','counter'): self.assertFalse(any(sql=='STOP' for _,sql in events))

    def test_failed_active_enable_still_removes_committed_infrastructure_preserving_primary(self):
        class Conn:
            def __init__(self): self.calls=[]
            def exec(self,sql):
                self.calls.append(sql)
                if 'ENABLE TRIGGER' in sql: raise ValueError('enable primary')
        conn=Conn()
        report=self.report()
        clean=restoration.PairedRestoration().clean_result()
        report.update(cpu_calibration=restoration.PairedRestoration().cpu(),
                      post_control_costs=restoration.PairedRestoration().post_costs(),completed_control_work=restoration.PairedRestoration().coverage())
        with tempfile.TemporaryDirectory() as tmp, patch.object(gate,'insert_identity',return_value=IDENTITY):
            with self.assertRaisesRegex(ValueError,'enable primary'):
                gate.measure([type('R',(),{'conn':conn})()]*2,CASE,gate.time.monotonic()+1000,True,
                             frozen=clean,sink=report['cases'],report=report,output=Path(tmp))
        self.assertIn('DROP TRIGGER perf_gate_insert_delay ON mutable',conn.calls)
        self.assertEqual(report['cases'][0]['primary_error']['error'],'enable primary')

    def test_composite_cost_population_never_sums_nested_costs_or_uses_pilot(self):
        helper=restoration.PairedRestoration()
        clean=helper.clean_result()
        clean['calibration']['histories']=5
        for role,block in [('validation',clean['validation']),('verdict',clean['initial']),('verdict',clean['confirmation'])]:
            pair=helper.clean_observation(wall=10,role=role,histories=5)
            if role=='validation': block['pair']=pair
            else: block['pairs']=[pair]*6
        cost=gate.insert_control_budget(clean,1000,helper.post_costs(),cpu_work=helper.cpu_work(),sources=('a'*40,'a'*40,'pr'),completed_work=helper.coverage(),reserve_seconds=30)
        self.assertEqual(cost['max_clean_full_N_pair_wall'],10)
        self.assertEqual(len(cost['clean_full_N_population']),13)
        self.assertEqual(cost['predicted_clean_pair_seconds'],130)
        self.assertEqual(cost['candidate_cpu_operations'],26*5*5)
        self.assertEqual(cost['expected_checkpoint_calls'],130)
        self.assertAlmostEqual(cost['predicted_seconds'],130+.65+.2)
        self.assertAlmostEqual(cost['predicted_total_remaining_seconds'],130+.65+.8)
        clean['validation']['pair']['histories'][2]['sides'][1]['phases']['postcheck']['complete']=False
        self.assertFalse(gate.insert_control_budget(clean,1000,helper.post_costs(),cpu_work=helper.cpu_work(),sources=('a'*40,'a'*40,'pr'),completed_work=helper.coverage())['evidence_complete'])

    def test_cpu_calibration_is_one_fixed_bounded_attempt_not_verdict_tuned(self):
        clock=[0.]
        class Conn:
            def rows(self,sql):
                row,iterations=map(int,sql.split('(')[1].split(')')[0].split(','))
                clock[0]+=.00001  # no identifiable CPU slope; cannot substitute zero/fallback
                return [[gate.cpu_checksum(row,iterations)]]
        raw={}
        with patch.object(gate.time,'perf_counter',side_effect=lambda:clock[0]):
            with self.assertRaises(gate.Inconclusive): gate.calibrate_cpu_work(Conn(),raw)
        self.assertFalse(raw['complete'])
        self.assertEqual([len(b['operation_seconds']) for b in raw['blocks']],[32,32])
        with self.assertRaises(gate.Inconclusive): gate.freeze_cpu_work({'sql_seconds_per_operation':[1,1]},raw)

    def main_acceptance(self, *, final_writer_delay=0, stop_error=False, measurement_error=False):
        events=[]
        clock=[100.]
        helper=restoration.PairedRestoration()
        class Rev:
            def __init__(self,name,*args):
                self.name,self.conn,self.timings,self.versions,self.started=name,self,{},[],False
            def setup(self,rows): events.append('setup_'+self.name); return ''
            def exec(self,sql): events.append(sql)
            def script(self,sql): events.append(sql)
            def rows(self,sql): return [['3']]
            def stop(self,output):
                events.append('stop_'+self.name)
                if stop_error: raise gate.BudgetExpired('cleanup expiry')
        def validate(revisions,case):
            events.append('validate_'+case.name)
            return {'size_checks':{'state':'mutable_vacuum','status':'pass','relations':[]} if case.name=='storage_vacuum_space' else None}
        def size(revisions,state):
            events.append(state)
            return dict(state=state,status='regression' if state.startswith('control_') else 'pass',relations=[])
        def churn(revisions):
            events.append('churn')
            return dict(state='churn_vacuum',status='pass',relations=[])
        def producer(revisions,report,output):
            events.append('cost_producer')
            report.update(cpu_calibration=helper.cpu(),post_control_costs=helper.post_costs())
        def measure(revisions,case,deadline,control=False,sink=None,**kwargs):
            events.append(case.name+('_fault' if control else '_clean'))
            if measurement_error and case.lifecycle=='insert_trajectory': raise ValueError('primary oracle')
            result=dict(name=case.name,workload_version=case.workload_version,lifecycle=case.lifecycle,
                        phase='complete',complete=True,status='regression' if control else 'pass',
                        timing_status='regression' if control else 'pass',
                        validation={'size_checks':dict(state='mutable_insert_trajectory',status='pass',relations=[]) if case.lifecycle=='insert_trajectory' else None},
                        confirmation=gate.interval([(1,1.5 if control else 1)]*6))
            sink.append(result)
            return result
        original_write=gate._write_report
        def write(output,report):
            events.append('writer')
            clock[0]+=.001
            original_write(output,report)
            if 'terminal_accounting_file' in report: clock[0]+=final_writer_delay
        with tempfile.TemporaryDirectory() as tmp:
            args=['--verify-control','--baseline-ref','a'*40,'--candidate-ref','a'*40,
                  '--output',tmp,'--scratch-root',tmp,'--pg-config','/bin/true',
                  '--budget-seconds','100','--cleanup-report-reserve-seconds','30']
            with patch.object(gate.time,'monotonic',side_effect=lambda:clock[0]), patch.object(gate,'Revision',Rev), patch.object(gate,'exact_sha'), patch.object(gate,'command',return_value='PostgreSQL 18.6'), patch.object(gate,'table_geometry',return_value={'units':3,'column_segments':1,'relation_blocks':256}), patch.object(gate,'sizes',return_value=[]), patch.object(gate,'size_snapshot',side_effect=size), patch.object(gate,'validate_case',side_effect=validate), patch.object(gate,'churn_sizes',side_effect=churn), patch.object(gate,'prepare_control_costs',side_effect=producer), patch.object(gate,'measure',side_effect=measure), patch.object(gate,'_write_report',side_effect=write), contextlib.redirect_stdout(__import__('io').StringIO()):
                code=gate.main(args)
            gate.DEADLINE=math.inf
            return code,json.loads((Path(tmp)/'results.json').read_text()),json.loads((Path(tmp)/'terminal-accounting.json').read_text()),events

    def test_main_moved_tail_is_completed_once_before_clean_insert_with_no_later_read(self):
        code,report,terminal,events=self.main_acceptance()
        self.assertEqual(code,0)
        at=events.index('churn')
        self.assertEqual(sum(e=='churn' for e in events),1)
        self.assertEqual(sum(e=='control_build_segments_64' for e in events),1)
        self.assertLess(events.index('storage_point_lookup_fault'),at)
        self.assertLess(at,events.index('control_build_segments_64'))
        self.assertLess(events.index('control_build_segments_64'),events.index('cost_producer'))
        self.assertLess(events.index('cost_producer'),events.index('storage_insert_clean'))
        self.assertFalse(any(e.startswith('validate_') or e.startswith('storage_point_lookup') for e in events[at:]))
        self.assertEqual(report['size_status'],'pass')
        self.assertAlmostEqual(terminal['accounting']['residual_seconds'],0)
        self.assertTrue(all(e.get('complete') for e in terminal['phase_ledger']))

    def test_actual_final_writer_time_cannot_return_pass_after_workflow_envelope(self):
        code,report,terminal,events=self.main_acceptance(final_writer_delay=101)
        self.assertEqual(code,2)
        self.assertEqual(report['status'],'inconclusive')
        self.assertFalse(report['complete'])
        self.assertTrue(report['budget_exhausted'])
        self.assertTrue(terminal['workflow_budget_exceeded'])
        self.assertEqual(terminal['status'],report['status'])
        self.assertGreaterEqual(terminal['driver_seconds'],101)
        self.assertAlmostEqual(terminal['accounting']['residual_seconds'],0)

    def test_main_primary_correctness_error_survives_final_stop_expiry(self):
        code,report,terminal,events=self.main_acceptance(stop_error=True,measurement_error=True)
        self.assertEqual(code,3)
        self.assertIn('primary oracle',report['error'])
        self.assertEqual(len(report['cleanup_errors']),2)
        self.assertTrue(all('cleanup expiry' in e for e in report['cleanup_errors']))
        self.assertEqual(terminal['status'],'error')

    def test_cost_estimates_cannot_be_borrowed_from_another_source_or_incomplete_schedule(self):
        helper=restoration.PairedRestoration()
        for flaw in ('source','unfinished_churn','missing_size_control'):
            costs=helper.post_costs()
            coverage=helper.coverage()
            if flaw=='source': costs['fault_setup']['provenance']['candidate']='b'*40
            if flaw=='unfinished_churn': coverage['phases'][0]['complete']=False
            if flaw=='missing_size_control': coverage['size_control_complete']=False
            budget=gate.insert_control_budget(helper.clean_result(),1000,costs,cpu_work=helper.cpu_work(),
                sources=('a'*40,'a'*40,'pr'),completed_work=coverage)
            self.assertFalse(budget['evidence_complete'])
            self.assertIsNone(budget['predicted_seconds'])

    def test_existing_cluster_start_owner_never_reloads_on_restart_and_reapplies_settings(self):
        from unittest.mock import MagicMock
        rev=gate.Revision.__new__(gate.Revision)
        rev.bindir,rev.base,rev.data,rev.socket=map(Path,('bin','owned','owned/data','owned/socket'))
        rev.env,rev.log,rev.port,rev.timings={},Path('owned/setup.log'),56260,{'cluster_init_start':.1}
        sql=[]
        class Conn:
            def __init__(self,*args): pass
            def exec(self,s): sql.append(s)
            def script(self,s): sql.append(s)
            def rows(self,s): return [['8192']] if s=='SHOW block_size' else [['PG18','3']]
        with patch.object(gate,'command') as command, patch.object(gate.cell,'libpq',return_value=MagicMock()), patch.object(gate,'SQL',Conn):
            rev.start(fixture_rows=8192)
            rev.start()
        commands=[list(map(str,call.args[0])) for call in command.call_args_list]
        self.assertEqual(len(commands),3)
        self.assertEqual(commands[0],commands[2])
        self.assertIn('psql',commands[1][0])
        self.assertEqual(sum(any('rows=8192'==a for a in c) for c in commands),1)
        self.assertFalse(any('initdb' in a or a=='make' for c in commands for a in c))
        self.assertEqual(sql,["LOAD 'pg_sift'",gate.SETTINGS]*2)

    def test_missing_or_null_measured_cost_is_incomplete_not_zero_or_error(self):
        helper=restoration.PairedRestoration()
        for value in (None,math.nan,math.inf,-1,0):
            with self.subTest(value=value):
                costs=helper.post_costs()
                costs['fault_removal']['seconds']=value
                budget=gate.insert_control_budget(helper.clean_result(),1000,costs,cpu_work=helper.cpu_work(),
                    sources=('a'*40,'a'*40,'pr'),completed_work=helper.coverage())
                self.assertFalse(budget['evidence_complete'])
                self.assertIsNone(budget['predicted_seconds'])

    def test_incomplete_clean_case_or_block_never_funds_active_control(self):
        helper=restoration.PairedRestoration()
        for flaw in ('case','initial','confirmation'):
            clean=helper.clean_result()
            if flaw=='case': clean['complete']=False
            else: clean[flaw]['pairs']=clean[flaw]['pairs'][:5]
            budget=gate.insert_control_budget(clean,1000,helper.post_costs(),cpu_work=helper.cpu_work(),
                sources=('a'*40,'a'*40,'pr'),completed_work=helper.coverage())
            self.assertFalse(budget['evidence_complete'])
            self.assertIsNone(budget['predicted_seconds'])

    def interrupted_validation(self, *, completed=1, regression_history=None, failure='budget'):
        # Round13 actual-owner reproduction: reset5 interrupts history2.
        helper=restoration.PairedRestoration()
        events,resets,seen=[],[0],{'A':0,'B':0}
        revisions=[Revision('A',events),Revision('B',events)]
        def reset(conn,case):
            resets[0]+=1
            if resets[0]==4*completed+1 and failure!='oracle':
                if failure=='sql': raise RuntimeError('later history SQL failed')
                raise gate.BudgetExpired('later history warm reset expired')
        def footprint(conn):
            seen[conn.side]+=1
            value=helper.footprint()
            if regression_history is not None and conn.side=='B' and seen['B']==2*regression_history:
                value['totals']['mutable']=32768
                value['forks']['mutable']['main']=32768
            return value
        def oracle(conn,identity,n,sink):
            helper.oracle(conn,identity,n,sink)
            if failure=='oracle' and resets[0]>4*completed and conn.side=='A':
                raise ValueError('later history oracle failed')
        saved={}
        with patch.object(gate,'configure'), patch.object(gate,'reset',side_effect=reset), patch.object(gate,'insert_footprint',side_effect=footprint), patch.object(gate,'check_insert',side_effect=oracle):
            try:
                gate.validate_insert(revisions,CASE,IDENTITY,2,math.inf,saved,histories=5)
            except Exception as error:
                primary=error
            else:
                self.fail('later history interruption was not raised')
        first=saved['pair']['histories'][0]
        self.assertTrue(first['complete'])
        self.assertEqual([side['oracle']['multiset_difference'] for side in first['sides']],[0,0])
        self.assertEqual([(len(side['operation_seconds']),len(side['warm_operation_seconds'])) for side in first['sides']],[(2,2)]*2)
        self.assertFalse(saved['complete'])
        self.assertFalse(saved['pair']['complete'])
        self.assertNotIn('samples',saved['pair'])
        self.assertNotIn('n',saved['pair'])
        self.assertEqual(len(saved['pair']['histories']),completed+1)
        self.assertFalse(saved['size_checks']['complete'])
        self.assertEqual(saved['size_checks']['completed_histories'],completed)
        self.assertEqual(saved['size_checks']['expected_histories'],5)
        case=dict(timing_status='inconclusive',validation=saved,operations=2,
                  calibration=dict(histories=5,pilot_pair=helper.clean_pair(2048,role='pilot')))
        report=dict(config={'verify_control':False},status='inconclusive' if isinstance(primary,gate.Inconclusive) else 'error',
                    complete=False,size_complete=False,required_size_states=['mutable_insert_trajectory'],size_checks=[],cases=[case])
        return saved,report,primary

    def test_round13_completed_history_size_regression_survives_later_validation_expiry(self):
        for completed,regression in ((1,1),(2,1),(2,2),(4,1)):
            with self.subTest(completed=completed,regression=regression):
                saved,report,error=self.interrupted_validation(completed=completed,regression_history=regression)
                self.assertIsInstance(error,gate.BudgetExpired)
                self.assertEqual(len(saved['history_size_checks']),completed)
                self.assertEqual(saved['history_size_checks'][regression-1]['status'],'regression')
                self.assertEqual(saved['size_checks']['status'],'regression')
                self.assertEqual(saved['size_checks']['relations'][0]['threshold_bytes'],24576)
                gate.reduce_report(report)
                self.assertEqual((report['status'],report['size_status']),('regression','regression'))
                self.assertFalse(report['complete'])
                self.assertFalse(report['size_complete'])
                diagnostic=dict(report)
                gate.reduce_insert_feasibility(diagnostic)
                self.assertEqual(diagnostic['status'],'diagnostic_regression')
                self.assertEqual(diagnostic['diagnostic_size_status'],'regression')
                self.assertFalse(diagnostic['diagnostic_complete'])

    def test_round13_partial_all_pass_sizes_never_complete_validation_or_gate(self):
        for completed in (1,4):
            with self.subTest(completed=completed):
                saved,report,error=self.interrupted_validation(completed=completed)
                self.assertEqual([h['status'] for h in saved['history_size_checks']],['pass']*completed)
                self.assertEqual(saved['size_checks']['status'],'inconclusive')
                gate.reduce_report(report)
                self.assertEqual((report['status'],report['size_status']),('inconclusive','inconclusive'))
                self.assertFalse(report['size_complete'])
                diagnostic=dict(report)
                gate.reduce_insert_feasibility(diagnostic)
                self.assertEqual(diagnostic['status'],'diagnostic_inconclusive')
                self.assertEqual(diagnostic['diagnostic_size_status'],'inconclusive')
                self.assertFalse(diagnostic['diagnostic_complete'])

    def test_round13_later_sql_or_oracle_error_stays_error_over_completed_size_regression(self):
        for failure in ('sql','oracle'):
            with self.subTest(failure=failure):
                saved,report,error=self.interrupted_validation(regression_history=1,failure=failure)
                self.assertIn('later history',str(error))
                self.assertNotIsInstance(error,gate.Inconclusive)
                self.assertEqual(saved['size_checks']['status'],'regression')
                gate.reduce_report(report)
                self.assertEqual((report['status'],report['size_status']),('error','regression'))
                diagnostic=dict(report)
                gate.reduce_insert_feasibility(diagnostic)
                self.assertEqual(diagnostic['status'],'diagnostic_error')
                self.assertFalse(diagnostic['diagnostic_complete'])

    def test_markdown_timing_table_is_contiguous_in_diagnostic(self):
        code,report,summary,*_=restoration.PairedRestoration().main_feasibility()
        table=summary.split('| Case | Phase | n |',1)[1].split('| storage_insert | initial',1)[0]
        self.assertNotIn('Scope:',table)
        self.assertNotIn('Diagnostic coverage:',table)


if __name__=='__main__': unittest.main()
