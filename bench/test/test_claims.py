"""The claims of the CHANGELOG against records (bench/steps/records/claims.py): a claim within its tolerance passes, one off by more names the line and the run, a statement is narrowed to a
table state and a mode. The records are the clean set of test_records."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps', 'records'))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
import claims  # noqa: E402
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)
from test_records import BASE, CHURN, build_rows, clean, mutate  # noqa: E402


LINE = '- Improved: pg_sift: a top 20 over a sorted key takes 1 ms and 5 million instructions where the heap took 40 ms.'


class Claims(unittest.TestCase):
    def claim(self, **kw):
        return [dict(line=3, phrase='a top 20 over a sorted key', statement='top20', rows=300000, arm='sift', option='default', metric='exec_ms', value=1.0, tol=None, **kw)]

    def test_a_claim_within_its_tolerance_passes(self):
        self.assertEqual(claims.verify(self.claim(), clean(), LINE), [])
        c = self.claim()
        c[0].update(arm='heap', value=45.0, metric='exec_ms')
        self.assertEqual(claims.verify(c, clean(), LINE), [])

    def test_a_claim_off_by_more_names_the_line_and_the_run(self):
        c = self.claim()
        c[0]['value'] = 0.5
        out = claims.verify(c, clean(), LINE)
        self.assertEqual(len(out), 1)
        self.assertIn('CLAIM NOT MET', out[0])
        self.assertIn('a top 20 over a sorted key takes 1 ms', out[0])
        self.assertIn(BASE['run_id'], out[0])
        self.assertIn('measured 1', out[0])

    def test_blocks_instructions_and_the_picked_arm(self):
        for metric, value in (('instr_m', 5.0), ('blocks', 40), ('cold_blocks', 30), ('est_cost', 100.0)):
            c = self.claim()
            c[0].update(metric=metric, value=value, arm='picked' if metric == 'est_cost' else 'sift')
            self.assertEqual(claims.verify(c, clean(), LINE), [], metric)

    def test_a_statement_is_narrowed_to_a_table_state_and_a_mode(self):
        recs = clean()
        for r in recs:
            r['dataset'], r['variant'] = 'orders_20', 'on'
        recs += [dict(r, dataset='orders_20', variant='off', instr_m=r.get('instr_m', 5.0) * 4, _line=r['_line'] + 1000) for r in recs if r['arm'] == 'sift' and r['mode'] == 'default']
        recs += [dict(r, dataset='orders_5', variant='on', instr_m=r.get('instr_m', 5.0) * 2, _line=r['_line'] + 2000) for r in recs if r['arm'] == 'sift' and r['mode'] == 'default' and r['variant'] == 'on']
        for statement, value in (('top20@orders_20#on', 5.0), ('top20@orders_20#off', 20.0), ('top20@orders_5#on', 10.0), ('top20#off', 20.0)):
            c = self.claim()
            c[0].update(statement=statement, metric='instr_m', value=value, arm='picked')
            self.assertEqual(claims.verify(c, recs, LINE), [], statement)
        c = self.claim()
        c[0].update(statement='top20@orders_20#on', metric='instr_m', value=20.0, arm='picked')
        self.assertIn('CLAIM NOT MET', claims.verify(c, recs, LINE)[0])
        c[0].update(statement='top20@nowhere#on')
        self.assertIn('NO RECORD', claims.verify(c, recs, LINE)[0])

    def test_heap_blocks_are_the_heap_alone(self):
        recs = clean()
        for r in recs:
            r.update(heap_hit=7, heap_read=3, sift_hit=100)
        c = self.claim()
        c[0].update(metric='heap_blocks', value=10, arm='picked')
        self.assertEqual(claims.verify(c, recs, LINE), [])

    def test_a_stale_phrase_and_a_missing_record_fail(self):
        c = self.claim()
        c[0]['phrase'] = 'a sentence nobody wrote'
        self.assertIn('STALE CLAIM', claims.verify(c, clean(), LINE)[0])
        c = self.claim()
        c[0]['rows'] = 1000000
        self.assertIn('NO RECORD', claims.verify(c, clean(), LINE)[0])

    def test_a_claim_that_no_changelog_line_states_is_checked_against_the_records_alone(self):
        c = self.claim(note='design note')
        c[0].update(phrase=claims.NO_LINE, statement='build', arm='sift', option='default', metric='segments', value=3.0)
        self.assertEqual(claims.verify(c, build_rows(), LINE), [])
        c[0]['value'] = 7.0
        out = claims.verify(c, build_rows(), LINE)
        self.assertEqual(len(out), 1)
        self.assertIn('CLAIM NOT MET', out[0])
        self.assertIn('no CHANGELOG line (design note)', out[0])
        for metric, value in (('instr_leader_m', 4.0), ('instr_workers_m', 6.0), ('index_bytes', 1000.0), ('immutable_segments', 3.0), ('buffer_segments', 0.0), ('wal_bytes', 5.0)):
            c[0].update(metric=metric, value=value)
            self.assertEqual(claims.verify(c, build_rows(), LINE), [], metric)

    def test_records_the_checker_refuses_check_no_claim(self):
        out = claims.verify(self.claim(), mutate(table_state='churned_unvacuumed', churn_history=CHURN), LINE)
        self.assertIn('the records are refused', out[0])

    def test_the_file_of_claims_is_read(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'claims.tsv')
            with open(p, 'w') as f:
                f.write('# c\nphrase\tstatement\trows\tarm\toption\tmetric\tvalue\ttolerance\tnote\na top 20\ttop20\t300,000\tsift\t\texec_ms\t1\t10\t\n')
            c = claims.read_claims(p)
        self.assertEqual((c[0]['rows'], c[0]['option'], c[0]['tol'], c[0]['line']), (300000, 'default', 10.0, 3))




if __name__ == '__main__':
    unittest.main()
