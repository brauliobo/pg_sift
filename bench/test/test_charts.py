#!/usr/bin/env python3
"""Unit tests of the chart captions of bench/steps/charts.py. No docker, no PostgreSQL:  python3 -I bench/test/test_charts.py   (or  bench/bench test)"""
import os, re, sys, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'steps'))
import charts  # noqa: E402


def caption_size(s):
    svg = charts.Svg('t', 'd')
    svg.caption(100, s)
    return float(re.search(r'font-size="([0-9.]+)"', svg.parts[-1]).group(1))


class Caption(unittest.TestCase):
    def test_a_caption_that_fits_keeps_its_size(self):
        self.assertEqual(caption_size('x' * 100), 11)

    def test_a_host_name_a_character_too_long_shrinks_the_line_instead_of_stopping_the_run(self):
        s = 'Simple English Wikipedia, 50,000 articles, 8 clients, median of 2-3 rounds; Test CPU Model 9 1234XT, 32 threads (load avg 20-20); Oct 2026.'
        self.assertLess(caption_size(s), 11)
        self.assertGreaterEqual(caption_size(s), 10)

    def test_a_caption_that_cannot_fit_stops(self):
        with self.assertRaises(AssertionError):
            caption_size('x' * 200)


if __name__ == '__main__':
    unittest.main()
