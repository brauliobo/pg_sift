"""Static behavioral coverage for the experimental paired INSERT lifecycle."""
import contextlib
import io
import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from test_perf_gate import gate

CASE = gate.CASES[13]
IDENTITY = {'seed_count':8192,'seed_max_id':8192,'first_id':8193,'seed_digest':'seed'}


class Revision:
    def __init__(self, side, events):
        self.name,self.conn,self.side,self.events = side,self,side,events
        self.delays = 0
    def exec(self, sql):
        self.events.append((self.side,sql))
        if sql.startswith('INSERT INTO mutable VALUES'): self.delays += 1
    def script(self, sql): self.events.append((self.side,sql))
    def rows(self, sql):
        self.events.append((self.side,'COUNTER'))
        return [[str(self.delays)]]


class PairedRestoration(unittest.TestCase):
    def test_existing_block_exposes_one_side_postcheck_before_other_finalization(self):
        events = []
        revisions = [Revision('A',events),Revision('B',events)]
        def reset(conn,case): events.append((conn.side,'RESET'))
        def elapsed(conn,*args,**kwargs): events.append((conn.side,'POINTS')); return 1.0
        with patch.object(gate,'reset',side_effect=reset), patch.object(gate,'configure'), \
             patch.object(gate,'elapsed',side_effect=elapsed), \
             patch.object(gate,'insert_footprint',return_value={}), \
             patch.object(gate,'check_insert',side_effect=lambda c,*args:events.append((c.side,'CHECK'))):
            gate.block(revisions,CASE,1,math.inf,identity=IDENTITY)
        self.assertEqual(events[:14],[('A','RESET'),('A','POINTS'),('B','RESET'),('B','POINTS'),
                                      ('A','RESET'),('B','RESET'),('A','CHECKPOINT'),('B','CHECKPOINT'),
                                      ('A','POINTS'),('B','POINTS'),('A','CHECK'),('B','CHECK'),
                                      ('B','RESET'),('B','POINTS')])

    def invoke(self, *, order=(0,1), control=False, failure=None, n=2):
        events = []
        revisions = [Revision('A',events),Revision('B',events)]
        reset_counts = [0,0]
        def reset(conn,case):
            side = revisions.index(conn)
            reset_counts[side] += 1
            events.append((conn.side,'RESET'))
            if failure=='second_final_reset' and side==1 and reset_counts[side]==2:
                raise RuntimeError('second final reset')
        def footprint(conn):
            events.append((conn.side,'FOOTPRINT'))
            if failure=='second_preprobe' and conn.side=='B': raise RuntimeError('second preprobe')
            return {'totals':{'mutable':8192,'mutable_pk':8192},'forks':{},'geometry':{'units':3}}
        def oracle(conn,*args):
            events.append((conn.side,'ORACLE'))
            if (failure=='second_oracle' and conn.side=='B') or (failure=='first_oracle' and conn.side=='A'): raise ValueError('second oracle')
        original_exec = Revision.exec
        def execute(conn,sql):
            original_exec(conn,sql)
            if failure=='second_checkpoint' and conn.side=='B' and sql=='CHECKPOINT':
                raise RuntimeError('checkpoint SQL timeout')
            if failure=='checkpoint_deadline' and conn.side=='B' and sql=='CHECKPOINT':
                raise gate.BudgetExpired('checkpoint deadline')
            if failure=='second_stream' and conn.side=='B' and reset_counts[1]==2 and sql.startswith('INSERT'):
                if len([x for x in events if x[0]=='B' and x[1].startswith('INSERT')])==n+2:
                    raise RuntimeError('second stream')
        saved = {}
        with patch.object(gate,'reset',side_effect=reset), patch.object(gate,'configure'), \
             patch.object(gate,'insert_footprint',side_effect=footprint), \
             patch.object(gate,'check_insert',side_effect=oracle), patch.object(Revision,'exec',new=execute):
            try:
                gate.insert_pair(revisions,CASE,IDENTITY,n,math.inf,saved,order=order,control=control)
            except Exception as exc:
                return saved,events,exc
        return saved,events,None

    def test_exact_pair_order_no_intervening_sql_and_states(self):
        for order in ((0,1),(1,0)):
            with self.subTest(order=order):
                saved,events,error = self.invoke(order=order)
                self.assertIsNone(error)
                first,second = ['AB'[i] for i in order]
                points = CASE.statements(8192,2)
                expected = []
                for side in (first,second): expected += [(side,'RESET'),*((side,p) for p in points)]
                for side in (first,second): expected += [(side,'RESET'),(side,'FOOTPRINT')]
                expected += [(first,'CHECKPOINT'),(second,'CHECKPOINT')]
                for side in (first,second): expected += [(side,p) for p in points]
                for side in (first,second): expected += [(side,'ORACLE'),(side,'FOOTPRINT')]
                self.assertEqual(events,expected)
                self.assertTrue(saved['complete'])
                self.assertEqual(len(saved['samples']),2)
                self.assertEqual(saved['before_observation'],'pre_finalization')
                self.assertEqual(saved['after_observation'],'pair_end')
                for side in saved['sides']:
                    self.assertEqual(len(side['operation_seconds']),2)
                    self.assertGreaterEqual(side['warm_age_seconds'],0)
                    self.assertTrue(side['phases']['checkpoint']['complete'])
                self.assertAlmostEqual(saved['wall_seconds'],saved['phase_seconds']+saved['residual_seconds'])

    def test_second_side_failures_preserve_raw_without_valid_pair(self):
        for phase in ('second_final_reset','second_preprobe','second_checkpoint','checkpoint_deadline','second_stream','first_oracle','second_oracle'):
            with self.subTest(phase=phase):
                saved,events,error = self.invoke(failure=phase)
                self.assertIsNotNone(error)
                self.assertFalse(saved['complete'])
                self.assertNotIn('samples',saved)
                self.assertIn('wall_seconds',saved)
                if phase in ('second_final_reset','second_preprobe','second_checkpoint','checkpoint_deadline'):
                    self.assertFalse(any(s['operation_seconds'] for s in saved['sides']))
                else:
                    self.assertEqual(len(saved['sides'][0]['operation_seconds']),2)
                if phase=='second_oracle':
                    self.assertEqual(len(saved['sides'][1]['operation_seconds']),2)

    def test_control_counters_include_warm_not_seed_and_26n(self):
        saved,events,error = self.invoke(control=True,n=5)
        self.assertIsNone(error)
        self.assertEqual(saved['sides'][1]['control_counts'],[0,0,5,5,10])
        self.assertNotIn('control_counts',saved['sides'][0])
        ready = max(i for i,event in enumerate(events) if event[1]=='CHECKPOINT')
        checked = next(i for i,event in enumerate(events) if event[1]=='ORACLE')
        self.assertEqual(len(events[ready+1:checked]),10)
        self.assertTrue(all(sql.startswith('INSERT INTO mutable VALUES') for side,sql in events[ready+1:checked]))
        self.assertEqual(13*(saved['sides'][1]['control_counts'][-1]),26*5)

    def test_pilot_replacement_2048_once_and_caps(self):
        calls = []
        def pair(revisions,case,identity,n,deadline,sink,**kwargs):
            calls.append((n,kwargs['role']))
            sink.update(complete=True,sides=[dict(sql_seconds=n*c,loop_wall_seconds=n*w,
                        operation_seconds=[c]*n,phases={'final_reset':{'seconds':r}})
                        for c,w,r in ((.0001,.0001,.2),(.0002,.0002,.3))])
        saved = {}
        with patch.object(gate,'insert_pair',side_effect=pair):
            gate.calibrate_insert([],CASE,IDENTITY,math.inf,saved)
        self.assertEqual(calls,[(2048,'pilot')])
        self.assertEqual(saved['operations'],1500)
        self.assertEqual(saved['wall_cap_operations'],3499)
        self.assertEqual(len(saved['pilot_pair']['sides'][0]['operation_seconds']),2048)
        for reset,expected in ((.1,2048),(1.1,None)):
            def bounded(revisions,case,identity,n,deadline,sink,**kwargs):
                sink.update(sides=[dict(sql_seconds=.01,loop_wall_seconds=.01,
                    phases={'final_reset':{'seconds':reset}})]*2)
            saved = {}
            with patch.object(gate,'insert_pair',side_effect=bounded):
                if expected is None:
                    with self.assertRaises(gate.Inconclusive): gate.calibrate_insert([],CASE,IDENTITY,math.inf,saved)
                    self.assertNotIn('operations',saved)
                else:
                    gate.calibrate_insert([],CASE,IDENTITY,math.inf,saved)
                    self.assertEqual(saved['operations'],expected)

    def clean_pair(self, n=5, wall=2, role='validation'):
        offset = 0
        sides = [dict(phases={},operation_seconds=[.001]*n,warm_operation_seconds=[.001]*n) for _ in range(2)]
        for side in sides:
            for name in gate.INSERT_PHASES:
                side['phases'][name] = dict(complete=True,seconds=.1,start_offset_seconds=offset,end_offset_seconds=offset+.1)
                offset += .1
        return dict(complete=True,clean=True,operations=n,role=role,workload_version=CASE.workload_version,
                    sides=sides,wall_seconds=wall,phase_seconds=1.4,residual_seconds=wall-1.4)

    def post_costs(self):
        return {name:dict(complete=True,seconds=.2,provenance='synthetic disjoint phase observation '+name)
                for name in gate.POST_CONTROL_PHASES}

    def clean_result(self):
        return dict(name='unchanged',identity=IDENTITY,calibration={'operations':5,
                    'pilot_operations':2048,'growth_cap_operations':2048,'predicted_wall_limit_seconds':1,
                    'pilot_pair':self.clean_pair(2048,1000,'pilot')},
                    validation={'pair':self.clean_pair(),'size_checks':None},
                    initial={'pairs':[self.clean_pair(wall=3,role='verdict')]},
                    confirmation={'pairs':[self.clean_pair(wall=4,role='verdict')]})

    def test_budget_max_only_complete_clean_full_n_not_pilot_or_nested_sum(self):
        unchanged = self.clean_result()
        unchanged['initial']['pairs'].append({'complete':False,'wall_seconds':999})
        budget = gate.insert_control_budget(unchanged,math.inf,self.post_costs())
        self.assertEqual(budget['max_clean_full_N_pair_wall'],4)
        self.assertEqual(len(budget['clean_full_N_population']),3)
        self.assertEqual(budget['predicted_clean_pair_seconds'],52)
        self.assertAlmostEqual(budget['minimum_added_sleep_seconds'],.13)
        self.assertAlmostEqual(budget['predicted_seconds'],52+.13+1.2)
        self.assertEqual(budget['candidate_delayed_operations'],26*5)
        self.assertEqual(budget['expected_checkpoint_calls'],26)
        # Even an equal-N huge pilot is never a substitute for full-N validation.
        unchanged['calibration']['pilot_pair'] = self.clean_pair(5,1000,'pilot')
        self.assertEqual(gate.insert_control_budget(unchanged,math.inf,self.post_costs())['max_clean_full_N_pair_wall'],4)

    def test_budget_missing_nonfinite_or_incomplete_evidence_is_not_zero(self):
        for mutation in ('validation','phase','nonfinite','version','raw','wrong_n','post'):
            with self.subTest(mutation=mutation):
                unchanged = self.clean_result()
                post = self.post_costs()
                pair = unchanged['validation']['pair']
                if mutation=='validation': del unchanged['validation']['pair']
                if mutation=='phase': del pair['sides'][0]['phases']['checkpoint']
                if mutation=='nonfinite': pair['sides'][1]['phases']['checkpoint']['seconds'] = math.nan
                if mutation=='version': pair['workload_version'] = 'committed_append_v2'
                if mutation=='raw': pair['sides'][0]['operation_seconds'].pop()
                if mutation=='wrong_n': pair['operations'] = 2048
                if mutation=='post': del post['cluster_teardown']
                budget = gate.insert_control_budget(unchanged,100,post)
                self.assertFalse(budget['evidence_complete'])
                self.assertIsNone(budget['predicted_seconds'])
                if mutation=='post': self.assertEqual(budget['missing_post_control_costs'],['cluster_teardown'])
                else: self.assertIsNotNone(budget['clean_evidence_error'])

    def test_persist_then_fresh_admission_and_missing_tail_reject_before_fault(self):
        for remaining,known_tail,expected in ((10,True,gate.BudgetExpired),(99,False,gate.Inconclusive),(99,True,RuntimeError)):
            with self.subTest(remaining=remaining,known_tail=known_tail), tempfile.TemporaryDirectory() as tmp:
                clock = [0.0]
                output = Path(tmp)
                cases = []
                report = dict(status='inconclusive',date='unit',size_status='inconclusive',complete=False,
                              size_complete=False,budget_exhausted=False,baseline_ref='a'*40,candidate_ref='a'*40,
                              cpu='unit',cpus=1,runner='unit',load=[],config={'verify_control':True},runtime_seconds={},
                              size_checks=[],cases=cases,post_control_costs=self.post_costs() if known_tail else {})
                snapshots = []
                original_write = gate.write_report
                def write(output,report):
                    original_write(output,report)
                    snapshots.append(json.loads((output/'results.json').read_text())['mutation_control_preflight'])
                    clock[0] = 100-remaining
                def fault(*args):
                    self.assertEqual(len(snapshots),1)
                    self.assertEqual(snapshots[0]['operations'],5)
                    raise RuntimeError('stopped at actual fault entry')
                with patch.object(gate.time,'monotonic',side_effect=lambda:clock[0]), patch.object(gate,'insert_identity',return_value=IDENTITY), patch.object(gate,'write_report',side_effect=write), patch.object(gate,'install_insert_control',side_effect=fault) as install:
                    with self.assertRaises(expected):
                        gate.measure([Revision('A',[]),Revision('B',[])],CASE,100,True,sink=cases,frozen=self.clean_result(),report=report,output=output)
                self.assertEqual(install.call_count,int(known_tail and remaining==99))
                self.assertEqual(cases[0]['control_admission']['remaining_after_persistence_seconds'],remaining)
                self.assertEqual(report['mutation_control_preflight'],snapshots[0])
                self.assertEqual(snapshots[0]['control_budget']['remaining_seconds'],100)
                # Final existing-writer update preserves the immutable pre-fault record.
                original_write(output,report)
                self.assertEqual(json.loads((output/'results.json').read_text())['mutation_control_preflight'],snapshots[0])

    def test_all_callers_shared_owner_maximum_28_and_54_checkpoints(self):
        for acceptance in (False,True):
            with self.subTest(acceptance=acceptance):
                events = []
                revisions = [Revision('A',events),Revision('B',events)]
                for rev in revisions: rev.controlled=False
                def elapsed(conn,case,n,deadline,delay=0,**kwargs):
                    cost = .002 if conn.side=='B' and (not acceptance or conn.controlled) else .001
                    if kwargs.get('raw') is not None: kwargs['raw'].extend([cost]*n)
                    if conn.controlled: conn.delays+=n
                    return cost*n
                def install(conn,maximum): conn.controlled=True
                footprint = {'totals':{'mutable':8192,'mutable_pk':8192},'forks':{}}
                with patch.object(gate,'insert_identity',return_value=IDENTITY), patch.object(gate,'reset'), patch.object(gate,'configure'), patch.object(gate,'elapsed',side_effect=elapsed), patch.object(gate,'insert_footprint',return_value=footprint), patch.object(gate,'check_insert') as oracle, patch.object(gate,'write_report'), patch.object(gate,'install_insert_control',side_effect=install), patch.object(gate,'insert_pair',wraps=gate.insert_pair) as owner:
                    clean = gate.measure(revisions,CASE,math.inf,force_confirmation=acceptance,report={},output=None)
                    if acceptance:
                        gate.measure(revisions,CASE,math.inf,True,frozen=clean,report={'post_control_costs':self.post_costs()})
                expected = 54 if acceptance else 28
                self.assertEqual(sum(sql=='CHECKPOINT' for side,sql in events),expected)
                self.assertEqual(oracle.call_count,expected)
                self.assertEqual(owner.call_count,expected//2)
                self.assertEqual(sum(call.kwargs.get('role')=='pilot' for call in owner.call_args_list),1)
                self.assertEqual(revisions[1].delays,26*clean['operations'] if acceptance else 0)


    def test_checkpoint_sql_error_versus_success_crossing_deadline(self):
        for sql_error in (False,True):
            clock = [0.0]
            events = []
            class Conn(gate.SQL):
                def __init__(self,name): self.side,self.conn=name,self
                def run(self,raw):
                    events.append((self.side,raw.decode()))
                    if self.side=='B':
                        clock[0]=20
                        if sql_error: raise RuntimeError('checkpoint statement timeout')
            saved = {}
            gate.DEADLINE = 10
            try:
                with patch.object(gate.time,'monotonic',side_effect=lambda:clock[0]), patch.object(gate,'reset'), patch.object(gate,'configure'), patch.object(gate,'elapsed',return_value=1), patch.object(gate,'insert_footprint',return_value={}):
                    with self.assertRaises(RuntimeError if sql_error else gate.BudgetExpired):
                        gate.insert_pair([Conn('A'),Conn('B')],CASE,IDENTITY,2,10,saved)
            finally:
                gate.DEADLINE = math.inf
            self.assertEqual(events,[('A','CHECKPOINT'),('B','CHECKPOINT')])
            self.assertTrue(saved['sides'][0]['phases']['checkpoint']['complete'])
            self.assertFalse(saved['sides'][1]['phases']['checkpoint']['complete'])
            self.assertFalse(any(side['operation_seconds'] for side in saved['sides']))

    def test_between_stream_deadline_preserves_provisional_raw_no_ci(self):
        clock = [0.0]
        events = []
        calls = [0]
        def elapsed(conn,case,n,deadline,**kwargs):
            calls[0]+=1
            kwargs['raw'].extend([.001]*n)
            if calls[0]==3: clock[0]=20  # Both barriers succeeded; first stream returns after expiry.
            return .001*n
        saved = {}
        with patch.object(gate.time,'monotonic',side_effect=lambda:clock[0]), patch.object(gate,'reset'), patch.object(gate,'configure'), patch.object(gate,'elapsed',side_effect=elapsed), patch.object(gate,'insert_footprint',return_value={}):
            with self.assertRaises(gate.BudgetExpired):
                gate.block([Revision('A',events),Revision('B',events)],CASE,2,10,sink=saved,identity=IDENTITY)
        self.assertNotIn('n',saved)
        self.assertEqual(saved['samples'],[[None,None]])
        first = saved['pairs'][0]
        self.assertEqual(len(first['sides'][0]['operation_seconds']),2)
        self.assertEqual(first['sides'][1]['operation_seconds'],[])
        self.assertEqual(events,[('A','CHECKPOINT'),('B','CHECKPOINT')])

    def main_feasibility(self, blocks=None, extra=(), failure=None):
        if blocks is None: blocks = [gate.interval([(1,1)]*6)]*2
        events = []
        seen = []
        class Rev:
            def __init__(self,name,*args):
                self.name,self.conn,self.timings,self.versions,self.started = name,self,{},[],False
            def setup(self,rows): return ''
            def exec(self,sql): events.append(sql)
            def rows(self,sql): return [['3']]
            def stop(self,output):
                if failure=='cleanup': raise RuntimeError('teardown failed')
        def calibration(*args): args[-1].update(operations=5,pilot_pair=self.clean_pair(2048,role='pilot'))
        def validation(*args,**kwargs):
            args[-1].update(pair=self.clean_pair(),size_checks={'state':'mutable_insert_trajectory','status':'pass','relations':[]})
        def block(*args,**kwargs):
            seen.append((args[1].name,kwargs.get('pair_count',6)))
            if failure=='partial' and len(seen)==2:
                kwargs['sink'].update(samples=[[1,None]],expected_pairs=6)
                raise gate.BudgetExpired('fresh incomplete')
            return dict(blocks[len(seen)-1],pairs=[self.clean_pair(role='verdict') for _ in range(6)])
        with tempfile.TemporaryDirectory() as tmp:
            args = ['--insert-feasibility','--baseline-ref','a'*40,'--candidate-ref','a'*40,
                    '--output',tmp,'--scratch-root',tmp,'--pg-config','/bin/true',
                    '--budget-seconds','180','--cleanup-report-reserve-seconds','30',*extra]
            with patch.object(gate,'Revision',side_effect=Rev) as revision, patch.object(gate,'exact_sha'), patch.object(gate,'command',return_value='PostgreSQL 18.6'), patch.object(gate,'table_geometry',return_value={'column_segments':1,'units':3,'relation_blocks':256}), patch.object(gate,'insert_identity',return_value=IDENTITY), patch.object(gate,'calibrate_insert',side_effect=calibration) as pilot, patch.object(gate,'validate_insert',side_effect=validation), patch.object(gate,'block',side_effect=block), patch.object(gate,'validate_case') as other, patch.object(gate,'churn_sizes') as churn, patch.object(gate,'size_snapshot') as broad_size, patch.object(gate,'install_insert_control') as fault, patch.object(gate,'reduce_report') as normal_reducer, contextlib.redirect_stdout(io.StringIO()):
                code = gate.main(args)
                report = json.loads((Path(tmp)/'results.json').read_text())
                summary = (Path(tmp)/'summary.md').read_text()
                gate.DEADLINE = math.inf
                return code,report,summary,seen,dict(other=other.call_count,churn=churn.call_count,
                    size=broad_size.call_count,fault=fault.call_count,normal_reducer=normal_reducer.call_count,
                    pilot=pilot.call_count,revision=revision.call_count)

    def test_feasibility_always_two_independent_blocks_no_other_coverage(self):
        code,report,summary,seen,calls = self.main_feasibility()
        self.assertEqual(code,0)
        self.assertEqual(report['status'],'diagnostic_pass')
        self.assertTrue(report['diagnostic_complete'])
        self.assertFalse(report['complete'])
        self.assertFalse(report['size_complete'])
        self.assertFalse(report['normal_gate_evaluated'])
        self.assertFalse(report['acceptance_evaluated'])
        self.assertNotIn('acceptance_pass',report)
        self.assertEqual(report['coverage_scope'],'storage_insert_pr_only')
        self.assertEqual(seen,[('storage_insert',6),('storage_insert',6)])
        self.assertEqual(calls,dict(other=0,churn=0,size=0,fault=0,normal_reducer=0,pilot=1,revision=2))
        self.assertEqual(report['coverage']['expected_restoration_checkpoints'],28)
        self.assertEqual(report['budget']['measurement_budget_seconds'],150)
        self.assertIn('INSERT feasibility',summary)

    def test_feasibility_initial_imprecision_not_erased_by_fresh_pass(self):
        noisy = gate.interval([(1,x) for x in (.8,1.2,.9,1.3,.9,1.2)])
        passed = gate.interval([(1,1)]*6)
        regression = gate.interval([(1,1.3)]*6)
        for blocks,expected in (([noisy,passed],2),([passed,noisy],2),([passed,regression],1)):
            code,report,summary,seen,calls = self.main_feasibility(blocks)
            self.assertEqual(code,expected)
            self.assertEqual(len(seen),2)
        short = gate.interval([(.039,.039)]*6)
        code,report,*_ = self.main_feasibility([short,short])
        self.assertEqual(code,0)
        self.assertFalse(report['achieved_windows']['initial']['target_attained'])
        self.assertAlmostEqual(report['achieved_windows']['initial']['target_fractions'][0],.26)

    def test_feasibility_rejects_cli_source_overrides_before_build(self):
        for extra in (('--groups',''),('--groups','storage'),('--verify-control',),('--tier','nightly'),
                      ('--baseline-tree','unused'),('--candidate-tree','unused'),('--candidate-ref','b'*40)):
            with self.subTest(extra=extra):
                code,report,summary,seen,calls = self.main_feasibility(extra=extra)
                self.assertEqual(code,3)
                self.assertEqual(calls['revision'],0)
                self.assertEqual(seen,[])

    def test_feasibility_partial_and_cleanup_preserve_nonzero_scope(self):
        for failure,expected in (('partial',2),('cleanup',3)):
            code,report,summary,seen,calls = self.main_feasibility(failure=failure)
            self.assertEqual(code,expected)
            self.assertFalse(report['normal_gate_evaluated'])
            self.assertFalse(report['complete'])
            if failure=='partial':
                self.assertFalse(report['diagnostic_complete'])
                self.assertNotIn('n',report['cases'][0]['confirmation'])
                self.assertEqual(report['cases'][0]['confirmation']['samples'],[[1,None]])



if __name__=='__main__': unittest.main()
