"""The records library (bench/steps/records): the schema round-trips, the checker accepts a clean set and refuses each way of mixing what must not be mixed, the report is generated
from records. A mutant is the clean set with one field changed; each must be refused with its reason."""
import copy
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps', 'records'))
import check  # noqa: E402
import report  # noqa: E402
import schema  # noqa: E402

BASE = dict(run_id='abc123456789-20261101T000000Z-0001', tip='abc123456789', harness='test records-1', dataset='orders', rows=300000, keys=3000, rows_per_key=100.0, heap_order='arrival',
            heap_corr=0.01, table_state='fresh_vacuumed', allvisible_pct=100.0, structures='btree,heap,sift', statement='top20', sql_hash='aaaaaaaaaaaa', kind='read', protocol='single', mode='forced',
            arm='sift', arm_option='default', picked=1, structure='sift', temp='warm', rep=1, est_cost=100.0, plan_ms=0.1, exec_ms=1.0, wall_ms=1.3, instr_m=5.0, cycles_m=4.0,
            instr_scope='cluster', workers_requested=0, workers_launched=0, heap_hit=10, heap_read=0, btree_hit=0, btree_read=0, gin_hit=0, gin_read=0, sift_hit=30, sift_read=0,
            other_hit=0, other_read=0, os_cache_dropped=None, wal_bytes=0, fpw=1, cpus=4.0, psi_cpu_start=1.0, psi_cpu_end=2.0, psi_io_start=0.0, psi_io_end=0.0, shared_buffers_mb=256.0,
            work_mem_mb=64.0, pg_version='18.1')


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
