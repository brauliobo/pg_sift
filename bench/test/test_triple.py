#!/usr/bin/env python3
"""Tests of steps/triple.py, the `time · CPU · IO` cell of the benchmark tables. Run: python3 -I bench/test/test_triple.py"""
import os, sys, unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
import triple  # noqa: E402
from triple import NA  # noqa: E402

MISSING = NA(triple.NOT_MEASURED)


class Cell(unittest.TestCase):
    def test_reference_has_no_factors(self):
        self.assertEqual(triple.cell((4.4, 6.1, 0.5)), '4.40 · 6.10 · 0.50')

    def test_factors_are_other_over_pg_sift(self):
        self.assertEqual(triple.cell((9.2, 11.0, 3.1), (4.4, 6.1, 0.5)), '9.20 · 11.0 · 3.10 (2.1x · 1.8x · 6.2x)')

    def test_factor_formats(self):
        for v, ref, want in ((100, 1, '100x'), (9.6, 1, '10x'), (2.0, 1, '2.0x'), (1.049, 1, '1.0x'), (0.951, 1, '1.0x'), (0.8, 1, '0.8x'), (0.045, 1, '0.045x'), (0, 0, '1.0x'), (3, 0, '∞x')):
            self.assertEqual(triple.times(v, ref), want, (v, ref))

    def test_missing_measure_prints_na_and_never_a_factor(self):
        self.assertEqual(triple.cell((1.0, MISSING, 2.0), (1.0, 2.0, MISSING)), '1.00 · n/a · 2.00 (1.0x · n/a · n/a)')

    def test_no_figure_is_na(self):
        self.assertEqual(triple.cell(None), 'n/a')
        self.assertEqual(triple.cell((None, 1, 1)), 'n/a')

    def test_tiny_io_is_not_zero(self):
        self.assertEqual(triple.num(0.001), '<0.01')
        self.assertEqual(triple.num(0), '0.00')

    def test_bold(self):
        self.assertEqual(triple.cell((1.0, 1.0, 1.0), bold=True), '**1.00 · 1.00 · 1.00**')


class Table(unittest.TestCase):
    FIGS = {('sift', 'q'): (1.0, 2.0, 3.0), ('pg', 'q'): (10.0, 20.0, 30.0), ('es', 'q'): (1.02, MISSING, MISSING)}

    def lines(self):
        return triple.table({'pg_sift': 'sift', 'PostgreSQL': 'pg', 'Elasticsearch': 'es'}, [('q', 'q')], lambda s, c: self.FIGS.get((s, c)), lambda s, c: self.FIGS['sift', c])

    def test_bold_is_the_fastest_and_its_ties(self):
        rows = {l.split(' | ')[0].strip('| '): l for l in self.lines() if l.startswith('| ') and 'system' not in l}
        self.assertIn('**1.00 · 2.00 · 3.00**', rows['pg_sift'])
        self.assertIn('**1.02 · n/a · n/a (1.0x · n/a · n/a)**', rows['Elasticsearch'])
        self.assertIn('| 10.0 · 20.0 · 30.0 (10x · 10x · 10x) |', rows['PostgreSQL'])

    def test_legend_gives_the_reason_once(self):
        text = '\n'.join(self.lines())
        self.assertEqual(text.count(f'`n/a`: {triple.NOT_MEASURED}.'), 1)
        self.assertIn('CPU milliseconds', text)

    def test_wide_tables_split(self):
        cols = [(str(i), str(i)) for i in range(7)]
        lines = triple.table({'a': 'sift'}, cols, lambda s, c: (1.0, 1.0, 1.0), lambda s, c: (1.0, 1.0, 1.0))
        self.assertEqual(sum(l.startswith('| system') for l in lines), 2)


if __name__ == '__main__':
    unittest.main()
