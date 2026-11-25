"""The expected-gap flag of the cases (`-- gap: LANE patch`, bench/steps/cases/run.py settle): no server."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
from cases import registry, run  # noqa: E402
from cases.runcfg import FIELDS  # noqa: E402


def flagged(gap='sift lab 0013'):
    row = ['topn_x', '-', 'orders', 'default', 'Global Order: true', 'native', '-', 'ms', 'faster33', 'no', '-']
    return registry.Case(row, '-- x\n-- label: l\n%s--@ pg\nSELECT 1\n' % ('-- gap: %s\n' % gap if gap else ''))


LOST = 'PLAN LOST: expected /Global Order: true/, the plan is Custom Scan'
EVERY = lambda lane, e: lane in ('sift', 'native') or e.startswith(('PLAN', 'RESULT'))


class KnownGap(unittest.TestCase):
    """`-- gap: LANE patch`: a case that is known not to hold its plan yet. The lost plan is reported, not failed; the kept plan fails the run until the flag is removed"""

    def settle(self, case, errors, judged=True, lanes=('sift', 'native')):
        failed, known = [], []
        run.settle(case, '', lanes, errors, judged, failed, known, EVERY)
        return failed, known

    def test_the_header_names_the_lane_and_the_patch(self):
        self.assertEqual(flagged().gap, ('sift', 'lab 0013'))
        self.assertIsNone(flagged(gap=None).gap)
        self.assertEqual(len(FIELDS), 11)
        with self.assertRaises(SystemExit):
            flagged(gap='sift')   # no patch that closes it

    def test_a_lost_plan_is_a_known_gap_and_does_not_fail(self):
        failed, known = self.settle(flagged(), {'sift': LOST})
        self.assertEqual(failed, [])
        self.assertEqual(len(known), 1)
        self.assertIn('known gap (sift)', known[0])
        self.assertIn('lab 0013', known[0])

    def test_a_kept_plan_fails_loudly_so_that_the_flag_cannot_outlive_the_fix(self):
        failed, known = self.settle(flagged(), {})
        self.assertEqual(known, [])
        self.assertEqual(len(failed), 1)
        self.assertIn('gap closed: remove the `-- gap:` line', failed[0])
        self.assertIn('topn_x.sql', failed[0])

    def test_a_wrong_answer_of_the_flagged_lane_still_fails(self):
        failed, known = self.settle(flagged(), {'sift': 'RESULT DIFFERS from native (3 rows against 5)'})
        self.assertEqual((len(failed), known), (1, []))

    def test_an_unflagged_case_that_loses_its_plan_fails(self):
        failed, known = self.settle(flagged(gap=None), {'sift': LOST})
        self.assertEqual((len(failed), known), (1, []))
        self.assertEqual(self.settle(flagged(gap=None), {}), ([], []))

    def test_other_lanes_are_not_covered_by_the_flag(self):
        failed, known = self.settle(flagged(), {'sift': LOST, 'native': 'RESULT DIFFERS from native (1 rows against 2)'})
        self.assertEqual((len(failed), len(known)), (1, 1))
        failed, _ = self.settle(flagged(gap='base lab 0013'), {'sift': LOST})
        self.assertEqual(len(failed), 1)

    def test_no_verdict_where_the_plan_is_not_asserted(self):
        self.assertEqual(self.settle(flagged(), {'sift': LOST}, judged=False)[1], [])   # scale points and --any-plan: the lost plan is not an error there either
        self.assertEqual(self.settle(flagged(), {}, judged=False), ([], []))   # and a kept plan is not a closed gap


if __name__ == '__main__':
    unittest.main()
