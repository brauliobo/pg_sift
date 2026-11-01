"""The records library (bench/steps/records): the schema round-trips, the checker accepts a clean set and refuses each way of mixing what must not be mixed, the report is generated
from records. A mutant is the clean set with one field changed; each must be refused with its reason."""
import copy
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps', 'records'))
import check  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)
import collect  # noqa: E402
import report  # noqa: E402
import schema  # noqa: E402

BASE = dict(run_id='abc123456789-20261101T000000Z-0001', tip='abc123456789', harness='test records-1', dataset='orders', rows=300000, keys=3000, rows_per_key=100.0, heap_order='arrival',
            heap_corr=0.01, table_state='fresh_vacuumed', allvisible_pct=100.0, structures='btree,heap,sift', statement='top20', sql_hash='aaaaaaaaaaaa', kind='read', protocol='single', mode='forced',
            arm='sift', arm_option='default', picked=1, structure='sift', temp='warm', rep=1, est_cost=100.0, plan_ms=0.1, exec_ms=1.0, wall_ms=1.3, instr_m=5.0, cycles_m=4.0,
            instr_scope='cluster', workers_requested=0, workers_launched=0, heap_hit=10, heap_read=0, btree_hit=0, btree_read=0, gin_hit=0, gin_read=0, sift_hit=30, sift_read=0,
            other_hit=0, other_read=0, os_cache_dropped=None, wal_bytes=0, fpw=1, cpus=4.0, psi_cpu_start=1.0, psi_cpu_end=2.0, psi_io_start=0.0, psi_io_end=0.0, shared_buffers_mb=256.0,
            work_mem_mb=64.0, pg_version='18.1', settings='', settings_hash='da39a3ee5e', maintenance='default')


def clean():
    rs = []
    for arm, option, structure, picked, mode, ms in (('sift', 'default', 'sift', 1, 'forced', 1.0), ('btree', 'default', 'btree', 0, 'forced', 2.0), ('heap', 'default', 'heap', 0, 'forced', 40.0),
                                                     ('sift', 'default', 'sift', 1, 'default', 1.0)):
        for temp in ('warm', 'cold'):
            for rep in (1, 2, 3):
                r = dict(BASE, arm=arm, arm_option=option, structure=structure, picked=picked, mode=mode, temp=temp, rep=rep, exec_ms=ms, wall_ms=ms * 1.3, _file='t.tsv', _line=len(rs) + 2)
                if temp == 'cold':
                    r.update(os_cache_dropped=0, sift_read=30 if arm == 'sift' else 0, btree_read=4 if arm == 'btree' else 0, heap_read=2000 if arm == 'heap' else 0)
                rs.append(r)
    return rs


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
            with self.assertRaisesRegex(ValueError, 'not the record schema'):
                schema.read(p)

    def test_the_statement_hash_ignores_white_space(self):
        self.assertEqual(schema.sql_hash('SELECT  1\n FROM t;'), schema.sql_hash('select 1 from t'.replace('select', 'SELECT').replace('from', 'FROM')))
        self.assertNotEqual(schema.sql_hash('SELECT 1'), schema.sql_hash('SELECT 2'))


PLAN = {'Node Type': 'Limit', 'Total Cost': 50.0, 'Plans': [{'Node Type': 'Index Scan', 'Index Name': 'ix', 'Relation Name': 't'}]}
AM = {'ix': 'btree', 'gx': 'gin', 'sx': 'sift_col'}


class Collect(unittest.TestCase):
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


class Doc(unittest.TestCase):
    def test_the_table_of_columns_in_the_readme_is_the_generated_one(self):
        text = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps', 'records', 'README.md')).read()
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
            ('MIXED STATE', first(table_state='churned_unvacuumed')),
            ('MIXED STATE', first(heap_order='sorted')),
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
            ('DIFFERENT LOAD', [dict(r, psi_cpu_start=40.0, psi_cpu_end=45.0) if r['arm'] == 'heap' else r for r in clean()]),
            ('OTHER STRUCTURES', every(kind='maintenance', structures='gin,heap,sift', exec_ms=1.0)),
            ('is empty', first(tip=None)),
        ]
        for reason, rs in cases:
            with self.subTest(reason=reason):
                self.assertTrue(any(reason in p for p in problems(rs)), '%s was not refused: %s' % (reason, problems(rs)))

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
        p = [x for x in problems(mutate(table_state='churned_unvacuumed')) if 'MIXED STATE' in x][0]
        self.assertIn('fresh_vacuumed', p)
        self.assertIn('churned_unvacuumed', p)
        self.assertIn('t.tsv:', p)


class Report(unittest.TestCase):
    def test_the_table_is_generated_from_the_records_with_the_picked_arm_marked(self):
        text = report.report(clean())
        self.assertIn('run abc123456789-20261101T000000Z-0001', text)
        self.assertIn('sift*', text)
        self.assertNotIn('btree*', text)
        self.assertIn('shared buffers only', text)
        row = [l for l in text.split('\n') if l.startswith('| heap ')][0]
        self.assertIn('| 52 |', row)

    def test_a_refused_set_has_no_table(self):
        with self.assertRaisesRegex(ValueError, 'MIXED STATE'):
            report.report(mutate(table_state='churned_unvacuumed'))

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
