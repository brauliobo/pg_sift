"""The plan checks of a case (bench/steps/cases/measure.py measure_query) and the settings of its lanes (server.apply_settings), on sessions that answer from a table: no server."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
from cases import measure, registry, run, server  # noqa: E402

HEAP = ['Seq Scan on orders']
UNDER = ['Custom Scan (SiftAggV6)', '  ->  Seq Scan on orders']
SCAN = ['Custom Scan (SiftAggV6)', '  Index: orders_col']


class Session:
    """a lane: EXPLAIN gives PLAN (or ERROR), a SET named in REFUSED gives its error, any other statement one row"""

    def __init__(self, plan, refused=(), error=''):
        self.plan, self.refused, self.error, self.ran = plan, refused, error, []

    def run(self, sql):
        self.ran.append(sql)
        if sql.startswith('EXPLAIN'):
            return 1.0, self.plan, self.error, ''
        return 1.0, ['1'], 'unrecognized configuration parameter' if sql in self.refused else '', ''


def case(head=''):
    row = ['x', '-', 'orders', 'default', r'Custom Scan \(SiftAggV6\)', 'native', '-', 'ms', 'faster33', 'no', '-']
    return registry.Case(row, '-- x\n-- label: l\n%s--@ pg\nSELECT 1\n' % head)


class PlanChecks(unittest.TestCase):
    def errors(self, c, sift, native=HEAP):
        return measure.measure_query(c, {'sift': Session(sift), 'native': Session(native)}, 0, 0)[2]

    def test_a_forbidden_node_under_the_scan_fails_the_lane(self):
        c = case('-- forbid: Seq Scan on orders\\b\n')
        self.assertEqual(c.forbid, [r'Seq Scan on orders\b'])
        self.assertTrue(self.errors(c, UNDER)['sift'].startswith('PLAN FORBIDDEN: /Seq Scan on orders\\b/'))
        self.assertEqual(self.errors(c, SCAN), {})
        self.assertEqual(self.errors(case(), UNDER), {})   # no forbid line: the plan regex alone

    def test_a_lost_plan_is_reported_before_a_forbidden_node(self):
        self.assertTrue(self.errors(case('-- forbid: Seq Scan on orders\\b\n'), HEAP)['sift'].startswith('PLAN LOST'))

    def test_a_forbidden_node_of_a_flagged_case_is_a_known_gap(self):
        c = case('-- gap: sift WS-113-4\n-- forbid: Seq Scan on orders\\b\n')
        failed, known = [], []
        run.settle(c, '', ('sift', 'native'), self.errors(c, UNDER), True, failed, known, lambda lane, e: True)
        self.assertEqual((failed, len(known)), ([], 1))

    def test_an_explain_that_fails_names_its_error(self):
        text = server.plan_text(Session([], error='could not find window clause for winref 1'), 'SELECT 1')
        self.assertEqual(text, 'EXPLAIN ERROR: could not find window clause for winref 1')
        self.assertEqual(server.plan_note(text), text)


class LaneSettings(unittest.TestCase):
    def test_a_setting_the_server_refuses_fails_the_run(self):
        s = Session(SCAN, refused=('SET sift.agg_preagg = off',))
        with self.assertRaises(SystemExit):
            server.apply_settings(s, ['SET search_path = c_x, public', 'SET sift.agg_preagg = off', 'SET work_mem = 1'], 'x: lane scan')
        self.assertNotIn('SET work_mem = 1', s.ran)

    def test_the_accepted_settings_return_the_session(self):
        s = Session(SCAN)
        self.assertIs(server.apply_settings(s, ['SET sift.enable_agg = off'], 'x: lane native'), s)
        self.assertEqual(s.ran, ['SET sift.enable_agg = off'])


if __name__ == '__main__':
    unittest.main()
