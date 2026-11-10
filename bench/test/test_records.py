"""The records library (bench/steps/records): the schema round-trips, the checker accepts a clean set and refuses each way of mixing what must not be mixed, the report is generated
from records. A mutant is the clean set with one field changed; each must be refused with its reason."""
import copy
import json
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # cluster.py: `python3 -I` leaves the script's directory out
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps', 'records'))
import check  # noqa: E402
import claims  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)
import collect  # noqa: E402
import report  # noqa: E402
import schema  # noqa: E402

SHAPES = {'sift': 'Custom Scan SiftAgg t', 'btree': 'Index Scan t_ix', 'heap': 'Seq Scan t'}


def shape(structure):
    """the plan columns of a read whose plan names STRUCTURE"""
    return dict(plan_shape=SHAPES[structure], plan_hash=collect.plan_hash(SHAPES[structure]))


BASE = dict(run_id='abc123456789-20261101T000000Z-0001', tip='abc123456789', harness='test records-1', dataset='orders', variant='main', rows=300000, keys=3000, rows_per_key=100.0, heap_order='arrival',
            heap_corr=0.01, table_state='fresh_vacuumed', allvisible_pct=100.0, structures='btree,heap,sift', statement='top20', sql_hash='aaaaaaaaaaaa', kind='read', protocol='single', mode='forced',
            arm='sift', arm_option='default', picked=1, structure='sift', temp='warm', rep=1, est_cost=100.0, plan_ms=0.1, exec_ms=1.0, wall_ms=1.3, instr_m=5.0, cycles_m=4.0,
            instr_scope='cluster', workers_requested=0, workers_launched=0, heap_hit=10, heap_read=0, btree_hit=0, btree_read=0, gin_hit=0, gin_read=0, sift_hit=30, sift_read=0,
            other_hit=0, other_read=0, os_cache_dropped=None, wal_bytes=0, fpw=1, cpus=4.0, psi_cpu_start=1.0, psi_cpu_end=2.0, psi_io_start=0.0, psi_io_end=0.0, shared_buffers_mb=256.0,
            work_mem_mb=64.0, pg_version='18.1', settings='', settings_hash='da39a3ee5e', maintenance='default', **shape('sift'))

CHURN = 'upd=5;ins=0;del=0;rounds=1'


def clean():
    rs = []
    for arm, option, structure, picked, mode, ms in (('sift', 'default', 'sift', 1, 'forced', 1.0), ('btree', 'default', 'btree', 0, 'forced', 2.0), ('heap', 'default', 'heap', 0, 'forced', 40.0),
                                                     ('sift', 'default', 'sift', 1, 'default', 1.0)):
        for temp in ('warm', 'cold'):
            for rep in (1, 2, 3):
                r = dict(BASE, arm=arm, arm_option=option, structure=structure, picked=picked, mode=mode, temp=temp, rep=rep, exec_ms=ms, wall_ms=ms * 1.3, _file='t.tsv', _line=len(rs) + 2, **shape(structure))
                if temp == 'cold':
                    r.update(os_cache_dropped=0, sift_read=30 if arm == 'sift' else 0, btree_read=4 if arm == 'btree' else 0, heap_read=2000 if arm == 'heap' else 0)
                rs.append(r)
    return rs


def build_rows(**kw):
    """the three warm repetitions of a build of a sift index: the leader and two workers counted apart, three segments, none declared"""
    rows = []
    for rep in (1, 2, 3):
        r = dict(BASE, kind='build', mode='forced', picked=0, arm='sift', arm_option='default', structure='sift', temp='warm', rep=rep, plan_ms=None, exec_ms=None, structures='heap,sift', statement='build',
                 index_bytes=1000, segments=3, immutable_segments=3, buffer_segments=0, index_options='default', instr_m=10.0, instr_leader_m=4.0, instr_workers_m=6.0, workers_requested=2,
                 workers_launched=2, wal_bytes=5, _file='t.tsv', _line=rep + 1)
        r.update(kw)
        rows.append(r)
    return rows


def mutate(**kw):
    rs = clean()
    for k, v in kw.items():
        rs[-1][k] = v
    return rs


def problems(rs):
    return check.check(rs)


class Schema(unittest.TestCase):
    def test_a_file_round_trips(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'r.tsv')
            schema.write(p, [{k: v for k, v in r.items() if not k.startswith('_')} for r in clean()])
            back = schema.read(p)
        self.assertEqual(len(back), len(clean()))
        self.assertEqual(back[0]['rows'], 300000)
        self.assertIsNone(back[0]['os_cache_dropped'])
        self.assertEqual(problems(back), [])

    def test_a_file_with_another_header_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'r.tsv')
            with open(p, 'w') as f:
                f.write('case\tarm\tmetric\n')
            with self.assertRaisesRegex(ValueError, 'none of the 7 schema versions'):
                schema.read(p)

    def test_the_statement_hash_ignores_white_space(self):
        self.assertEqual(schema.sql_hash('SELECT  1\n FROM t;'), schema.sql_hash('select 1 from t'.replace('select', 'SELECT').replace('from', 'FROM')))
        self.assertNotEqual(schema.sql_hash('SELECT 1'), schema.sql_hash('SELECT 2'))


PLAN = {'Node Type': 'Limit', 'Total Cost': 50.0, 'Plans': [{'Node Type': 'Index Scan', 'Index Name': 'ix', 'Relation Name': 't'}]}
AM = {'ix': 'btree', 'gx': 'gin', 'sx': 'sift_col'}


class Collect(unittest.TestCase):
    def test_churn_is_applied_the_same_way_and_returns_its_history(self):
        seen = []

        class Recorder:
            def run(self, sql):
                seen.append(sql)
                return (1.0, [], '')
        history = collect.apply_churn(Recorder(), 't', 'v', upd=5, dele=2, ins=1, rounds=2, insert_sql='INSERT INTO t SELECT * FROM src WHERE random() < {pct} / 100.0')
        self.assertEqual(history, 'upd=5;ins=1;del=2;rounds=2')
        self.assertEqual(len(seen), 6)
        self.assertEqual(seen[0], 'UPDATE t SET v = v WHERE random() < 0.05')
        self.assertEqual(seen[1], 'DELETE FROM t WHERE random() < 0.02')
        self.assertIn('< 1 / 100.0', seen[2])
        with self.assertRaisesRegex(ValueError, 'insert_sql'):
            collect.apply_churn(Recorder(), 't', 'v', ins=1)

    def test_an_arm_is_forced_by_the_first_settings_whose_plan_is_its_structure_else_it_is_unavailable(self):
        class Planner:
            def __init__(self, plans):
                self.on, self.plans = set(), plans

            def run(self, sql):
                if sql.startswith('SET'):
                    self.on.add(sql)
                elif sql == 'RESET ALL':
                    self.on.clear()
                elif sql.startswith('EXPLAIN'):
                    return (1, [json.dumps([{'Plan': self.plans(self.on)}])], '')
                return (1, [], '')
        sift = {'Node Type': 'Custom Scan', 'Custom Plan Provider': 'SiftAgg'}
        index = {'Node Type': 'Index Scan', 'Index Name': 'ix'}
        seq = {'Node Type': 'Seq Scan'}
        # the sift plan is lost once the sequential scan is off: the arm is the first try, no settings
        s = Planner(lambda on: index if 'SET enable_seqscan = off' in on else sift)
        arm, named = collect.forceable(s, collect.Arm('sift'), 'SELECT 1', (AM, None, {}), [])
        self.assertEqual((arm.settings, named), ([], None))
        # only with the sequential scan off does the plan name sift
        s = Planner(lambda on: sift if 'SET enable_seqscan = off' in on else seq)
        arm, _ = collect.forceable(s, collect.Arm('sift'), 'SELECT 1', (AM, None, {}), [])
        self.assertEqual(arm.settings, collect.FORCING['sift'])
        # no way: unavailable, with what the plans named
        s = Planner(lambda on: index)
        arm, named = collect.forceable(s, collect.Arm('sift', option='o'), 'SELECT 1', (AM, None, {}), [])
        self.assertEqual((arm, named), (None, ('sift', 'o', 'btree')))

    def test_an_arm_whose_executed_plan_is_not_its_structure_is_unavailable(self):
        # the plain EXPLAIN names the sift node; when it runs it falls back to a scan of the heap
        sift = {'Node Type': 'Custom Scan', 'Custom Plan Provider': 'SiftAgg'}
        ran = dict(sift, Plans=[{'Node Type': 'Seq Scan', 'Relation Name': 't'}])

        class Planner:
            def run(self, sql):
                if sql.startswith('EXPLAIN (ANALYZE'):
                    return (1, [json.dumps([{'Plan': ran}])], '')
                if sql.startswith('EXPLAIN'):
                    return (1, [json.dumps([{'Plan': sift}])], '')
                return (1.0, [], '')
        s, catalog = Planner(), (AM, 't', {})
        arm, _ = collect.forceable(s, collect.Arm('sift'), 'SELECT 1', catalog, [])
        self.assertEqual(arm.arm, 'sift')
        subject = collect.Subject('t', 1, 'arrival', 'fresh_vacuumed', 't')
        with self.assertRaises(collect.NotForced) as caught:
            collect.arm_rows(s, 'SELECT 1', subject, None, {}, 'forced', arm, 1, 1, catalog, None, None, ('warm',))
        self.assertEqual(caught.exception.structure, 'mixed')

    def changed_plan_rows(self, first, final, mode):
        plans = {'sift': {'Node Type': 'Custom Scan', 'Custom Plan Provider': 'SiftAgg'},
                 'btree': {'Node Type': 'Index Scan', 'Index Name': 'ix'}, 'heap': {'Node Type': 'Seq Scan'}}

        class Planner:
            def __init__(self):
                self.plans = iter((first, final))

            def run(self, sql):
                if sql.startswith('EXPLAIN'):
                    return (1.0, [json.dumps([{'Plan': plans[next(self.plans)]}])], '')
                return (1.0, [], '')

        def sizes(s, table, structure):
            return (100, {'sift': 1000, 'btree': 200, 'heap': None}[structure])
        facts = dict(segments=2, immutable_segments=1, buffer_segments=1, index_options='default')
        env = collect.Env('abc123456789', 'test', BASE['run_id'], 1, reps=1)
        with patch.object(collect, 'pressure', return_value=(0., 0.)), patch.object(collect, 'statio', return_value={}), \
             patch.object(collect, 'wal_lsn', return_value=0), patch.object(collect, 'structure_bytes', side_effect=sizes), \
             patch.object(collect, 'index_facts', return_value=facts), patch.object(collect, 'count_instructions', return_value=(1., 1.)):
            return collect.arm_rows(Planner(), 'SELECT 1', collect.Subject('t', 1, 'arrival', 'fresh_vacuumed', 't'), env, {}, mode,
                                    collect.Arm('sift'), 1, 1, (AM, None, {}), None, None, ('warm',))[0]

    def test_read_inventory_follows_the_executed_plan(self):
        for first, final, size, segments in [('btree', 'sift', 1000, 2), ('sift', 'btree', 200, None)]:
            with self.subTest(first=first, final=final):
                row = self.changed_plan_rows(first, final, 'default')
                self.assertEqual((row['structure'], row['index_bytes'], row.get('segments')), (final, size, segments))

    def test_a_forced_arm_cannot_change_structure_during_repetitions(self):
        with self.assertRaises(collect.NotForced) as caught:
            self.changed_plan_rows('sift', 'heap', 'forced')
        self.assertEqual(caught.exception.structure, 'heap')

    def test_only_the_scans_of_the_subject_table_name_the_structure(self):
        lateral = {'Node Type': 'Nested Loop', 'Plans': [{'Node Type': 'Seq Scan', 'Relation Name': 'probe'},
                                                         {'Node Type': 'Index Scan', 'Index Name': 'ix', 'Relation Name': 'events'}]}
        self.assertEqual(collect.plan_structure(lateral, AM), 'mixed')
        self.assertEqual(collect.plan_structure(lateral, AM, table='public.events'), 'btree')
        self.assertEqual(collect.plan_structure(lateral, AM, table='probe'), 'heap')
        bitmap = {'Node Type': 'Nested Loop', 'Plans': [{'Node Type': 'Seq Scan', 'Relation Name': 'probe'}, {'Node Type': 'Bitmap Index Scan', 'Index Name': 'gx'}]}
        self.assertEqual(collect.plan_structure(bitmap, AM, table='events', index_table={'gx': 'events'}), 'gin')
        parts = {'Node Type': 'Append', 'Plans': [{'Node Type': 'Seq Scan', 'Relation Name': 'p1'}, {'Node Type': 'Seq Scan', 'Relation Name': 'p2'}]}
        self.assertEqual(collect.plan_structure(parts, AM, table='parent'), 'heap')   # no scan of the table itself: the plan as a whole

    def test_the_scans_of_the_partitions_of_the_subject_name_its_structure(self):
        leaf = lambda n: {'Node Type': 'Index Only Scan', 'Relation Name': 'lp_ev_%02d' % n, 'Index Name': 'lp_ev_%02d_ix' % n}   # noqa: E731
        lateral = {'Node Type': 'Nested Loop', 'Plans': [{'Node Type': 'Seq Scan', 'Relation Name': 'lp_dim'},
                                                         {'Node Type': 'Limit', 'Plans': [{'Node Type': 'Append', 'Plans': [leaf(1), leaf(2), leaf(3)]}]}]}
        am = {'lp_ev_%02d_ix' % n: 'btree' for n in (1, 2, 3)}
        # a plan of a partitioned table names its partitions, never the parent: its name alone finds no scan and the probe table's heap makes the plan mixed
        self.assertEqual(collect.plan_structure(lateral, am, 'twin.lp_ev'), 'mixed')
        names = ['twin.lp_ev', 'twin.lp_ev_01', 'twin.lp_ev_02', 'twin.lp_ev_03']
        self.assertEqual(collect.plan_structure(lateral, am, names), 'btree')
        self.assertEqual(collect.plan_structure(lateral, am, names + ['twin.lp_dim']), 'mixed')   # the probe table as a subject too: the plan is both
        self.assertEqual(collect.plan_structure(lateral, am, ['twin.lp_dim']), 'heap')
        # a plan with the schemas (VERBOSE): the heap scan of a table of the same name in another schema is not the subject's
        other = {'Node Type': 'Seq Scan', 'Relation Name': 'lp_ev_01', 'Schema': 'other'}
        mine = dict(leaf(2), Schema='twin')
        self.assertEqual(collect.plan_structure({'Node Type': 'Append', 'Plans': [other, mine]}, am, names), 'btree')
        self.assertEqual(collect.plan_structure({'Node Type': 'Append', 'Plans': [dict(other, Schema='twin'), mine]}, am, names), 'mixed')

    def test_a_perf_stat_that_outlived_its_count_is_found_by_its_pids(self):
        with tempfile.TemporaryDirectory() as proc:
            def process(pid, args, started_ticks):
                os.makedirs(os.path.join(proc, str(pid)))
                open(os.path.join(proc, str(pid), 'cmdline'), 'w').write('\0'.join(args) + '\0')
                open(os.path.join(proc, str(pid), 'stat'), 'w').write('%d (perf) S ' % pid + ' '.join(['0'] * 18 + [str(started_ticks)]) + ' 0 0\n')
            ticks = os.sysconf('SC_CLK_TCK')
            open(os.path.join(proc, 'uptime'), 'w').write('10000.00 9000.00\n')
            process(11, ['perf', 'stat', '-x,', '-e', 'cycles:u', '-p', '5,6'], 100 * ticks)       # 9,900 s old: a count that never ended
            process(12, ['perf', 'stat', '-x,', '-p', '5,6'], 9900 * ticks)                         # 100 s old: a count in progress
            process(13, ['perf', 'record', '-p', '5'], 100 * ticks)                                 # not a stat
            process(14, ['perf', 'stat', '-a', 'sleep', '1'], 100 * ticks)                          # not of a process
            os.makedirs(os.path.join(proc, 'self'))
            self.assertEqual([(p, a) for p, a, _ in collect.stale_perf(600, proc)], [(11, 9900)])
            self.assertEqual([p for p, _, _ in collect.stale_perf(60, proc)], [11, 12])

    def test_a_standby_starts_from_the_facts_of_the_primary(self):
        class Standby:
            def run(self, sql):
                raise AssertionError('the standby has no statistics to ask: %s' % sql)
        facts = dict(churn_history='upd=5;ins=0;del=0;rounds=2', vacuums=9, autovacuums=0, settled_s=42.0, dead_pct=0.0, tombstone_pct=1.5)
        subject = collect.Subject('t', 1, 'arrival', 'churned_vacuumed', 't', facts=facts, settled=True)
        self.assertEqual(collect.start_state(Standby(), subject), facts)
        self.assertEqual(collect.start_state(Standby(), collect.Subject('t', 1, 'arrival', 'churned_vacuumed', 't', facts=facts, churn='declared'))['churn_history'], 'declared')
        rs = [dict(r, table_state='churned_vacuumed', settled=1, **facts) for r in clean()]
        self.assertEqual(problems(rs), [])

    def test_table_tombstones_count_column_rows_only(self):
        class Table:
            def run(self, sql):
                return (0, [], '')

        def answer(session, sql):
            if 'relkind' in sql:
                return ('r',)
            if 'pg_stat_all_tables' in sql:
                return (100, 5, 0, 100, 0, 1, 0, 42)
            if 'SELECT am.amname' in sql:
                return ('sift_table',)
            if 'sift.table_segments' in sql:
                self.assertIn("WHERE kind = 'columns'", sql)
                return (5, 100)
            if 'pg_index' in sql:
                return None
            raise AssertionError(sql)

        with patch.object(collect, 'one', side_effect=answer):
            facts = collect.churn_facts(Table(), 't')
        self.assertEqual(facts['tombstone_pct'], 5.0)
        self.assertEqual(facts['dead_pct'], 0.0)
        self.assertEqual(facts['vacuums'], 1)

    def test_a_failed_query_for_the_leaves_of_a_partitioned_table_is_an_error_not_no_facts(self):
        class Server:
            def run(self, sql):
                if 'relkind' in sql:
                    return (0.1, ['p'], '', '')
                if 'pg_partition_tree' in sql:
                    return (0.1, [], 'permission denied for function pg_partition_tree', '')
                return (0.1, [], '', '')
        with self.assertRaisesRegex(RuntimeError, 'permission denied'):
            collect.churn_facts(Server(), 'p')

    def test_a_short_statement_is_counted_over_a_window_of_runs(self):
        # (ms of one run, runs in the window)
        for ms, want in ((120.0, 1), (5.0, 1), (4.9, 50), (1.0, 50), (0.1, 50), (0.02, 250), (0.0, 5000), (0.0004, 5000)):
            with self.subTest(ms=ms):
                self.assertEqual(collect.repeats_for(ms), want)

    def test_a_list_of_statements_adds_its_times_and_a_failed_one_is_no_measurement(self):
        class Fake:
            def run(self, sql):
                return (2.5, [], 'ERROR: boom' if 'bad' in sql else '')
        self.assertEqual(collect.run_all(Fake(), ['a', 'b', 'c']), 7.5)
        self.assertEqual(collect.run_all(Fake(), 'a'), 2.5)
        with self.assertRaisesRegex(RuntimeError, 'boom'):
            collect.run_all(Fake(), ['a', 'bad'])

    def test_the_structure_is_the_access_method_of_the_index_the_plan_names(self):
        def plan(*nodes):
            return {'Node Type': 'Aggregate', 'Plans': list(nodes)}
        self.assertEqual(collect.plan_structure(PLAN, AM), 'btree')
        self.assertEqual(collect.plan_structure(plan({'Node Type': 'Seq Scan'}), AM), 'heap')
        self.assertEqual(collect.plan_structure(plan({'Node Type': 'Bitmap Heap Scan', 'Plans': [{'Node Type': 'Bitmap Index Scan', 'Index Name': 'gx'}]}), AM), 'gin')
        self.assertEqual(collect.plan_structure(plan({'Node Type': 'Index Only Scan', 'Index Name': 'sx'}), AM), 'sift')
        self.assertEqual(collect.plan_structure(plan({'Node Type': 'Custom Scan', 'Custom Plan Provider': 'SiftAgg'}), AM), 'sift')
        self.assertEqual(collect.plan_structure(plan({'Node Type': 'Seq Scan'}, {'Node Type': 'Index Scan', 'Index Name': 'ix'}), AM), 'mixed')
        self.assertEqual(collect.plan_structure(plan({'Node Type': 'Result'}), AM), 'mixed')
        self.assertEqual(collect.plan_structure(plan({'Node Type': 'Index Scan', 'Index Name': 'unknown'}), AM), 'mixed')

    def test_the_facts_of_an_explain_document_include_the_workers_of_every_gather(self):
        doc = {'Planning Time': 0.2, 'Execution Time': 3.0, 'Plan': {'Node Type': 'Gather', 'Total Cost': 9.0, 'Workers Planned': 2, 'Workers Launched': 1,
                                                                      'Plans': [{'Node Type': 'Seq Scan'}]}}
        self.assertEqual(collect.plan_facts(doc, AM), (0.2, 3.0, 9.0, 'heap', 2, 1))

    def test_blocks_are_the_difference_of_two_snapshots_by_kind(self):
        before = {('heap', 'hit'): 5, ('heap', 'read'): 1, ('btree', 'hit'): 2}
        after = {('heap', 'hit'): 15, ('heap', 'read'): 4, ('btree', 'hit'): 2, ('sift', 'read'): 7}
        self.assertEqual(collect.block_deltas(before, after), {'heap_hit': 10, 'heap_read': 3, 'btree_hit': 0, 'sift_read': 7})

    def test_the_host_and_the_server_figures(self):
        self.assertEqual(collect.psi('some avg10=12.50 avg60=1.00 avg300=0.50 total=1\nfull avg10=3.00'), 12.5)
        self.assertEqual(collect.cpu_quota('200000 100000', 8), 2.0)
        self.assertEqual(collect.cpu_quota('max 100000', 8), 8.0)
        self.assertEqual(collect.cpu_quota('', 4), 4.0)
        self.assertEqual([collect.pg_bytes_mb(x) for x in ('256MB', '4096kB', '1GB')], [256.0, 4.0, 1024.0])
        self.assertEqual([collect.cache_dropped(x) for x in ('evict+fadvise', 'evict (tmpfs)', 'warm: no eviction')], [1, 0, None])


class Partitioned(unittest.TestCase):
    """a partitioned table has no statistics, storage or access method of its own: churn_facts and structure_bytes read its leaf partitions (a plain cluster, no extension needed)"""

    @classmethod
    def setUpClass(cls):
        import cluster
        from duckdb import Session
        cls.dir, _, cls.port = cluster.start(cls, 'records-partitioned.', '-c autovacuum=off -c shared_buffers=32MB')
        cls.s = s = Session(cls.port, 60, None, db='postgres', host=cls.dir)
        for q in ("CREATE TABLE p (id int, k int, v int) PARTITION BY HASH (k)",
                  *("CREATE TABLE p_%d PARTITION OF p FOR VALUES WITH (MODULUS 3, REMAINDER %d)" % (i, i) for i in range(3)),
                  "INSERT INTO p SELECT g, g, g FROM generate_series(1, 30000) g", "CREATE INDEX p_k ON p (k)", "VACUUM p",
                  "UPDATE p SET v = v + 1 WHERE id % 10 = 0", "DELETE FROM p WHERE id % 20 = 1", "VACUUM p_0", "ANALYZE p_1",
                  "CREATE TABLE never (id int, k int) PARTITION BY HASH (k)",
                  *("CREATE TABLE never_%d PARTITION OF never FOR VALUES WITH (MODULUS 2, REMAINDER %d)" % (i, i) for i in range(2)),
                  "INSERT INTO never SELECT g, g FROM generate_series(1, 1000) g", "VACUUM never_0",
                  "CREATE TABLE empty (k int) PARTITION BY HASH (k)",
                  "CREATE TABLE tree (k int, v int) PARTITION BY RANGE (k)", "CREATE TABLE tree_a PARTITION OF tree FOR VALUES FROM (0) TO (100) PARTITION BY HASH (v)",
                  "CREATE TABLE tree_a0 PARTITION OF tree_a FOR VALUES WITH (MODULUS 2, REMAINDER 0)", "CREATE TABLE tree_a1 PARTITION OF tree_a FOR VALUES WITH (MODULUS 2, REMAINDER 1)",
                  "CREATE TABLE tree_b PARTITION OF tree FOR VALUES FROM (100) TO (200)", "INSERT INTO tree SELECT g % 200, g FROM generate_series(1, 4000) g",
                  "CREATE VIEW vw AS SELECT * FROM p", "CREATE TABLE raw AS SELECT g FROM generate_series(1, 2000) g",
                  "CREATE SCHEMA twin", "CREATE TABLE twin.lp_dim (id int PRIMARY KEY)", "INSERT INTO twin.lp_dim SELECT g FROM generate_series(1, 100) g",
                  "CREATE TABLE twin.lp_ev (id bigint NOT NULL, dim_a_id int NOT NULL, created_at timestamp NOT NULL) PARTITION BY RANGE (created_at)",
                  *("CREATE TABLE twin.lp_ev_%02d PARTITION OF twin.lp_ev FOR VALUES FROM ('2025-%02d-01') TO ('2025-%02d-01')" % (m, m, m + 1) for m in (1, 2, 3)),
                  "INSERT INTO twin.lp_ev SELECT i, 1 + i % 100, timestamp '2025-01-01' + (i % 90) * interval '1 day' FROM generate_series(1, 30000) i",
                  "CREATE INDEX lp_bt ON twin.lp_ev (dim_a_id, created_at)", "VACUUM (FREEZE, ANALYZE) twin.lp_dim, twin.lp_ev"):
            if s.run(q)[2]:
                raise RuntimeError(q + ': ' + s.run(q)[2])
        s.run('SELECT pg_stat_force_next_flush()')
        s.run('SELECT 1')

    def values(self, sql):
        return [r.split('|') for r in self.s.run(sql)[1]]

    def test_the_facts_of_a_partitioned_table_are_those_of_its_leaves_together(self):
        facts = collect.churn_facts(self.s, 'p')
        stats = self.values("SELECT sum(n_tup_ins), sum(n_tup_upd), sum(n_tup_del), sum(n_dead_tup), sum(n_live_tup), min(vacuum_count), max(vacuum_count), "
                            "min(extract(epoch FROM now() - last_vacuum)) FROM pg_stat_all_tables WHERE relname LIKE 'p\\_%'")[0]
        ins, upd, dele, dead, live, fewest, most, since = stats
        self.assertEqual(facts['churn_history'], 'ins=%s;upd=%s;del=%s' % (ins, upd, dele))
        self.assertEqual((ins, upd, dele), ('30000', '3000', '1500'))
        self.assertEqual((facts['vacuums'], facts['autovacuums']), (1, 0))   # the leaf that was vacuumed twice is not the table: every leaf had one
        self.assertEqual((fewest, most), ('1', '2'))
        self.assertAlmostEqual(facts['dead_pct'], 100.0 * int(dead) / (int(live) + int(dead)))
        self.assertGreater(int(dead), 0)
        self.assertAlmostEqual(facts['settled_s'], float(since), delta=5)
        self.assertIsNone(facts['tombstone_pct'])
        self.assertEqual(collect.churn_facts(self.s, 'p', 'upd=10;ins=0;del=5;rounds=1')['churn_history'], 'upd=10;ins=0;del=5;rounds=1')

    def test_a_leaf_without_a_pass_leaves_the_table_unsettled(self):
        facts = collect.churn_facts(self.s, 'never')
        self.assertEqual((facts['vacuums'], facts['autovacuums'], facts['settled_s']), (0, 0, None))
        rs = [dict(r, table_state='churned_vacuumed', settled=1, **facts) for r in clean()]
        self.assertTrue(any('SETTLED WITHOUT A VACUUM' in x for x in problems(rs)))

    def test_leaves_at_every_level_count_once(self):
        self.assertEqual(collect.churn_facts(self.s, 'tree')['churn_history'], 'ins=4000;upd=0;del=0')
        self.assertEqual(collect.churn_facts(self.s, 'tree_a')['churn_history'], 'ins=%s;upd=0;del=0' % self.values("SELECT count(*) FROM tree_a")[0][0])

    def test_what_has_no_statistics_has_no_facts_and_what_is_missing_is_an_error(self):
        self.assertEqual(collect.churn_facts(self.s, 'empty'), {})
        self.assertEqual(collect.churn_facts(self.s, 'vw'), {})
        with self.assertRaisesRegex(RuntimeError, 'does not exist'):
            collect.churn_facts(self.s, 'nosuch')

    LATERAL = 'SELECT d.id, x.created_at FROM twin.lp_dim d LEFT JOIN LATERAL (SELECT created_at FROM twin.lp_ev e WHERE e.dim_a_id = d.id ORDER BY created_at DESC LIMIT 1) x ON true'

    def test_the_plan_of_a_partitioned_btree_twin_names_btree(self):
        _, rows, err, *_ = self.s.run('EXPLAIN (FORMAT JSON) ' + self.LATERAL)
        self.assertFalse(err)
        plan = json.loads('\n'.join(rows))[0]['Plan']
        am, tables = collect.index_methods(self.s), collect.index_tables(self.s)
        self.assertEqual(collect.plan_structure(plan, am, 'twin.lp_ev', tables), 'mixed')   # the parent's name alone: the plan names its partitions
        self.assertEqual(collect.plan_structure(plan, *collect.catalog_of(self.s, 'twin.lp_ev')), 'btree')
        arm, named = collect.forceable(self.s, collect.Arm('btree'), self.LATERAL, collect.catalog_of(self.s, 'twin.lp_ev'), [])
        self.assertEqual((arm.arm, named), ('btree', None))

    def test_the_bytes_of_a_partitioned_twin_are_the_sums_over_its_leaves(self):
        table = int(self.values("SELECT sum(pg_table_size(relid)) FROM pg_partition_tree('twin.lp_ev') WHERE isleaf")[0][0])
        index = int(self.values("SELECT sum(pg_relation_size(i.indexrelid)) FROM pg_index i JOIN pg_partition_tree('twin.lp_ev') t ON t.relid = i.indrelid WHERE t.isleaf")[0][0])
        self.assertEqual(collect.structure_bytes(self.s, 'twin.lp_ev', 'btree'), (table, index))
        self.assertEqual(self.values("SELECT count(*) FROM pg_partition_tree('twin.lp_ev') WHERE isleaf")[0][0], '3')
        self.assertGreater(index, 0)
        self.assertEqual(collect.structure_bytes(self.s, 'twin.lp_ev', 'mixed'), (table, None))   # no index is a mixed plan's: not 0 bytes

    def test_the_visible_share_of_a_partitioned_table_is_over_its_leaves_and_unknown_until_counted(self):
        share = lambda t: self.values("SELECT 100.0 * sum(relallvisible) / sum(relpages) FROM pg_class c JOIN pg_partition_tree('%s') p ON p.relid = c.oid WHERE p.isleaf" % t)[0][0]   # noqa: E731
        vis = lambda t: collect.table_facts(self.s, t)[0]   # noqa: E731
        self.assertAlmostEqual(vis('twin.lp_ev'), 100.0)
        self.assertAlmostEqual(vis('p'), float(share('p')), places=6)   # one leaf vacuumed after the churn: the share of the leaves together, between 0 and 100
        self.assertTrue(0 < vis('p') < 100)
        self.assertIsNone(vis('never'))   # a leaf that no VACUUM counted: the table's share is not known
        self.assertIsNone(vis('raw'))
        self.assertIsNone(vis('empty'))

    def test_the_bytes_of_a_partitioned_table_are_those_of_its_leaves(self):
        table = int(self.values("SELECT sum(pg_table_size(relid)) FROM pg_partition_tree('p') WHERE isleaf")[0][0])
        index = int(self.values("SELECT sum(pg_relation_size(i.indexrelid)) FROM pg_index i JOIN pg_partition_tree('p') t ON t.relid = i.indrelid WHERE t.isleaf")[0][0])
        self.assertGreater(table, 0)
        self.assertGreater(index, 0)
        self.assertEqual(collect.structure_bytes(self.s, 'p', 'heap'), (table, None))
        self.assertEqual(collect.structure_bytes(self.s, 'p', 'btree'), (table, index))
        self.assertEqual(collect.structure_bytes(self.s, 'empty', 'btree'), (0, None))   # no leaf, no index: no bytes of storage, and no index to size
        self.assertEqual(collect.structure_bytes(self.s, 'raw', 'btree')[1], None)   # a table with no b-tree: not a b-tree of 0 bytes
        plain = int(self.values("SELECT pg_table_size('p_0')")[0][0])
        self.assertEqual(collect.structure_bytes(self.s, 'p_0', 'heap'), (plain, None))


class WriteInventory(unittest.TestCase):
    """what a table holds, measured, and the harness that declares it held to it (a plain cluster; the instruction count is stubbed: these are about the structures, not the count)"""

    @classmethod
    def setUpClass(cls):
        import cluster
        from duckdb import Session
        cls.dir, _, cls.port = cluster.start(cls, 'records-inventory.', '-c autovacuum=off -c shared_buffers=32MB')
        cls.s = Session(cls.port, 60, None, db='postgres', host=cls.dir)

    def run_sql(self, *statements):
        for q in statements:
            r = self.s.run(q)
            self.assertFalse(r[2], (q, r[2]))

    def test_the_structures_of_a_table_are_measured_and_a_constraint_index_is_the_definition(self):
        self.run_sql('CREATE TABLE inv1 (id int PRIMARY KEY, k int, label text)', 'CREATE TABLE inv0 (id int, k int)',
                     'CREATE TABLE inv_ref (r int REFERENCES inv1 (id))')   # a foreign key names the primary key's index, it does not add one
        st = lambda t: collect.table_structures(self.s, t)   # noqa: E731
        self.assertEqual((st('inv0'), st('inv1'), st('inv_ref')), ('heap', 'constraint,heap', 'heap'))
        self.run_sql('CREATE INDEX inv1_k ON inv1 (k)')
        self.assertEqual(st('inv1'), 'btree,constraint,heap')   # an index of its own is a structure
        self.run_sql('CREATE UNIQUE INDEX inv0_u ON inv0 (id)')
        self.assertEqual(st('inv0'), 'btree,heap')   # unique, but no constraint: nothing the table needs
        self.run_sql('CREATE TABLE inv_ref2 (r int REFERENCES inv0 (id))')   # a foreign key can name a unique index that is no constraint: still an index of its own
        self.assertEqual(st('inv0'), 'btree,heap')
        self.run_sql('CREATE TABLE inv_self (id int, p int)', 'CREATE UNIQUE INDEX inv_self_u ON inv_self (id)', 'ALTER TABLE inv_self ADD FOREIGN KEY (p) REFERENCES inv_self (id)')
        self.assertEqual(st('inv_self'), 'btree,heap')   # a foreign key of the table itself names that unique index: it is no constraint's
        self.run_sql('ALTER TABLE inv0 ADD CONSTRAINT inv0_unique UNIQUE (k)')
        self.assertEqual(st('inv0'), 'btree,constraint,heap')
        self.run_sql('CREATE TABLE inv_part (id int PRIMARY KEY, k int) PARTITION BY HASH (id)')
        self.assertEqual(st('inv_part'), 'constraint,heap')
        self.assertIsNone(st('no_such_table'))
        self.assertIsNone(st('a_schema_name_of_a_load'))

    def measure(self, index_sql, structures, arm='heap'):
        """the records of an INSERT into a table built with a primary key, INDEX_SQL and the structures the harness DECLARES; the table is rebuilt before each repetition"""
        env = collect.Env('0e767cbf3944', 'test inventory', schema.run_id('0e767cbf3944'), 4.0, reps=3, cool=None, maintenance='disabled')

        def prepare():
            self.run_sql('DROP TABLE IF EXISTS w', 'CREATE TABLE w (id int PRIMARY KEY, k int, label text)', 'INSERT INTO w SELECT g, g % 10, g::text FROM generate_series(1, 1000) g')
            self.run_sql(*index_sql)
        subject = collect.Subject('write', 1000, 'arrival', 'fresh_vacuumed', 'w', kind='load', key='k', keys=10, structures=structures)
        with patch.object(collect, 'count_instructions', return_value=(1.0, 1.0)), patch.object(collect, 'check_perf_free'):
            return collect.measure_statement(self.s, 'insert', 'INSERT INTO w SELECT g + 10000, g % 10, g::text FROM generate_series(1, 100) g', subject,
                                             [collect.Arm(arm, 'plain', settings=[], prepare=prepare, structures=structures)], env, temps=('warm',))

    def test_a_harness_is_held_to_the_structures_it_declares(self):
        records = self.measure([], 'constraint,heap')
        self.assertEqual(len(records), 3)
        self.assertEqual({r['structures'] for r in records}, {'constraint,heap'})
        self.assertEqual(problems(records), [])
        # an index nobody listed: the figure is of another table than the one the file says
        with self.assertRaisesRegex(RuntimeError, 'holds btree,constraint,heap after the statement, the harness declared constraint,heap'):
            self.measure(['CREATE INDEX w_k ON w (k)'], 'constraint,heap')
        # an index that was never built (a CREATE INDEX that failed): declared, absent
        with self.assertRaisesRegex(RuntimeError, 'holds constraint,heap after the statement, the harness declared btree,constraint,heap'):
            self.measure([], 'btree,constraint,heap', arm='btree')

    def test_an_unrelated_index_the_harness_does_declare_is_still_refused_by_the_checker(self):
        records = self.measure(['CREATE INDEX w_k ON w (k)'], 'btree,constraint,heap', arm='heap')
        self.assertTrue(any('OTHER STRUCTURES' in p and 'btree' in p for p in problems(records)), problems(records))

    def test_an_arm_that_reaches_the_statement_timeout_or_its_budget_is_unavailable_and_the_rest_goes_on(self):
        prepare = lambda: self.run_sql('DROP TABLE IF EXISTS w', 'CREATE TABLE w (id int PRIMARY KEY, k int, label text)')   # noqa: E731
        subject = collect.Subject('write', 100, 'arrival', 'fresh_vacuumed', 'w', kind='load', key='k', keys=10, structures='constraint,heap')
        fast = collect.Arm('heap', 'fast', settings=[], prepare=prepare, structures='constraint,heap', sql=['INSERT INTO w SELECT g, g, g::text FROM generate_series(1, 100) g'])
        for slow, budget, reason in [(collect.Arm('btree', 'slow', settings=['SET statement_timeout = 100'], prepare=prepare, structures='constraint,heap', sql=['SELECT pg_sleep(1)']), None, 'statement timeout'),
                                     (collect.Arm('btree', 'slow', settings=[], prepare=prepare, structures='constraint,heap', sql=['SELECT pg_sleep(0.6)']), 1, 'time budget of 1 s')]:
            with self.subTest(reason=reason):
                env = collect.Env('0e767cbf3944', 'test timeout', schema.run_id('0e767cbf3944'), 4.0, reps=3, cool=None, maintenance='disabled', arm_budget=budget)
                missed = []
                with patch.object(collect, 'count_instructions', return_value=(1.0, 1.0)), patch.object(collect, 'check_perf_free'):
                    records = collect.measure_statement(self.s, 'ins', fast.sql, subject, [fast, slow], env, temps=('warm',), unavailable=missed)
                self.assertEqual(sorted({(r['arm_option'], r['mode']) for r in records}), [('fast', 'forced'), ('slow', 'unavailable')])
                self.assertEqual(len([r for r in records if r['arm_option'] == 'fast']), 3)   # the arm before it is whole
                self.assertEqual([(a, o) for a, o, _ in missed], [('btree', 'slow')])
                self.assertRegex(missed[0][2], '^timeout: .*' + reason)
        self.assertEqual(self.s.run('SELECT 1')[2], '')   # the session is usable after the cancelled statement

    def test_the_timeout_of_a_statement_is_the_arms_and_any_other_error_stops_the_run(self):
        class Failing:
            def run(self, sql):
                return (1.0, [], self.err)
        s = Failing()
        s.err = 'ERROR:  canceling statement due to statement timeout'
        with self.assertRaises(collect.ArmTimeout):
            collect.run_all(s, 'SELECT 1')
        s.err = 'ERROR:  relation "t" does not exist'
        with self.assertRaises(RuntimeError) as caught:
            collect.run_all(s, 'SELECT 1')
        self.assertNotIsInstance(caught.exception, collect.ArmTimeout)


class CaseFlush(unittest.TestCase):
    def test_the_records_of_the_cases_so_far_are_in_the_file_after_each_case(self):
        from cases import recs
        rows = [{k: v for k, v in r.items() if not k.startswith('_')} for r in clean()]
        with tempfile.TemporaryDirectory() as d, patch.object(recs, 'ROWS', []):
            path = os.path.join(d, 'r.tsv')
            for n, part in enumerate((rows[:12], rows[12:])):
                recs.ROWS.extend(part)
                recs.flush(path)
                self.assertEqual(len(schema.read(path)), len(recs.ROWS), n)
                self.assertEqual(os.listdir(d), ['r.tsv'])   # no half-written file is left beside it

    def test_the_native_lane_of_a_case_is_the_arm_native_with_the_statement_of_its_own_or_the_sift_one(self):
        from types import SimpleNamespace
        from cases import recs
        case = lambda baselines, **kw: SimpleNamespace(baselines=baselines, opt=kw.get('opt', {}), statement=lambda lane: {'native': kw.get('pg', 'SELECT 1'), 'sift': 'SELECT 1'}[lane], sec={}, id='c')   # noqa: E731
        with patch.object(recs, 'expand', lambda text, c: text):
            self.assertIsNone(recs.native_arm(case(['pg_duckdb']), 'SELECT 1'))
            arm = recs.native_arm(case(['native']), 'SELECT 1')
            self.assertEqual((arm.arm, arm.option, arm.sql, arm.settings), (collect.NATIVE, 'default', None, ['SET sift.enable_agg = off']))
            own = recs.native_arm(case(['native'], pg='SELECT 2', opt={'set': 'sift.enable_col_scan = off'}), 'SELECT 1')
            self.assertEqual((own.sql, own.settings), ('SELECT 2', ['SET sift.enable_agg = off', 'SET sift.enable_col_scan = off']))


class SiftIndexFacts(unittest.TestCase):
    """the segments of a sift index and the leader of a build, from stand-ins for the extension on a plain cluster: access methods that are the hash index under the names sift and sift_col (the b-tree handler refuses a build under another access method on PostgreSQL 16 and 17), and the two
    directory functions over a table of rows; the instruction count is stubbed"""

    @classmethod
    def setUpClass(cls):
        import cluster
        from duckdb import Session
        cls.dir, _, cls.port = cluster.start(cls, 'records-facts.', '-c autovacuum=off -c shared_buffers=32MB')
        cls.s = Session(cls.port, 60, None, db='postgres', host=cls.dir)
        cls.run_sql(cls, 'CREATE SCHEMA sift', 'CREATE TABLE fake_segments (idx text, kind text)')
        for am, fn in (('sift_col', 'col_segments'), ('sift', 'segments')):
            cls.run_sql(cls, 'CREATE ACCESS METHOD %s TYPE INDEX HANDLER hashhandler' % am,
                        'CREATE OPERATOR CLASS int4_%s DEFAULT FOR TYPE int4 USING %s AS OPERATOR 1 =, FUNCTION 1 hashint4(int4)' % (am, am),
                        'CREATE FUNCTION sift.%s(index regclass) RETURNS TABLE (gen int8, kind text) LANGUAGE sql AS $$ SELECT row_number() OVER (), kind FROM fake_segments WHERE idx = index::text $$' % fn)

    def run_sql(self, *statements):
        for q in statements:
            r = self.s.run(q)
            if r[2]:
                raise AssertionError((q, r[2]))

    def directory(self, index, *kinds):
        self.run_sql("DELETE FROM fake_segments WHERE idx = '%s'" % index, *("INSERT INTO fake_segments VALUES ('%s', '%s')" % (index, k) for k in kinds))

    def test_the_directory_of_the_sift_indexes_of_a_table_is_counted_by_kind(self):
        self.run_sql('CREATE TABLE fw (k int)', 'CREATE INDEX fw_col ON fw USING sift_col (k) WITH (fillfactor = 70)')
        self.directory('fw_col', 'immutable', 'immutable', 'immutable', 'buffer', 'sealed buffer', 'tombstones')
        self.assertEqual(collect.index_facts(self.s, 'fw'), dict(segments=6, immutable_segments=3, buffer_segments=2, index_options='fillfactor=70'))
        # a second index adds its directory and its options, told apart; the BM25 index is read through sift.segments
        self.run_sql('CREATE INDEX fw_bm ON fw USING sift (k) WITH (fillfactor = 60)')
        self.directory('fw_bm', 'immutable', 'active', 'sealed')
        self.assertEqual(collect.index_facts(self.s, 'fw'), dict(segments=9, immutable_segments=4, buffer_segments=4, index_options='fillfactor=60 | fillfactor=70'))
        # the table of a load is named by its schema
        self.run_sql('CREATE SCHEMA c_fw', 'CREATE TABLE c_fw.t (k int)', 'CREATE INDEX c_ix ON c_fw.t USING sift_col (k)')
        self.directory('c_fw.c_ix', 'immutable')
        self.assertEqual(collect.index_facts(self.s, 'c_fw'), dict(segments=1, immutable_segments=1, buffer_segments=0, index_options='default'))
        # no sift index: the heap and b-tree arms have none
        self.run_sql('CREATE TABLE plain (k int)', 'CREATE INDEX plain_bt ON plain (k)')
        self.assertEqual(collect.index_facts(self.s, 'plain'), {})

    def build(self, cluster_m, leader_m, declared=None):
        """the records of a CREATE INDEX of the sift index fb_col: a stub counts CLUSTER_M instructions for the cluster and LEADER_M for the backend alone"""
        env = collect.Env('0e767cbf3944', 'test build', schema.run_id('0e767cbf3944'), 4.0, reps=3, cool=None, maintenance='disabled')

        def prepare():
            self.run_sql('DROP TABLE IF EXISTS fb', 'CREATE TABLE fb AS SELECT g AS k FROM generate_series(1, 1000) g')
            self.directory('fb_col', 'immutable', 'immutable', 'buffer')
        counts = lambda s, sql, pid, post, **kw: (leader_m, 1.0) if kw.get('leader') else (cluster_m, 2.0)   # noqa: E731
        subject = collect.Subject('build', 1000, 'arrival', 'fresh_vacuumed', 'fb', kind='build', protocol='single', structures='heap,sift')
        with patch.object(collect, 'count_instructions', side_effect=counts), patch.object(collect, 'check_perf_free'):
            return collect.measure_statement(self.s, 'create', 'CREATE INDEX fb_col ON fb USING sift_col (k) WITH (fillfactor = 80)', subject,
                                             [collect.Arm('sift', 'fill80', settings=[], prepare=prepare, structures='heap,sift', segments=declared)], env, temps=('warm',))

    def test_a_build_records_the_leader_apart_from_the_workers_and_the_directory_it_wrote(self):
        records = self.build(10.0, 4.0, declared=3)
        self.assertEqual(len(records), 3)
        for r in records:
            self.assertEqual((r['kind'], r['instr_m'], r['instr_leader_m'], r['instr_workers_m']), ('build', 10.0, 4.0, 6.0))
            self.assertEqual((r['segments'], r['immutable_segments'], r['buffer_segments'], r['segments_declared'], r['index_options']), (3, 2, 1, 3, 'fillfactor=80'))
            self.assertGreater(r['index_bytes'], 0)
        # the cluster ran 6 M beyond the leader but the monitor saw no worker: the figures do not agree
        self.assertTrue(any('BUILD WORKERS' in p for p in problems(records)))
        self.assertEqual(problems(self.build(4.0, 4.0, declared=3)), [])
        self.assertEqual(self.build(4.0, 9.0)[0]['instr_workers_m'], 0.0)   # a leader count above the cluster's is no negative figure

    def test_a_case_that_declares_a_count_the_directory_contradicts_is_refused(self):
        self.assertTrue(any('SEGMENTS CONTRADICT' in p and 'says 7' in p and 'holds 3' in p for p in problems(self.build(4.0, 4.0, declared=7))))

    def test_the_leader_is_counted_on_its_own_pid_and_the_cluster_on_both(self):
        seen = []

        class Gone:
            pid = 2 ** 22 + 7

            def poll(self):
                return 0

        def popen(args, **kw):
            seen.append(args[-1])
            return Gone()
        with patch.object(collect.subprocess, 'Popen', popen), patch.object(collect, 'stop_perf', return_value=''), patch.object(collect, 'stale_perf', return_value=[]):
            for leader in (False, True):
                with self.assertRaises(collect.PerfUnavailable):
                    collect.count_instructions(None, 'SELECT 1', 5, 6, tries=1, leader=leader)
        self.assertEqual(seen, ['5,6', '5'])


@unittest.skipUnless(os.environ.get('RECORDS_HOST'), 'RECORDS_HOST (a socket directory) and RECORDS_PORT name a server with a table t(id, k) with an index on k')
class Live(unittest.TestCase):
    def test_a_measured_set_passes_the_checker_and_has_instructions_with_workers(self):
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
        from duckdb import Session
        import iolib
        s = Session(int(os.environ['RECORDS_PORT']), 60, None, db=os.environ.get('RECORDS_DB', 'postgres'), host=os.environ['RECORDS_HOST'])
        env = collect.Env('abc123456789', 'test', schema.run_id('abc123456789'), collect.host_cpus(collect.postmaster_pid(collect.backend_pid(s))), cool=iolib.cool)
        recs = collect.measure_statement(s, 'range', 'SELECT count(*) FROM t WHERE k BETWEEN 100 AND 400', collect.Subject('t', 1, 'hash', 'fresh_vacuumed', 't', key='k'),
                                         [collect.Arm('heap'), collect.Arm('btree')], env)
        self.assertEqual(check.check(recs), [])
        self.assertTrue(all(r['instr_m'] for r in recs if r['temp'] == 'warm'))

    def test_a_point_lookup_of_twenty_microseconds_has_a_stable_instruction_count(self):
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
        from duckdb import Session
        import iolib
        import statistics
        s = Session(int(os.environ['RECORDS_PORT']), 60, None, db=os.environ.get('RECORDS_DB', 'postgres'), host=os.environ['RECORDS_HOST'])
        s.run('CREATE TABLE IF NOT EXISTS point_t AS SELECT g AS k, g % 100 AS v FROM generate_series(1, 100000) g')
        s.run('ALTER TABLE point_t ADD PRIMARY KEY (k)') if 'point_t_pkey' not in ''.join(s.run("SELECT relname FROM pg_class WHERE relname = 'point_t_pkey'")[1]) else None
        s.run('VACUUM ANALYZE point_t')
        env = collect.Env('abc123456789', 'test', schema.run_id('abc123456789'), collect.host_cpus(collect.postmaster_pid(collect.backend_pid(s))), reps=5, cool=iolib.cool)
        recs = collect.measure_statement(s, 'point', 'SELECT v FROM point_t WHERE k = 4242', collect.Subject('point_t', 100000, 'sorted', 'fresh_vacuumed', 'point_t', key='k'),
                                         [collect.Arm('btree')], env, temps=('warm',))
        counts = [r['instr_m'] for r in recs if r['mode'] == 'forced']
        self.assertEqual(len(counts), 5)
        self.assertLess(statistics.pstdev(counts) / statistics.mean(counts), 0.05)
        self.assertTrue(all(r['instr_n'] > 1 for r in recs if r['mode'] == 'forced'))

SAMPLES = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'records_samples')


class Versions(unittest.TestCase):
    """the schema is versioned: a change of FIELDS is a new version with its columns frozen in history.py, a step in schema.MIGRATIONS and a frozen sample here"""

    def test_a_change_of_the_columns_needs_a_new_version_a_migration_and_a_sample(self):
        self.assertEqual(schema.HISTORY[schema.VERSION], schema.COLUMNS, 'FIELDS changed: bump schema.VERSION, freeze the columns in history.py, add the step to MIGRATIONS and a records_samples/vN.tsv')
        self.assertEqual(sorted(schema.MIGRATIONS), list(range(1, schema.VERSION)), 'a migration from every version to the next')
        self.assertEqual(sorted(schema.HISTORY), list(range(1, schema.VERSION + 1)))
        for v, cols in schema.HISTORY.items():
            with self.subTest(version=v):
                self.assertTrue(set(cols) <= set(schema.COLUMNS), 'columns are only added')
                self.assertEqual(cols, [c for c in schema.COLUMNS if c in cols], 'and keep their order')
                with open(os.path.join(SAMPLES, 'v%d.tsv' % v)) as f:
                    first = f.readline().rstrip('\n')
                    head = f.readline().rstrip('\n') if first.startswith(schema.MARK) else first
                self.assertEqual(head.split('\t'), cols, 'the frozen sample of version %d is never edited' % v)

    def test_a_frozen_sample_of_every_version_is_read_upgraded_and_passes_the_checker(self):
        for v in schema.HISTORY:
            with self.subTest(version=v):
                rs = schema.read(os.path.join(SAMPLES, 'v%d.tsv' % v))
                self.assertEqual(len(rs), 24)
                self.assertEqual({r['_schema'] for r in rs}, {v})
                self.assertEqual(problems(rs), [])
                self.assertTrue(all(set(schema.COLUMNS) <= set(r) for r in rs))

    def test_old_rows_get_the_values_the_migrations_define(self):
        v1 = schema.read(os.path.join(SAMPLES, 'v1.tsv'))
        self.assertEqual({r['variant'] for r in v1}, {'main'})
        self.assertEqual({r['instr_n'] for r in v1 if r['instr_m'] is not None}, {1})
        row = dict(psi_cpu_start=3.0, psi_cpu_end=12.0, psi_io_start=0.0, psi_io_end=0.0)
        self.assertEqual(schema.upgrade(dict(row), 3)['noise'], 1)
        self.assertEqual(schema.upgrade(dict(row, psi_cpu_end=2.0), 3)['noise'], 0)
        churned = schema.upgrade(dict(table_state='churned_vacuumed', kind='read', instr_m=None), 2)
        self.assertEqual(churned['churn_history'], 'not recorded (schema 2)')

    def test_migrate_writes_the_file_again_keeps_the_old_one_and_leaves_a_current_one_alone(self):
        import shutil
        import tmpfs
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'old.tsv')
            shutil.copy(os.path.join(SAMPLES, 'v2.tsv'), path)
            self.assertEqual(schema.migrate(path), 2)
            self.assertTrue(os.path.exists(path + '.schema2'))
            self.assertEqual(schema.header_of(path)[0], schema.VERSION)
            self.assertEqual({r['_schema'] for r in schema.read(path)}, {schema.VERSION})
            self.assertEqual(problems(schema.read(path)), [])
            self.assertIsNone(schema.migrate(path))

    def test_a_file_that_is_no_version_says_so_and_an_old_one_is_noted(self):
        import tmpfs
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'x.tsv')
            open(path, 'w').write('a\tb\n1\t2\n')
            with self.assertRaisesRegex(ValueError, 'none of the 7 schema versions'):
                schema.read(path)
        said = []
        schema.read_files([os.path.join(SAMPLES, 'v1.tsv'), os.path.join(SAMPLES, 'v7.tsv')], say=said.append)
        self.assertEqual(len(said), 1)
        self.assertIn('schema 1, read as schema 7', said[0])
        self.assertIn('bench records migrate', said[0])


class Doc(unittest.TestCase):
    def test_the_table_of_columns_in_the_readme_is_the_generated_one(self):
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps', 'records', 'README.md')) as f:
            text = f.read()
        self.assertIn('<!-- columns -->\n%s\n<!-- /columns -->' % schema.doc_table(), text)


class Checker(unittest.TestCase):
    def test_a_clean_set_has_no_problem(self):
        self.assertEqual(problems(clean()), [])

    def test_each_mutant_is_refused_with_its_reason(self):
        def every(**kw):
            rs = clean()
            for r in rs:
                r.update(kw)
            return rs

        def first(**kw):
            rs = clean()
            rs[0].update(kw)
            return rs

        cases = [
            ('MIXED STATE', first(table_state='churned_unvacuumed', churn_history=CHURN)),
            ('MIXED STATE', first(heap_order='sorted')),
            ('CHURN HISTORY MISSING', every(table_state='churned_vacuumed')),
            ('MIXED STATE', [dict(r, table_state='churned_vacuumed', churn_history=CHURN) for r in clean()[:-1]] + [dict(clean()[-1], table_state='churned_vacuumed', churn_history='upd=50;ins=0;del=0;rounds=1')]),
            ('MIXED STATE', first(vacuums=3)),
            ('MIXED STATE', [dict(r, dead_pct=0.5) for r in clean()[1:]] + [dict(clean()[0], dead_pct=30.0)]),
            ('SETTLED WITHOUT A VACUUM', first(settled=1, vacuums=0, autovacuums=0)),
            ('INSTRUCTIONS MISSING', first(instr_m=None, instr_scope=None, temp='warm')),
            ('PICKED FLAG', every(kind='load', structures='heap,sift', plan_ms=None, exec_ms=None)),
            ('MIXED SETTINGS', first(shared_buffers_mb=128.0)),
            ('MIXED SETTINGS', first(fpw=0)),
            ('MIXED SETTINGS', first(cpus=1.0)),
            ('MIXED STATE', first(protocol='rounds=12')),
            ('HEAP ORDER NOT AS DECLARED', first(heap_order='sorted', heap_corr=0.02)),
            ('HEAP ORDER NOT AS DECLARED', every(heap_order='hash', heap_corr=0.99)),
            ('NO RECORDS', []),
            ('MIXED SETTINGS', first(settings_hash='1111111111')),
            ('TEST SETTINGS NOT LABELLED', every(settings='sift.col_auto_merge=off', settings_hash='2222222222')),
            ('TEST SETTINGS NOT LABELLED', every(settings='autovacuum=off', settings_hash='2222222222')),
            ('TEST SETTINGS NOT LABELLED', every(settings='sift.test_col_net=off', settings_hash='2222222222')),
            ('LABELLED DISABLED WITHOUT A SWITCH', every(maintenance='disabled')),
            ('MIXED STATE', first(maintenance='disabled', settings='sift.col_auto_merge=off', settings_hash='2222222222')),
            ('MIXED RUNS', first(run_id='abc123456789-20261101T010000Z-0002')),
            ('TWO STATEMENTS', first(sql_hash='bbbbbbbbbbbb')),
            ('WRONG ARM', first(structure='btree')),
            ('NO PICKED', [r for r in clean() if r['mode'] != 'default']),
            ('PICKED FLAG', first(picked=0)),
            ('INSTRUCTIONS WITHOUT WORKERS', first(workers_requested=2, workers_launched=2, instr_scope='backend')),
            ('INSTRUCTIONS WITHOUT SCOPE', first(instr_scope=None)),
            ('WORKERS SHORT', first(workers_requested=2, workers_launched=1, instr_scope='cluster')),
            ('QUOTA UNDER THE PROCESSES', every(cpus=2.0, workers_requested=2, workers_launched=2)),
            ('PLANNING NOT SEPARATE', first(plan_ms=None)),
            ('COLD WITHOUT os_cache_dropped', [dict(r, os_cache_dropped=None) if r['temp'] == 'cold' else r for r in clean()]),
            ('TOO FEW RUNS', clean()[1:]),
            ('OTHER STRUCTURES', every(kind='maintenance', structures='gin,heap,sift', exec_ms=1.0)),
            ('is empty', first(tip=None)),
        ]
        for reason, rs in cases:
            with self.subTest(reason=reason):
                self.assertTrue(any(reason in p for p in problems(rs)), '%s was not refused: %s' % (reason, problems(rs)))

    def test_a_default_plan_that_mixes_structures_is_a_row_of_its_own_and_no_arm(self):
        rs = [dict(r, picked=0) if r['mode'] == 'forced' else dict(r, arm='mixed', structure='mixed', plan_shape='Nested Loop(Seq Scan t, Seq Scan u)', plan_hash=collect.plan_hash('Nested Loop(Seq Scan t, Seq Scan u)')) for r in clean()]
        self.assertEqual(problems(rs), [])
        rs.append(dict(rs[0], arm='mixed', structure='mixed', rep=9))
        self.assertTrue(any('is none of' in p for p in problems(rs)))

    def test_an_arm_without_a_plan_is_one_row_that_says_so_and_does_not_refuse_the_set(self):
        gone = dict(clean()[1], mode='unavailable', arm='gin', structure='btree', picked=0, rep=1, wall_ms=None, plan_ms=None, exec_ms=None, instr_m=None, instr_scope=None, temp='warm', _line=99)
        rs = clean() + [gone]
        self.assertEqual(problems(rs), [])
        text = report.report(rs)
        self.assertIn('no plan for gin: the plans name btree', text)
        self.assertEqual(len([l for l in text.split('\n') if l.startswith('| gin')]), 0)
        self.assertTrue(any('also measured' in p for p in problems(clean() + [dict(gone, arm='btree', structure='heap')])))

    def test_host_load_is_a_flag_and_a_warning_never_a_refusal(self):
        rs = [dict(r, psi_cpu_start=40.0, psi_cpu_end=45.0, noise=1) if r['arm'] == 'heap' else r for r in clean()]
        self.assertEqual(problems(rs), [])
        self.assertEqual(len(check.load_warnings(rs)), 1)
        self.assertEqual(check.load_warnings(clean()), [])
        text = report.report(rs)
        self.assertIn('WARNING: measured under different host load', text)
        self.assertIn('noisy', [l for l in text.split('\n') if l.startswith('| heap')][0])

    def test_a_new_backend_is_compared_with_a_kept_one_but_not_with_rounds(self):
        rs = [dict(r, protocol='fresh backend') if r['arm'] == 'heap' else r for r in clean()]
        self.assertEqual(problems(rs), [])
        self.assertTrue(any('protocol' in p for p in problems(mutate(protocol='rounds=12'))))

    def test_a_load_has_forced_arms_and_no_picked_plan(self):
        rs = [dict(r, kind='load', picked=0, plan_ms=None, exec_ms=None, structures='heap,' + r['arm'] if r['arm'] != 'heap' else 'heap') for r in clean() if r['mode'] == 'forced']
        self.assertEqual(problems(rs), [])
        rs[0]['picked'] = 1
        self.assertTrue(any('PICKED FLAG' in p and 'no plan the planner chooses' in p for p in problems(rs)), problems(rs))

    def test_the_constraint_index_of_a_table_is_its_definition_and_any_other_index_is_a_structure(self):
        def writes(own, **kw):
            return [dict(r, kind='load', picked=0, plan_ms=None, exec_ms=None, structures=own(r['arm']), **kw) for r in clean() if r['mode'] == 'forced']
        companion = lambda arm: 'constraint,heap' + ('' if arm == 'heap' else ',' + arm)   # noqa: E731
        self.assertEqual(problems(writes(companion)), [])
        # an index that nobody needs is a structure of the figure, the b-tree of a primary key or not
        self.assertTrue(any('OTHER STRUCTURES' in p and 'gin' in p for p in problems(writes(lambda arm: companion(arm) + ',gin'))))
        self.assertTrue(any('OTHER STRUCTURES' in p and 'btree' in p for p in problems(writes(lambda arm: 'btree,heap' + ('' if arm == 'heap' else ',' + arm) if arm != 'btree' else 'btree,heap'))))
        # the arms of a comparison hold the same companions: a figure with the primary key beside one without is not a comparison of structures
        self.assertTrue(any('COMPANION DIFFERS' in p for p in problems(writes(lambda arm: 'heap' if arm == 'heap' else companion(arm)))))

    def test_a_revision_that_is_not_a_commit_is_refused_once(self):
        rs = [dict(r, tip='unknown') for r in clean()]
        found = [p for p in problems(rs) if 'TIP UNKNOWN' in p]
        self.assertEqual(len(found), 1)
        self.assertIn('"unknown"', found[0])
        self.assertEqual(problems([dict(r, tip='0e767cbf3944') for r in clean()]), [])

    def test_variants_are_tables_with_their_own_heap_order_and_picked_flag(self):
        rs = clean() + [dict(r, variant='sorted', heap_order='sorted', heap_corr=0.99) for r in clean()]
        self.assertEqual(problems(rs), [])
        forced = [r for r in rs if r['variant'] == 'sorted' and r['mode'] == 'forced'][0]
        forced['picked'] = 1 - forced['picked']
        self.assertTrue(any('PICKED FLAG' in p and 'variant sorted' in p for p in problems(rs)), problems(rs))

    def test_a_set_labelled_maintenance_disabled_is_accepted_and_says_so(self):
        rs = clean()
        for r in rs:
            r.update(settings='sift.col_auto_merge=off;work_mem=64MB', settings_hash='3333333333', maintenance='disabled')
        self.assertEqual(problems(rs), [])
        head = report.report(rs).split('\n')[0]
        self.assertIn('MAINTENANCE DISABLED', head)
        self.assertIn('3333333333 (sift.col_auto_merge=off, work_mem=64MB)', head)

    def test_the_report_shows_the_hash_of_default_settings(self):
        self.assertIn('settings da39a3ee5e (all default)', report.report(clean()).split('\n')[0])

    def test_the_hash_ignores_what_only_names_the_server_and_sees_what_changes_behaviour(self):
        a = schema.settings_hash({'work_mem': '64MB', 'port': '5432'})
        self.assertEqual(a, schema.settings_hash({'work_mem': '64MB', 'port': '6000', 'data_directory': '/x'}))
        self.assertNotEqual(a, schema.settings_hash({'work_mem': '4MB'}))
        self.assertEqual(schema.maintenance_switches({'autovacuum': 'on', 'sift.col_auto_merge': 'on', 'work_mem': '1MB'}), [])
        self.assertEqual(schema.maintenance_switches({'autovacuum': 'off', 'sift.async_maintenance': 'off'}), ['autovacuum=off', 'sift.async_maintenance=off'])

    def test_instructions_of_the_cluster_with_workers_are_accepted(self):
        rs = clean()
        for r in rs:
            r.update(workers_requested=2, workers_launched=2, instr_scope='cluster')
        self.assertEqual(problems(rs), [])

    def test_a_refusal_names_the_lines_and_the_values(self):
        p = [x for x in problems(mutate(table_state='churned_unvacuumed', churn_history=CHURN)) if 'MIXED STATE' in x][0]
        self.assertIn('fresh_vacuumed', p)
        self.assertIn('churned_unvacuumed', p)
        self.assertIn('t.tsv:', p)


class SiftFacts(unittest.TestCase):
    """the segments, the options and the split of a build are in the record, and the checker refuses what contradicts them"""

    def test_a_build_with_its_split_its_segments_and_its_options_is_clean(self):
        self.assertEqual(problems(build_rows()), [])
        self.assertEqual(problems(build_rows(segments_declared=3)), [])

    def test_each_way_to_contradict_the_facts_is_refused_with_its_reason(self):
        cases = [
            ('SEGMENTS MISSING', build_rows(segments=None)),
            ('SEGMENTS MISSING', build_rows(immutable_segments=None)),
            ('SEGMENTS MISSING', build_rows(buffer_segments=None)),
            ('SEGMENTS MISSING', build_rows(index_options=None)),
            ('SEGMENT KINDS', build_rows(immutable_segments=2, buffer_segments=2)),
            ('SEGMENTS CONTRADICT', build_rows(segments_declared=7)),
            ('SEGMENTS CONTRADICT', build_rows(segments=None, immutable_segments=None, buffer_segments=None, segments_declared=1)),
            ('BUILD WITHOUT SPLIT', build_rows(instr_leader_m=None)),
            ('BUILD WITHOUT SPLIT', build_rows(instr_workers_m=None)),
            ('BUILD SPLIT', build_rows(instr_workers_m=3.0)),
            ('BUILD WORKERS', build_rows(workers_requested=0, workers_launched=0)),
            ('BUILD ON TEST SETTINGS', build_rows(settings='sift.test_col_merge_workers=0', settings_hash='4444444444', maintenance='disabled')),
            ('BUILD WITHOUT index_bytes', build_rows(index_bytes=None, segments=None, immutable_segments=None, buffer_segments=None, index_options=None)),
            ('BUILD WITHOUT wal_bytes', build_rows(wal_bytes=None)),
        ]
        for reason, rs in cases:
            with self.subTest(reason=reason, rows=len(rs)):
                self.assertTrue(any(reason in p for p in problems(rs)), '%s was not refused: %s' % (reason, problems(rs)))

    def test_a_build_with_no_workers_has_the_cluster_of_its_leader(self):
        self.assertEqual(problems(build_rows(workers_requested=0, workers_launched=0, instr_leader_m=10.0, instr_workers_m=0.0)), [])

    def test_every_record_of_a_sift_index_has_segments_and_other_structures_have_none(self):
        withbytes = lambda **kw: [dict(r, index_bytes=100, **kw) for r in clean()]   # noqa: E731
        found = [p for p in problems(withbytes()) if 'SEGMENTS MISSING' in p]
        self.assertEqual(len(found), 12)   # the sift arm's repetitions and the default row's, warm and cold, not the b-tree's or the heap's
        self.assertEqual(problems(withbytes(segments=2, immutable_segments=2, buffer_segments=0, index_options='default')), [])
        # a row written before the facts were recorded is not refused for lacking them
        self.assertEqual(problems(withbytes(_schema=5)), [])

    def test_the_facts_survive_a_file(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'r.tsv')
            schema.write(p, [{k: v for k, v in r.items() if not k.startswith('_')} for r in build_rows(segments_declared=3, index_options='order_array_columns=created_at;key_cut=off')])
            back = schema.read(p)
        self.assertEqual({(r['segments'], r['immutable_segments'], r['buffer_segments'], r['segments_declared'], r['instr_leader_m'], r['instr_workers_m'], r['index_options']) for r in back},
                         {(3, 3, 0, 3, 4.0, 6.0, 'order_array_columns=created_at;key_cut=off')})
        self.assertEqual(problems(back), [])


class Report(unittest.TestCase):
    def test_the_table_is_generated_from_the_records_with_the_picked_arm_marked(self):
        text = report.report(clean())
        self.assertIn('run abc123456789-20261101T000000Z-0001', text)
        self.assertIn('sift*', text)
        self.assertNotIn('btree*', text)
        self.assertIn('shared buffers only', text)
        row = [l for l in text.split('\n') if l.startswith('| heap ')][0]
        self.assertIn('| 52 |', row)

    def test_a_churned_table_that_was_not_vacuumed_says_so_in_the_header(self):
        rs = [dict(r, table_state='churned_unvacuumed', churn_history=CHURN, allvisible_pct=12.0) for r in clean()]
        self.assertIn('NOT VACUUMED AFTER THE CHURN', report.report(rs).split('\n')[0])
        self.assertNotIn('NOT VACUUMED', report.report(clean()).split('\n')[0])

    def test_variants_are_rows_of_their_own_not_one_average(self):
        rs = clean() + [dict(r, variant='B', heap_order='sorted', heap_corr=0.99, exec_ms=9.0) for r in clean()]
        text = report.report(rs)
        self.assertEqual(len([l for l in text.split('\n') if l.startswith('| heap ') or l.startswith('| B: heap ')]), 4)   # warm and cold table, each variant
        self.assertIn('| B: sift*', text)

    def test_a_refused_set_has_no_table(self):
        with self.assertRaisesRegex(ValueError, 'MIXED STATE'):
            report.report(mutate(table_state='churned_unvacuumed', churn_history=CHURN))

    def test_the_tsv_form_has_one_line_for_each_arm_and_temperature(self):
        lines = [l for l in report.report(clean(), tsv=True).split('\n') if l]
        self.assertEqual(len(lines), 4 + 4)

    def test_noise_is_marked(self):
        rs = clean()
        for r in rs:
            if r['arm'] == 'btree' and r['temp'] == 'warm':
                r['wall_ms'] = r['wall_ms'] * (1 + 0.5 * (r['rep'] - 2))
        row = [l for l in report.report(rs).split('\n') if l.startswith('| btree ')][0]
        self.assertIn('noisy', row)


if __name__ == '__main__':
    unittest.main()
