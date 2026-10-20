"""NoDB behavioral contracts for bounded, separately committed insert trajectories."""
import math
import contextlib
import io
import json
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
from test_perf_gate import gate

CASE = gate.CASES[13]


class Capture:
    def __init__(self): self.calls = []
    def exec(self, sql): self.calls.append(sql)
    def script(self, sql): self.calls.append(sql)


class InsertTrajectory(unittest.TestCase):
    def test_legacy_elapsed_cannot_represent_committed_append(self):
        conn = Capture()
        with patch.object(gate,'reset') as restoration:
            gate.elapsed(conn,CASE,2,math.inf,statements=CASE.statements(8192,2))
        self.assertEqual(restoration.call_count,0,'an append stream must not restore inside its point loop')
        self.assertNotEqual(conn.calls[0],conn.calls[1],'point IDs must advance within the stream')

    def test_explicit_lifecycles_and_point_identity(self):
        self.assertEqual(CASE.lifecycle,'insert_trajectory')
        self.assertEqual(CASE.workload_version,'committed_append_settled_composite_v4')
        for seed_max in (8192,32768):
            statements = CASE.statements(seed_max,5)
            self.assertEqual(len(statements),5)
            for ordinal,sql in enumerate(statements,1):
                self.assertIn(f'({seed_max+ordinal},1,7,',sql)
                self.assertNotIn(';',sql)
                self.assertTrue(sql.startswith('INSERT INTO mutable VALUES'))
        self.assertTrue(all(c.lifecycle=='reset_each_operation' for c in gate.CASES[14:16]))
        self.assertTrue(all(c.lifecycle=='read' for c in gate.CASES[:13]))


    def test_full_growth_domain_checked_before_pilot(self):
        class Conn:
            def rows(self,sql): return [['10',str(2**31-100),'digest']]
        class Rev: conn=Conn()
        with self.assertRaisesRegex(ValueError,'integer domain'):
            gate.insert_identity([Rev(),Rev()])

    def test_insert_never_resets_inside_elapsed(self):
        conn = Capture()
        statements = CASE.statements(8192,2)
        with patch.object(gate,'reset') as reset:
            gate.elapsed(conn,CASE,2,math.inf,statements=statements)
        reset.assert_not_called()
        self.assertEqual(conn.calls,statements)



    def test_full_multiset_mismatch_rejects_compensating_corruption(self):
        identity = {'seed_count':8192,'seed_max_id':8192,'first_id':8193,'seed_digest':'seed'}
        for actual_digest,delta in (('wrong payload','2'),('same','2'),('wrong seed','2')):
            class Conn(Capture):
                def rows(self,sql):
                    if 'FROM seed' in sql and sql.startswith('SELECT count(*)'): return [['8192','8192','seed']]
                    self.calls.append(sql)
                    return [['8197','same','8197',actual_digest,delta]]
            conn = Conn()
            with self.assertRaisesRegex(ValueError,'multiset'):
                gate.check_insert(conn,identity,5,{})
            query = next(sql for sql in conn.calls if sql.startswith('WITH expected'))
            self.assertEqual(query.count('EXCEPT ALL'),2)
            self.assertIn('id,g,amount,body,pad',query)
            self.assertIn('generate_series(1,5)',query)
            self.assertIn('8192+i',query)

    def test_mutation_control_is_real_row_trigger_above_seed(self):
        conn = Capture()
        gate.install_insert_control(conn,8192,17)
        sql='\n'.join(conn.calls)
        self.assertIn('IF NEW.id > 8192 THEN',sql)
        self.assertIn('perf_gate_cpu_work(NEW.id,17)',sql)
        self.assertNotIn('pg_sleep',sql)
        self.assertIn('FOR EACH ROW',sql)

    def test_raw_relation_forks_sum_per_relation(self):
        class Conn:
            def rows(self,sql):
                self.query = sql
                return [['mutable','8192','8192','0','0'],['mutable_pk','32768','0','8192','0']]
        conn = Conn()
        forks = gate.relation_forks(conn)
        self.assertEqual(forks['mutable'],{'main':8192,'fsm':8192,'vm':0,'init':0})
        self.assertEqual(gate.fork_totals(forks),{'mutable':16384,'mutable_pk':40960})
        for fork in gate.FORKS: self.assertIn(f"pg_relation_size(c.oid,'{fork}')",conn.query)

    def test_validation_gates_full_n_separate_mutable_relations(self):
        def pair(revisions,case,identity,n,k,deadline,sink,**kwargs):
            self.assertEqual(n,5)
            sink.update(complete=True,histories=[{'complete':True,'sides':[{'after':{'totals':{'mutable':v,'mutable_pk':1048576},'forks':{}}}
                               for v in (8192,32768)]}])
        saved = {}
        with patch.object(gate,'insert_observation',side_effect=pair):
            gate.validate_insert([],CASE,{},5,math.inf,saved)
        sizes = saved['size_checks']
        self.assertEqual(sizes['state'],'mutable_insert_trajectory')
        self.assertEqual(sizes['relations'][0]['status'],'regression')
        self.assertEqual(sizes['relations'][1]['status'],'pass')
        self.assertEqual(len(sizes['forks']),2)

    def test_insert_geometry_precondition_is_actual(self):
        for geometry in ({'units':2,'column_segments':1,'relation_blocks':256},
                         {'units':3,'column_segments':0,'relation_blocks':256},
                         {'units':3,'column_segments':1,'relation_blocks':255}):
            with patch.object(gate,'table_geometry',return_value=geometry):
                with self.assertRaisesRegex(ValueError,'boundary'):
                    gate.insert_footprint(None)

    def test_common_n_frozen_for_initial_and_fresh_six(self):
        calls = []
        def pair(revisions,case,identity,n,deadline,sink,**kwargs):
            calls.append((n,list(kwargs['order'])))
            return [1,1.3]
        def calibration(*args): args[-1].update(operations=5,histories=1)
        def validation(*args,**kwargs): args[-1].update(size_checks=None)
        with patch.object(gate,'insert_identity',return_value={'seed_max_id':32768}), patch.object(gate,'calibrate_insert',side_effect=calibration) as pilot, patch.object(gate,'validate_insert',side_effect=validation), patch.object(gate,'insert_pair',side_effect=pair):
            result = gate.measure([],CASE,math.inf)
        pilot.assert_called_once()
        self.assertEqual(calls,[(5,order) for order in ([0,1],[1,0])]*6)
        self.assertEqual(result['confirmation']['n'],6)
        self.assertEqual(result['timing_status'],'regression')
        self.assertEqual(result['workload_version'],'committed_append_settled_composite_v4')

    def test_seed_identity_mismatch_and_full_allowed_id_range(self):
        class Rev:
            def __init__(self,count,maximum,digest):
                self.conn = self
                self.seed = [[str(count),str(maximum),digest]]
            def rows(self,sql): return self.seed
        with self.assertRaisesRegex(ValueError,'identity'):
            gate.insert_identity([Rev(8192,8192,'A'),Rev(8192,8192,'B')])
        for maximum in (2**31-1-2048,32768):
            identity = gate.insert_identity([Rev(8192,maximum,'A')]*2)
            self.assertEqual(identity['first_id'],maximum+1)

    def test_partial_insert_block_never_has_interval(self):
        class Rev: conn=None
        saved = {}
        with patch.object(gate,'insert_pair',side_effect=[[1,1],gate.BudgetExpired('partial')]):
            with self.assertRaises(gate.BudgetExpired):
                gate.block([Rev(),Rev()],CASE,5,math.inf,sink=saved,identity={})
        self.assertEqual(saved['samples'],[[1,1],[None,None]])
        self.assertNotIn('n',saved)


    def test_oracle_primary_error_survives_expired_aggregate_restore(self):
        identity = {'seed_count':8192,'seed_max_id':8192,'seed_digest':'seed'}
        class Conn(gate.SQL):
            def __init__(self): pass
            def run(self,sql): pass
            def rows(self,sql):
                if sql==gate.SEED_IDENTITY: return [['8192','8192','seed']]
                gate.DEADLINE = -math.inf
                raise RuntimeError('full multiset SQL failed')
        saved = {}
        gate.DEADLINE = math.inf
        try:
            with self.assertRaisesRegex(RuntimeError,'full multiset SQL failed'):
                gate.check_insert(Conn(),identity,5,saved)
            self.assertIn('before SQL operation',saved['cleanup_errors'][0]['error'])
        finally:
            gate.DEADLINE = math.inf

    def test_control_primary_error_survives_expired_trigger_removal(self):
        class Conn(gate.SQL):
            def __init__(self): pass
            def run(self,sql): pass
        class Rev: conn=Conn()
        identity = {'seed_count':8192,'seed_max_id':8192,'seed_digest':'seed'}
        frozen = {'name':'clean','identity':identity,'calibration':{'operations':5,'histories':1},'validation':{}}
        saved = []
        def invalid(*args,**kwargs):
            gate.DEADLINE = -math.inf
            raise ValueError('original correctness error')
        gate.DEADLINE = math.inf
        try:
            with patch.object(gate,'insert_identity',return_value=identity), \
                 patch.object(gate,'freeze_cpu_work',return_value={'iterations':1}), patch.object(gate,'insert_control_budget',return_value={'predicted_seconds':1,'predicted_total_remaining_seconds':1,'reserve_seconds':0,'remaining_seconds':100,'evidence_complete':True}), \
                 patch.object(gate,'validate_insert',side_effect=invalid), patch.object(gate,'write_report'):
                with self.assertRaisesRegex(ValueError,'original correctness error'):
                    gate.measure([Rev(),Rev()],CASE,math.inf,True,sink=saved,frozen=frozen,report={'cpu_calibration':{},'baseline_ref':'a'*40,'candidate_ref':'a'*40,'config':{'tier':'pr'}})
            self.assertIn('before SQL operation',saved[0]['cleanup_errors'][0]['error'])
            self.assertIn('runtime_seconds',saved[0])
        finally:
            gate.DEADLINE = math.inf


    def main_cleanup_scenario(self, mode):
        identity = {'seed_count':8192,'seed_max_id':8192,'seed_digest':'seed','first_id':8193}
        calibration = {'operations':5,'histories':1,'pilot_operations':2048,'growth_cap_operations':2048,
                       'predicted_wall_limit_seconds':1,'predicted_reset_loop_seconds':[.105,.105],
                       'loop_wall_seconds_per_operation':[.001,.001],
                       'pilot_pair':{'sides':[{'operation_seconds':[.001]*2048},{'operation_seconds':[.002]*2048}]}}
        observed = []
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            class Conn(gate.SQL):
                def __init__(self): self.control=False
                def run(self,raw):
                    sql = raw.decode()
                    if sql.startswith('CREATE FUNCTION perf_gate_cpu_work'):
                        path = output/'results.json'
                        # Inspect the actual persisted report at the first fault statement.
                        assert path.exists(), 'pre-fault results.json missing'
                        snapshot = json.loads(path.read_text())
                        assert 'mutation_control_preflight' in snapshot, 'preflight evidence missing'
                        observed.append(snapshot['mutation_control_preflight'])
                        assert snapshot['cases'][-1]['phase']=='control_preflight'
                        if mode=='setup_error': raise RuntimeError('trigger setup SQL failed')
                        if mode=='setup_budget': raise gate.BudgetExpired('fault setup interrupted')
                        self.control=True
                    if sql.startswith('DROP TRIGGER') and mode in ('cleanup','budget_cleanup_error'):
                        raise RuntimeError('trigger removal SQL failed')
                def rows(self,sql):
                    if sql==gate.SEED_IDENTITY: return [['8192','8192','seed']]
                    if sql.startswith('WITH expected'):
                        gate.DEADLINE = -math.inf
                        if mode=='oracle_sql': raise RuntimeError('full multiset SQL failed')
                        return [['8197','expected','8197','corrupt','2']]
                    return [['3']]
            class Revision:
                def __init__(self,name,*args):
                    self.name,self.conn,self.timings,self.versions,self.started = name,Conn(),{},[],False
                def setup(self,rows): return ''
                def stop(self,output): pass
            def calibrate(*args): args[-1].update(calibration)
            def validate(revisions,case,identity,n,deadline,sink,**kwargs):
                sink.update(visits=[{'oracle':{}}],size_checks=None)
                if mode.startswith('oracle_'):
                    gate.check_insert(revisions[0].conn,identity,n,sink['visits'][0]['oracle'])
                if kwargs.get('control'):
                    if mode=='control_error':
                        gate.DEADLINE = -math.inf
                        raise ValueError('original correctness error')
                    if mode=='budget':
                        gate.DEADLINE = -math.inf
                        raise gate.BudgetExpired('primary measurement budget')
                    if mode=='budget_cleanup_error':
                        raise gate.BudgetExpired('primary measurement budget')
                sink['size_checks'] = {'state':'mutable_insert_trajectory','status':'pass','relations':[]}
            def block(*args,**kwargs):
                count = kwargs.get('pair_count',6)
                return dict(gate.interval([(1,1.3 if args[4] else 1)]*count),
                            visits=[[{'check_seconds':.001},{'check_seconds':.001}]]*count)
            args = ['--baseline-ref','a'*40,'--candidate-ref','a'*40,'--verify-control',
                    '--output',tmp,'--scratch-root',tmp,'--pg-config','/bin/true']
            with patch.object(gate,'Revision',Revision), patch.object(gate,'refresh_report_cost'), patch.object(gate,'prepare_control_costs',side_effect=lambda revs,report,output:report.update(cpu_calibration={})), patch.object(gate,'churn_sizes',return_value={'state':'churn_vacuum','status':'pass','relations':[]}), patch.object(gate,'exact_sha'), \
                 patch.object(gate,'command',return_value='PostgreSQL 18.6'), \
                 patch.object(gate,'table_geometry',return_value={'column_segments':1,'units':3,'relation_blocks':256}), \
                 patch.object(gate,'sizes',return_value=[]), \
                 patch.object(gate,'size_snapshot',return_value={'state':'settled_build','status':'pass','relations':[]}), \
                 patch.object(gate,'validate_case',return_value={'size_checks':None}), \
                 patch.object(gate,'calibrate',return_value={'operations':5}), patch.object(gate,'block',side_effect=block), \
                 patch.object(gate,'insert_identity',return_value=identity), \
                 patch.object(gate,'calibrate_insert',side_effect=calibrate), patch.object(gate,'validate_insert',side_effect=validate), \
                 patch.object(gate,'freeze_cpu_work',return_value={'iterations':1}), patch.object(gate,'insert_control_budget',return_value={'predicted_seconds':1,'predicted_total_remaining_seconds':1,'reserve_seconds':0,'remaining_seconds':100,'evidence_complete':True,'candidate_delayed_operations':130,'cost_scope':{'startup':'already elapsed'}}), \
                 contextlib.redirect_stdout(io.StringIO()):
                try:
                    code = gate.main(args)
                finally:
                    gate.DEADLINE = math.inf
            return code,json.loads((output/'results.json').read_text()),(output/'summary.md').read_text(),observed,calibration

    def test_prefault_snapshot_is_written_and_survives_fault_setup_failure(self):
        for mode,expected in (('setup_error',3),('setup_budget',2)):
            with self.subTest(mode=mode):
                code,report,summary,observed,calibration = self.main_cleanup_scenario(mode)
                self.assertEqual(code,expected)
                self.assertEqual(len(observed),1)
                record = report['mutation_control_preflight']
                self.assertEqual(record,observed[0])
                self.assertEqual(record['calibration'],calibration)
                self.assertEqual([len(p['operation_seconds']) for p in record['calibration']['pilot_pair']['sides']],[2048,2048])
                self.assertEqual(record['calibration']['growth_cap_operations'],2048)
                self.assertEqual(record['calibration']['predicted_wall_limit_seconds'],1)
                self.assertFalse(record['fault_installed'])
                self.assertIn('already elapsed',record['control_budget']['cost_scope']['startup'])
                self.assertEqual(record['operations'],5)
                self.assertEqual(record['control_budget']['candidate_delayed_operations'],130)
                self.assertIn('Mutation control preflight',summary)
                self.assertIn('runtime_seconds',report['cases'][-1])

    def test_actual_main_preserves_primary_and_cleanup_error_precedence(self):
        for mode,expected,primary in (
            ('oracle_sql',3,'full multiset SQL failed'),
            ('oracle_correctness',3,'INSERT full multiset'),
            ('control_error',3,'original correctness error'),
            ('budget',2,'primary measurement budget'),
            ('cleanup',3,'trigger removal SQL failed'),
            ('budget_cleanup_error',3,'trigger removal SQL failed')):
            with self.subTest(mode=mode):
                code,report,summary,observed,calibration = self.main_cleanup_scenario(mode)
                self.assertEqual(code,expected)
                self.assertIn(primary,report['error'])
                self.assertIn(primary,summary)
                case = report['cases'][-1]
                saved = case['validation']['visits'][0]['oracle'] if mode.startswith('oracle_') else case
                self.assertTrue(saved['cleanup_errors'])
                self.assertIn('runtime_seconds',case)
                if mode=='budget_cleanup_error':
                    self.assertIn('primary measurement budget',saved['primary_error']['error'])



if __name__=='__main__': unittest.main()
