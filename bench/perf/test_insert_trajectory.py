"""NoDB behavioral contracts for bounded, separately committed insert trajectories."""
import math
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
        self.assertEqual(CASE.workload_version,'committed_append_v2')
        for seed_max in (8192,32768):
            statements = CASE.statements(seed_max,5)
            self.assertEqual(len(statements),5)
            for ordinal,sql in enumerate(statements,1):
                self.assertIn(f'({seed_max+ordinal},1,7,',sql)
                self.assertNotIn(';',sql)
                self.assertTrue(sql.startswith('INSERT INTO mutable VALUES'))
        self.assertTrue(all(c.lifecycle=='reset_each_operation' for c in gate.CASES[14:16]))
        self.assertTrue(all(c.lifecycle=='read' for c in gate.CASES[:13]))

    def test_trajectory_has_two_resets_and_no_inner_reset(self):
        conn = Capture()
        identity = {'seed_count':32768,'seed_max_id':32768,'first_id':32769,'seed_digest':'digest'}
        with patch.object(gate,'reset',side_effect=lambda *args:conn.calls.append('RESET')), \
             patch.object(gate,'insert_footprint',return_value={}), \
             patch.object(gate,'check_insert',return_value={}):
            saved = {}
            gate.insert_visit(conn,CASE,identity,5,math.inf,saved)
        inserts = CASE.statements(32768,5)
        calls = [c for c in conn.calls if c=='RESET' or c.startswith('INSERT')]
        self.assertEqual(calls,['RESET',*inserts,'RESET',*inserts])
        self.assertEqual(len(saved['operation_seconds']),5)
        self.assertGreater(saved['sql_seconds'],0)
        self.assertGreater(saved['loop_wall_seconds'],0)

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

    def test_calibration_uses_both_sides_and_keeps_raw(self):
        class Rev:
            def __init__(self,side): self.conn = side
        calls = []
        def visit(conn,case,identity,n,deadline,sink,**kwargs):
            calls.append((conn,n))
            sink.update(reset_seconds=.2 if conn=='A' else .3,
                        sql_seconds=.0256 if conn=='A' else .0512,
                        loop_wall_seconds=.0256 if conn=='A' else .0512,
                        operation_seconds=[.0001 if conn=='A' else .0002]*n)
        with patch.object(gate,'insert_visit',side_effect=visit):
            result = gate.calibrate_insert([Rev('A'),Rev('B')],CASE,{},math.inf,{})
        self.assertEqual(calls,[('A',256),('B',256)])
        self.assertEqual(result['operations'],1500)
        self.assertEqual(result['wall_cap_operations'],3499)
        self.assertEqual(result['growth_cap_operations'],2048)
        self.assertEqual(len(result['pilots'][0]['operation_seconds']),256)

    def test_unadmissible_cap_is_inconclusive_and_growth_is_bounded(self):
        class Rev: conn=None
        def visit(*args,**kwargs):
            args[5].update(reset_seconds=1.1,sql_seconds=.001,loop_wall_seconds=.001,operation_seconds=[.001/256]*256)
        saved = {}
        with patch.object(gate,'insert_visit',side_effect=visit):
            with self.assertRaises(gate.Inconclusive):
                gate.calibrate_insert([Rev(),Rev()],CASE,{},math.inf,saved)
        self.assertLess(saved['wall_cap_operations'],1)
        self.assertNotIn('operations',saved)

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

    def test_mutation_control_scales_per_operation_and_excludes_reset(self):
        for n in (2,5):
            class Conn(Capture):
                def __init__(self): super().__init__(); self.delayed=0
                def exec(self,sql):
                    super().exec(sql)
                    if sql.startswith('INSERT INTO mutable VALUES'): self.delayed+=1
                def rows(self,sql): return [[str(self.delayed)]]
            conn = Conn()
            with patch.object(gate,'reset',side_effect=lambda c,k:conn.calls.append('RESET')), \
                 patch.object(gate,'insert_footprint',return_value={}), \
                 patch.object(gate,'check_insert',return_value={}):
                saved = {}
                gate.insert_visit(conn,CASE,{'seed_max_id':8192},n,math.inf,saved,control=True)
            self.assertEqual(conn.delayed,2*n)
            self.assertEqual(saved['control_counts'],[0,0,n,n,2*n])
        conn = Capture()
        gate.install_insert_control(conn,8192)
        function = conn.calls[0]
        self.assertIn('IF NEW.id > 8192 THEN',function)
        self.assertIn('pg_sleep(0.001)',function)
        self.assertIn('FOR EACH ROW',conn.calls[1])

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
        class Rev: conn=None
        def visit(conn,case,identity,n,deadline,sink,**kwargs):
            self.assertEqual(n,5)
            candidate = bool(saved['visits'].index(sink))
            sink.update(after={'totals':{'mutable':32768 if candidate else 8192,'mutable_pk':1048576},
                               'forks':{'mutable':{'main':32768 if candidate else 8192,'fsm':0,'vm':0,'init':0}}})
        saved = {}
        with patch.object(gate,'insert_visit',side_effect=visit):
            gate.validate_insert([Rev(),Rev()],CASE,{},5,math.inf,saved)
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
        class Rev:
            def __init__(self,side): self.conn=side
        def visit(conn,case,identity,n,deadline,sink,**kwargs):
            calls.append((conn,n,kwargs.get('control',False)))
            return 1.3 if conn=='B' else 1
        def calibration(*args): args[-1].update(operations=5)
        def validation(*args,**kwargs): args[-1].update(size_checks=None)
        with patch.object(gate,'insert_identity',return_value={'seed_max_id':32768}), \
             patch.object(gate,'calibrate_insert',side_effect=calibration) as pilot, \
             patch.object(gate,'validate_insert',side_effect=validation), \
             patch.object(gate,'insert_visit',side_effect=visit):
            result = gate.measure([Rev('A'),Rev('B')],CASE,math.inf)
        pilot.assert_called_once()
        self.assertEqual(calls,[(side,5,False) for side in ['A','B','B','A']*6])
        self.assertEqual(result['confirmation']['n'],6)
        self.assertEqual(result['timing_status'],'regression')
        self.assertEqual(result['workload_version'],'committed_append_v2')

    def test_growth_cap_and_raw_partial_pilot_are_retained(self):
        class Rev: conn=None
        def visit(*args,**kwargs):
            args[-1].update(reset_seconds=.1,sql_seconds=.00256,loop_wall_seconds=.00256,
                            operation_seconds=[.00001]*256)
        saved = {}
        with patch.object(gate,'insert_visit',side_effect=visit):
            gate.calibrate_insert([Rev(),Rev()],CASE,{},math.inf,saved)
        self.assertEqual(saved['operations'],2048)
        self.assertTrue(saved['limited_by_growth'])
        def partial(*args,**kwargs):
            args[-1].update(operation_seconds=[.001,.002],complete=False)
            raise gate.BudgetExpired('pilot')
        saved = {}
        with patch.object(gate,'insert_visit',side_effect=partial):
            with self.assertRaises(gate.Inconclusive):
                gate.calibrate_insert([Rev(),Rev()],CASE,{},math.inf,saved)
        self.assertEqual(saved['pilots'][0]['operation_seconds'],[.001,.002])
        self.assertNotIn('operations',saved)

    def test_control_budget_includes_all_excluded_warm_delays(self):
        unchanged = {'calibration':{'operations':2048,'pilots':[{'reset_seconds':.2}]*2,
                                   'loop_wall_seconds_per_operation':[.0001,.0002]},
                     'initial':{'visits':[[{'check_seconds':.03},{'check_seconds':.04}]]*6},
                     'phase_runtime_seconds':{'validation':2}}
        with patch.object(gate.time,'monotonic',return_value=10):
            budget = gate.insert_control_budget(unchanged,300)
        self.assertEqual(budget['candidate_delayed_operations'],26*2048)
        self.assertAlmostEqual(budget['minimum_added_sleep_seconds'],53.248)
        self.assertAlmostEqual(budget['predicted_check_seconds'],12*.07)
        self.assertEqual(budget['remaining_seconds'],290)

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
        with patch.object(gate,'insert_visit',side_effect=[1,1,gate.BudgetExpired('partial')]):
            with self.assertRaises(gate.BudgetExpired):
                gate.block([Rev(),Rev()],CASE,5,math.inf,sink=saved,identity={})
        self.assertEqual(saved['samples'],[[1,1],[None,None]])
        self.assertNotIn('n',saved)



if __name__=='__main__': unittest.main()
