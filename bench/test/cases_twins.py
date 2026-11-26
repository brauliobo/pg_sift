"""The statement of each lane of a case (bench/steps/cases/registry.py Case.statement): no server."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
from cases import registry  # noqa: E402

TEXT = '-- x\n-- label: l\n--@ pg\nSELECT pg\n--@ sift\nSELECT sift\n'
ROW = ['twin_x', '-', 'events', '-', 'SiftAgg', 'native', 'gin:search_path = c_twin_x__gin;sift.enable_agg = off,scan:sift.agg_preagg = off', 'ms', 'faster33', 'no', '-']


class Statements(unittest.TestCase):
    """a stock PostgreSQL index on a twin schema answers PostgreSQL's statement; a variant of the sift lane answers the sift one"""

    def test_each_lane_runs_its_statement(self):
        case = registry.Case(ROW, TEXT)
        self.assertEqual(case.variants, [['gin', 'search_path = c_twin_x__gin;sift.enable_agg = off'], ['scan', 'sift.agg_preagg = off']])
        want = {'native': 'SELECT pg', 'sift': 'SELECT sift', 'scan': 'SELECT sift', 'pg_duckdb': 'SELECT pg'}
        want.update({twin: 'SELECT pg' for twin in registry.PG_TWINS})
        self.assertEqual({lane: case.statement(lane) for lane in want}, want)


if __name__ == '__main__':
    unittest.main()
