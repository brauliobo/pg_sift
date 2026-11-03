"""The instruction counts of bench cases: which lanes get one (cases/run.py wants_minstr) and the serial pass of a load case (cases/measure.py load_settings, load_passes). No server."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
from cases import measure, run  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../steps'))
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)


class WhichLanes(unittest.TestCase):
    def test_a_lane_gets_the_count_where_the_case_or_the_run_asks_for_it(self):
        # (metrics of the case, --minstr, lane, fresh backend, counted)
        table = [
            (['ms'], False, 'sift', False, False),
            (['ms'], True, 'sift', False, True),
            (['ms'], True, 'native', False, True),
            (['ms'], True, 'btree', False, True),
            (['ms'], True, 'workers_4', False, False),      # a variant is counted only where the case lists the metric
            (['ms', 'minstr'], False, 'workers_4', False, True),
            (['ms', 'minstr'], False, 'sift', False, True),
            (['ms', 'minstr'], True, 'pg_duckdb', False, False),
            (['ms', 'minstr'], True, 'sift', True, False),   # a new backend for each run has none to count
        ]
        for metrics, asked, lane, fresh, want in table:
            with self.subTest(metrics=metrics, asked=asked, lane=lane, fresh=fresh):
                self.assertEqual(run.wants_minstr(metrics, asked, lane, fresh), want)


class SerialPass(unittest.TestCase):
    def test_the_timed_pass_alone_without_instructions_and_a_serial_one_with_them(self):
        self.assertEqual(measure.load_passes(False), (False,))
        self.assertEqual(measure.load_passes(True), (False, True))

    def test_the_serial_pass_ends_with_no_workers_so_it_overrides_the_variant(self):
        variant = ['SET max_parallel_maintenance_workers = 4']
        timed = measure.load_settings('col_vacuum_13cols', '2', variant, False)
        serial = measure.load_settings('col_vacuum_13cols', '2', variant, True)
        self.assertEqual(timed, ['SET search_path = c_col_vacuum_13cols, public', 'SET max_parallel_maintenance_workers = 2', 'SET max_parallel_maintenance_workers = 4'])
        self.assertEqual(serial[:-1], timed)
        self.assertEqual(serial[-1], 'SET max_parallel_maintenance_workers = 0')


if __name__ == '__main__':
    unittest.main()
