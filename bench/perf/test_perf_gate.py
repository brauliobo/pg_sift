"""Focused methodology tests: python3 -m unittest discover -s bench/perf -v."""
import importlib.util
from pathlib import Path
import math
import sys
import tempfile
import json
import contextlib
import io
import unittest
from unittest.mock import patch

PATH = Path(__file__).resolve().parents[1]/'steps/perf_gate.py'
spec = importlib.util.spec_from_file_location('perf_gate',PATH)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class Methodology(unittest.TestCase):
    def test_unchanged_passes(self):
        b = gate.interval([(1,1)]*6)
        self.assertEqual(gate.verdict(b),'pass')
        self.assertEqual(b['ratio'],1)

    def test_slowdown_requires_fresh_confirmation(self):
        b = gate.interval([(1,1.25)]*6)
        self.assertEqual(gate.verdict(b),'suspect')
        self.assertEqual(gate.verdict(b,True),'regression')

    def test_unstable_is_inconclusive(self):
        b = gate.interval([(1,x) for x in (0.7,1.5,0.8,1.8,1.1,1.4)])
        self.assertEqual(gate.verdict(b,True),'inconclusive')

    def test_boundary_inclusive_pass_and_strict_failure(self):
        self.assertEqual(gate.verdict({'upper':1.10,'lower':1.10},True),'pass')
        self.assertEqual(gate.verdict({'upper':1.11,'lower':1.10},True),'inconclusive')
        self.assertEqual(gate.verdict({'upper':1.12,'lower':1.10001},True),'regression')

    def test_t_interval(self):
        logs = [-.06,-.04,-.02,.02,.04,.06]
        b = gate.interval([(1,math.exp(x)) for x in logs])
        half = 2.570581835636305 * math.sqrt(sum(x*x for x in logs)/5)/math.sqrt(6)
        self.assertAlmostEqual(b['lower'],math.exp(-half))
        self.assertAlmostEqual(b['upper'],math.exp(half))

    def test_invalid_measurements_fail(self):
        for value in (0,-1,float('nan'),float('inf')):
            with self.assertRaises(ValueError):
                gate.interval([(1,value)]*6)
        with self.assertRaises(ValueError):
            gate.interval([(1,1)]*5)

    def test_supported_sample_counts_use_correct_student_t(self):
        for count, critical in ((6,2.570581835636305),(16,2.131449545559323)):
            logs = [(i-(count-1)/2)/100 for i in range(count)]
            b = gate.interval([(1,math.exp(x)) for x in logs])
            half = critical * math.sqrt(sum(x*x for x in logs)/(count-1))/math.sqrt(count)
            self.assertAlmostEqual(b['lower'],math.exp(-half))
            self.assertAlmostEqual(b['upper'],math.exp(half))
            self.assertEqual(b['degrees_freedom'],count-1)
            self.assertEqual(b['t_critical'],critical)
        for count in (5,7,12,15,17,22):
            with self.assertRaises(ValueError):
                gate.interval([(1,1)]*count)

    def test_fixed_confirmation_policy_and_fresh_samples(self):
        for case in (gate.CASES[0],gate.CASES[13]):
            calls = []
            def sample(conn,case,operations,deadline,delay=0):
                if operations==7:
                    calls.append(conn)
                    # Initial block is a regression; the fresh block alone passes.
                    return 1.2 if len(calls)<=12 and conn=='B' else 1.0
                return 1.0
            class Rev:
                def __init__(self,side): self.conn = side
            with patch.object(gate,'validate_case',return_value={'size_checks':None}), \
                 patch.object(gate,'calibrate',return_value={'operations':7}) as calibration, \
                 patch.object(gate,'prepare'), patch.object(gate,'elapsed',side_effect=sample):
                result = gate.measure([Rev('A'),Rev('B')],case,math.inf)
            count = 6 if case.reset else 16
            self.assertEqual(calibration.call_count,1)
            self.assertEqual(result['status'],'pass')
            self.assertEqual(result['initial']['n'],6)
            self.assertEqual(result['confirmation']['n'],count)
            self.assertEqual(calls[:12],['A','B','B','A']*3)
            self.assertEqual(calls[12:],['A','B','B','A']*(count//2))
            self.assertEqual(result['confirmation']['samples'],[[1,1]]*count)
            self.assertIsNot(result['initial']['samples'],result['confirmation']['samples'])

    def test_partial_sixteen_pair_confirmation_retains_initial_and_raw(self):
        saved = []
        class Rev: conn=None
        # Twelve initial sides, then two fresh sides and one incomplete pair.
        values = [v for _ in range(6) for side in (1,1.2) for v in (1,side)]
        values += [1,1,1,1,gate.BudgetExpired('confirmation exhausted')]
        with patch.object(gate,'validate_case',return_value={'size_checks':None}), \
             patch.object(gate,'calibrate',return_value={'operations':7}), \
             patch.object(gate,'prepare'), patch.object(gate,'elapsed',side_effect=values):
            with self.assertRaises(gate.BudgetExpired):
                gate.measure([Rev(),Rev()],gate.CASES[0],math.inf,sink=saved)
        self.assertEqual(saved[0]['initial']['n'],6)
        self.assertEqual(saved[0]['confirmation']['expected_pairs'],16)
        self.assertEqual(saved[0]['confirmation']['samples'],[[1,1],[None,None]])
        self.assertNotIn('n',saved[0]['confirmation'])
        self.assertEqual(saved[0]['status'],'inconclusive')

    def test_fixture_checkpoint_barrier_is_synchronous_and_timed(self):
        calls = []
        class Rev:
            def __init__(self,name): self.name,self.conn = name,self
            def exec(self,sql): calls.append((self.name,sql))
        saved = {}
        with patch.object(gate.time,'monotonic',side_effect=[0,0,0,1,1,1,1,3,3,3]):
            gate.finalize_fixtures([Rev('A'),Rev('B')],10,saved)
        self.assertEqual(calls,[('A','CHECKPOINT'),('B','CHECKPOINT')])
        self.assertEqual(saved['revisions']['A']['runtime_seconds'],1)
        self.assertEqual(saved['revisions']['B']['runtime_seconds'],2)
        self.assertEqual(saved['runtime_seconds'],3)
        self.assertTrue(all(x['complete'] for x in saved['revisions'].values()))
        self.assertLessEqual(saved['revisions']['A']['finished_at'],saved['revisions']['B']['started_at'])

    def test_checkpoint_expiry_or_failure_stops_later_cluster(self):
        calls = []
        class Rev:
            def __init__(self,name): self.name,self.conn = name,self
            def exec(self,sql): calls.append(self.name)
        saved = {}
        with patch.object(gate.time,'monotonic',side_effect=[0,0,0,2,2,2]):
            with self.assertRaises(gate.BudgetExpired):
                gate.finalize_fixtures([Rev('A'),Rev('B')],1,saved)
        self.assertEqual(calls,['A'])
        self.assertFalse(saved['revisions']['A']['complete'])
        self.assertEqual(saved['runtime_seconds'],2)
        calls.clear()
        with patch.object(gate.time,'monotonic',return_value=2):
            with self.assertRaises(gate.BudgetExpired):
                gate.finalize_fixtures([Rev('A'),Rev('B')],1,{})
        self.assertEqual(calls,[])
        with patch.object(Rev,'exec',side_effect=RuntimeError('checkpoint SQL failed')):
            with self.assertRaisesRegex(RuntimeError,'checkpoint SQL failed'):
                gate.finalize_fixtures([Rev('A'),Rev('B')],math.inf,{})

    def test_both_fixture_loads_and_barriers_precede_read_validation(self):
        for fail in (False,True):
            events = []
            class Revision:
                def __init__(self,name,*args):
                    self.name,self.conn,self.timings,self.versions,self.started = name,self,{},[],False
                def setup(self,rows):
                    events.append('load '+self.name)
                    return ''
                def exec(self,sql):
                    events.append(sql+' '+self.name)
                    if fail: raise RuntimeError('checkpoint failed')
                def rows(self,sql):
                    events.append('read '+self.name)
                    raise RuntimeError('stopped at first validation')
                def stop(self,output): events.append('stop '+self.name)
            with tempfile.TemporaryDirectory() as tmp:
                args = ['--baseline-ref','a'*40,'--candidate-ref','b'*40,
                        '--output',tmp,'--scratch-root',tmp,'--pg-config','/bin/true']
                with patch.object(gate,'Revision',Revision), patch.object(gate,'exact_sha'), \
                     patch.object(gate,'command',return_value='PostgreSQL 18.6'), \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(gate.main(args),3)
                report = json.loads((Path(tmp)/'results.json').read_text())
            expected = ['load baseline','load candidate','CHECKPOINT baseline']
            if not fail: expected += ['CHECKPOINT candidate','read baseline']
            self.assertEqual(events,expected+['stop candidate','stop baseline'])
            self.assertIn('fixture_checkpoint_barrier',report['runtime_seconds'])
            self.assertEqual(report['fixture_checkpoint_barrier']['revisions']['baseline']['complete'],not fail)
            gate.DEADLINE = math.inf

    def test_workflow_elapsed_and_reserve_reduce_driver_budget(self):
        context = gate.budget_context(300,1000,30,now=1044)
        self.assertEqual(context['prior_elapsed_seconds'],44)
        self.assertEqual(context['measurement_budget_seconds'],226)
        self.assertEqual(context['cleanup_report_reserve_seconds'],30)
        self.assertEqual(gate.budget_context(300,1000,30,now=1400)['measurement_budget_seconds'],0)
        self.assertEqual(gate.budget_context(300,None,0,now=1400)['measurement_budget_seconds'],300)
        for budget,started,reserve in ((-1,None,0),(300,float('nan'),30),(300,None,-1)):
            with self.assertRaises(ValueError):
                gate.budget_context(budget,started,reserve,now=1044)

    def test_main_reports_partial_confirmation_as_exit_two_with_all_raw(self):
        class Revision:
            def __init__(self,name,*args):
                self.name,self.conn,self.timings,self.versions,self.started = name,self,{},[],False
            def setup(self,rows): return ''
            def exec(self,sql): pass
            def rows(self,sql): return [['3']]
            def stop(self,output): pass
        values = [v for _ in range(6) for side in (1,1.2) for v in (1,side)]
        values += [1,1,1,1,gate.BudgetExpired('confirmation exhausted')]
        with tempfile.TemporaryDirectory() as tmp:
            args = ['--baseline-ref','a'*40,'--candidate-ref','b'*40,
                    '--output',tmp,'--scratch-root',tmp,'--pg-config','/bin/true']
            with patch.object(gate,'Revision',Revision), patch.object(gate,'exact_sha'), \
                 patch.object(gate,'command',return_value='PostgreSQL 18.6'), \
                 patch.object(gate,'table_geometry',return_value={'column_segments':1,'units':3,'relation_blocks':256}), \
                 patch.object(gate,'sizes',return_value=[]), \
                 patch.object(gate,'size_snapshot',return_value={'status':'pass','relations':[]}), \
                 patch.object(gate,'validate_case',return_value={'size_checks':None}), \
                 patch.object(gate,'calibrate',return_value={'operations':7}), \
                 patch.object(gate,'prepare'), patch.object(gate,'elapsed',side_effect=values), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gate.main(args),2)
            report = json.loads((Path(tmp)/'results.json').read_text())
            summary = (Path(tmp)/'summary.md').read_text()
        self.assertEqual(report['status'],'inconclusive')
        self.assertEqual(report['size_status'],'inconclusive')
        self.assertEqual(report['cases'][0]['initial']['n'],6)
        confirmation = report['cases'][0]['confirmation']
        self.assertEqual(confirmation['expected_pairs'],16)
        self.assertEqual(confirmation['samples'],[[1,1],[None,None]])
        self.assertNotIn('ratio',confirmation)
        self.assertIn('16 read pairs',summary)
        gate.DEADLINE = math.inf

    def test_exhausted_workflow_budget_reports_exit_two_without_setup(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = ['--baseline-ref','a'*40,'--candidate-ref','b'*40,
                    '--output',tmp,'--scratch-root',tmp,'--workflow-start-seconds','1',
                    '--cleanup-report-reserve-seconds','30']
            with patch.object(gate,'Revision') as revision, patch.object(gate,'exact_sha') as identity, \
                 contextlib.redirect_stdout(io.StringIO()):
                code = gate.main(args)
            self.assertEqual(code,2)
            revision.assert_not_called()
            identity.assert_not_called()
            report = json.loads((Path(tmp)/'results.json').read_text())
            self.assertEqual(report['status'],'inconclusive')
            self.assertEqual(report['budget']['measurement_budget_seconds'],0)
            self.assertEqual(report['config']['initial_pairs'],6)
            self.assertEqual(report['config']['read_confirmation_pairs'],16)
            self.assertEqual(report['config']['mutation_confirmation_pairs'],6)
            gate.DEADLINE = math.inf

    def test_correctness_mismatch_fails(self):
        with self.assertRaisesRegex(ValueError,'correctness differs'):
            gate.require_equal([['8192']],[['8191']],'baseline/head')

    def test_three_smoke_on_every_pr_and_groups_add(self):
        self.assertEqual(len(gate.select([],'pr')),3)
        self.assertEqual(len(gate.select(['bm25'],'pr')),8)
        self.assertEqual(len(gate.select([],'nightly')),18)
        self.assertIn('storage_point_lookup',[c.name for c in gate.select([],'pr')])
        with self.assertRaises(ValueError):
            gate.select(['typo'],'pr')

    def test_sql_errors_propagate(self):
        class Broken:
            def exec(self,sql):
                raise RuntimeError('invalid SQL')
        case = gate.CASES[0]
        with self.assertRaisesRegex(RuntimeError,'invalid SQL'):
            gate.elapsed(Broken(),case,1,math.inf)

    def test_budget_exhaustion_is_not_a_sample(self):
        with self.assertRaises(gate.BudgetExpired):
            gate.elapsed(None,gate.CASES[0],1,0)

    def test_control_is_sql_visible(self):
        class Capture:
            def __init__(self):
                self.statements = []
            def exec(self,sql):
                self.statements.append(sql)
        conn = Capture()
        gate.elapsed(conn,gate.CASES[0],1,math.inf,.003)
        self.assertEqual(conn.statements[-1],'SELECT pg_sleep(0.003000000)')

    def test_alternating_independent_pairs(self):
        calls = []
        class Rev:
            def __init__(self,side):
                self.conn = side
        def sample(conn,case,operations,deadline,delay=0):
            calls.append((conn,operations))
            return 1.0
        with patch.object(gate,'elapsed',side_effect=sample), patch.object(gate,'prepare'):
            b = gate.block([Rev('A'),Rev('B')],gate.CASES[0],7,math.inf)
        self.assertEqual([c for c,n in calls if n==7],['A','B','B','A']*3)
        self.assertEqual(len([n for _,n in calls if n==1]),12)
        self.assertEqual(b['n'],6)

    def test_confirmation_is_one_fresh_block(self):
        case = gate.CASES[0]
        initial = dict(gate.interval([(1,1.2)]*6),samples=[(1,1.2)]*6)
        confirm = dict(gate.interval([(1,1.01)]*16),samples=[(1,1.01)]*16)
        with patch.object(gate,'validate_case',return_value={'size_checks':None}), \
             patch.object(gate,'calibrate',return_value={'operations':7}), \
             patch.object(gate,'block',side_effect=[initial,confirm]) as blocks:
            result = gate.measure([],case,math.inf)
        self.assertEqual(blocks.call_count,2)
        self.assertEqual(blocks.call_args.kwargs['pair_count'],16)
        self.assertEqual(result['status'],'pass')
        self.assertNotEqual(result['initial']['samples'],result['confirmation']['samples'])
        self.assertIsNot(result['initial'],result['confirmation'])

    def test_initial_pass_does_not_confirm(self):
        with patch.object(gate,'validate_case',return_value={'size_checks':None}), \
             patch.object(gate,'calibrate',return_value={'operations':7}), \
             patch.object(gate,'block',return_value=gate.interval([(1,1)]*6)) as blocks:
            result = gate.measure([],gate.CASES[0],math.inf)
        self.assertEqual(blocks.call_count,1)
        self.assertNotIn('confirmation',result)

    def test_calibration_caps_total_reset_cost(self):
        class Rev: conn=None
        # SQL needs 1500 operations, but reset takes 100ms per operation.
        with patch.object(gate,'prepare'), patch.object(gate,'elapsed',return_value=.0003), \
             patch.object(gate.time,'monotonic',side_effect=[0,.3,1,1.3]):
            c = gate.calibrate([Rev(),Rev()],gate.CASES[0],math.inf)
        self.assertLessEqual(c['operations'],10)
        self.assertEqual(c['desired_operations'],1500)
        self.assertTrue(c['target_limited_by_reset'])

    def test_each_mutation_resets_before_timing(self):
        calls = []
        class Conn:
            def exec(self,sql): calls.append(sql)
        with patch.object(gate,'reset',side_effect=lambda c,k:calls.append('RESET')):
            gate.elapsed(Conn(),gate.CASES[13],2,math.inf)
        self.assertEqual(calls,['RESET',gate.CASES[13].sql,'RESET',gate.CASES[13].sql])

    def test_structured_plan_rejects_unrelated_marker(self):
        fake = [{'Plan':{'Node Type':'Seq Scan','Filter':'docs_bm25'}}]
        with self.assertRaises(ValueError):
            gate.require_plan(fake,gate.CASES[0])
        real = [{'Plan':{'Node Type':'Index Scan','Index Name':'docs_bm25'}}]
        gate.require_plan(real,gate.CASES[0])

    def test_space_regression_has_verdict(self):
        with patch.object(gate,'validate_case',return_value={'size_checks':{'status':'regression'}}), \
             patch.object(gate,'calibrate',return_value={'operations':1}), \
             patch.object(gate,'block',return_value=gate.interval([(1,1)]*6)):
            result = gate.measure([],gate.CASES[15],math.inf)
        self.assertEqual(result['status'],'regression')
        self.assertEqual(result['space_status'],'regression')

    def test_partial_raw_pairs_survive_budget_exhaustion(self):
        class Rev: conn=None
        saved = {}
        with patch.object(gate,'prepare'), \
             patch.object(gate,'elapsed',side_effect=[1,1,gate.BudgetExpired('done')]):
            with self.assertRaises(gate.BudgetExpired):
                gate.block([Rev(),Rev()],gate.CASES[0],2,math.inf,sink=saved)
        self.assertEqual(saved['samples'],[[1,None]])

    def test_size_threshold_includes_two_page_tolerance(self):
        # Small indexes tolerate two pages, large indexes tolerate ten percent.
        self.assertEqual(gate.size_verdict(8192,24576)['status'],'pass')
        self.assertEqual(gate.size_verdict(8192,32768)['status'],'regression')
        self.assertEqual(gate.size_verdict(819200,901120)['status'],'pass')
        self.assertEqual(gate.size_verdict(819200,909312)['status'],'regression')
        self.assertEqual(gate.size_verdict(8192,8192)['ratio'],1)
        for invalid in (-1,float('nan'),None):
            with self.assertRaises(ValueError):
                gate.size_verdict(8192,invalid)

    def test_report_exit_error_and_budget_inconclusive(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = ['--baseline-ref','a'*40,'--candidate-ref','b'*40,
                    '--output',tmp,'--scratch-root',tmp]
            with patch.object(gate,'exact_sha'), contextlib.redirect_stdout(io.StringIO()):
                code = gate.main(args+['--groups','typo'])
            self.assertEqual(code,3)
            self.assertEqual(json.loads((Path(tmp)/'results.json').read_text())['status'],'error')
            with patch.object(gate,'exact_sha',side_effect=gate.BudgetExpired('global budget')), \
                 contextlib.redirect_stdout(io.StringIO()):
                code = gate.main(args)
            self.assertEqual(code,2)
            self.assertEqual(json.loads((Path(tmp)/'results.json').read_text())['status'],'inconclusive')
            gate.DEADLINE = math.inf

    def test_nightly_adds_workloads(self):
        self.assertEqual(len([c for c in gate.CASES if c.nightly]),2)
        self.assertTrue(all(not c.nightly for c in gate.select(['bm25','agg','storage'],'pr')))

    def test_reset_crossing_deadline_never_sends_sql(self):
        class Conn:
            def exec(self,sql): raise AssertionError('SQL after deadline')
        with patch.object(gate,'reset'), patch.object(gate.time,'monotonic',side_effect=[0,2]):
            with self.assertRaisesRegex(gate.BudgetExpired,'after reset'):
                gate.elapsed(Conn(),gate.CASES[0],1,1)

    def test_sql_crossing_deadline_never_records_valid_sample(self):
        class Conn:
            def exec(self,sql): pass
        with patch.object(gate,'reset'), patch.object(gate.time,'monotonic',side_effect=[0,0,2]):
            with self.assertRaisesRegex(gate.BudgetExpired,'after SQL'):
                gate.elapsed(Conn(),gate.CASES[0],1,1)

    def test_shared_size_comparator_keeps_relations_distinct(self):
        measured = gate.compare_sizes([{'mutable':'8192','mutable_pk':'1048576'},
                                       {'mutable':'32768','mutable_pk':'1048576'}],
                                      'mutable_vacuum',{'mutable':'mutable_table_storage','mutable_pk':'mutable_btree_index'})
        self.assertEqual(measured['status'],'regression')
        self.assertEqual(measured['relations'][0]['status'],'regression')
        self.assertEqual(measured['relations'][1]['status'],'pass')

    def test_valid_plan_change_is_diagnostic_and_still_measured(self):
        first = [{'Plan':{'Node Type':'Index Scan','Index Name':'docs_bm25'}}]
        second = [{'Plan':{'Node Type':'Custom Scan','Custom Plan Provider':'SiftScan','Index':'docs_bm25'}}]
        class Conn:
            def __init__(self,plan): self.plan = plan
            def script(self,sql): pass
            def rows(self,sql):
                return [[json.dumps(self.plan)]] if sql.startswith('EXPLAIN') else [['1']]
        class Rev:
            def __init__(self,plan): self.conn = Conn(plan)
        result = gate.validate_case([Rev(first),Rev(second)],gate.CASES[0])
        self.assertTrue(result['plan_changed'])
        self.assertEqual(result['results'][0],result['results'][1])


if __name__=='__main__':
    unittest.main()
