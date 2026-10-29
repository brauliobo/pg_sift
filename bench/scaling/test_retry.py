#!/usr/bin/env python3
"""A failing slope is measured again before it fails (bench/scaling/scaling.py): a pass lifted by the host is forgiven, a growth that every pass shows is not. No server."""
import importlib.util
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
SPEC = importlib.util.spec_from_file_location('scaling', os.path.join(HERE, 'scaling.py'))
S = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(S)
from model import Case, Query, verdict  # noqa: E402

SIZES = [512, 1024, 2048, 4096, 8192]


class Args:
    reps, keep, quick = 5, True, False


def measure(times):
    """times[pass][size] -> ms: the rows run_case collects for one query when explain answers with those times, pass by pass"""
    calls = {}

    def explain(sql, sets, reps):
        size = int(sql)
        n = calls[size] = calls.get(size, -1) + 1
        return ([0.0], [times[min(n, len(times) - 1)][size]], 'SiftAgg', None), None

    S.psql = lambda *a, **k: ''
    S.explain = explain
    S.nosift.clear()
    case = Case('t', SIZES, [Query('q', lambda size: str(size), target='flat')], lambda size: '', native=False)
    rows = {}
    S.run_case('t', case, Args, rows, {})
    return verdict(rows[('t', 'q', 'sift', 'flat')], 'flat')[2]


class RetryTest(unittest.TestCase):
    flat = {s: 0.9 for s in SIZES}

    def test_a_pass_lifted_by_the_host_is_forgiven(self):
        lifted = dict(zip(SIZES, [0.14, 0.14, 0.16, 0.95, 0.88]))   # a hosted runner: the last two points of one pass, 40x the others, then flat (units read: 2)
        self.assertEqual(verdict(list(lifted.items()), 'flat')[2], 'FAIL')
        self.assertEqual(measure([lifted, dict(zip(SIZES, [0.14, 0.14, 0.15, 0.15, 0.16]))]), 'PASS')

    def test_growth_in_every_pass_fails(self):
        grow = {s: 0.9 * s / 512 for s in SIZES}
        self.assertEqual(measure([grow]), 'FAIL')

    def test_a_clean_pass_is_measured_once(self):
        self.assertEqual(measure([self.flat]), 'PASS')


if __name__ == '__main__':
    unittest.main()
