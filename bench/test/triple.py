#!/usr/bin/env python3
"""Tests of steps/triple.py, the `time · CPU · IO` cell of the benchmark tables. Run: python3 -I bench/test/triple.py"""
import os, sys, unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
import triple  # noqa: E402
from triple import NA  # noqa: E402

MISSING = NA(triple.NOT_MEASURED)


class NotMeasured(unittest.TestCase):
    ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')

    def test_the_version_in_the_reason_is_the_one_of_the_control_file(self):
        control = open(os.path.join(self.ROOT, 'pg_sift.control')).read()
        version = control.split('default_version', 1)[1].split("'")[1]
        self.assertEqual(triple.NOT_MEASURED, f'not measured under {version} yet')

    def test_no_generator_spells_the_reason_or_a_version_of_it(self):
        steps = os.path.join(self.ROOT, 'bench', 'steps')
        spelled = [f for f in sorted(os.listdir(steps)) if f.endswith('.py') and 'not measured under' in open(os.path.join(steps, f)).read()]
        self.assertEqual(spelled, ['triple.py'])


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


class Factors(unittest.TestCase):
    def test_a_missing_or_zero_reference_has_no_factor(self):
        self.assertEqual(triple.factors((10.0, MISSING, 4.0), (2.0, 3.0, 0)), (5.0, None, None))

class SearchFig(unittest.TestCase):
    ROWS = [('articles', 'slim', e, 'and', '-', m, v, v, v, 'x', 'NA', '2', v) for e, m, v in (
        ('sift', 'qps', '4000'), ('sift', 'cpu_ms_query', '0.5'), ('sift', 'read_mb_query', '0.25'),
        ('es', 'qps', '2000'), ('es', 'cpu_ms_query', '2.0'), ('es', 'read_mb_query', 'NA'), ('pg', 'qps', '1000'))]

    def test_time_is_the_clients_over_the_throughput(self):
        import tables
        self.assertEqual(tables.query_fig(self.ROWS, 'articles', 'slim', 'sift', 'and'), (2.0, 0.5, 0.25))
        got = tables.query_fig(self.ROWS, 'articles', 'slim', 'es', 'and')
        self.assertEqual(got[:2], (4.0, 2.0))
        self.assertIs(got[2], tables.NO_COUNTER)
        self.assertEqual(tables.query_fig(self.ROWS, 'articles', 'slim', 'pg', 'and')[1:], (triple.NA(triple.NOT_MEASURED),) * 2)
        self.assertIsNone(tables.query_fig(self.ROWS, 'articles', 'slim', 'pg', 'or'))

    def test_table_has_factors_over_sift(self):
        import tables
        text = '\n'.join(tables.md_triples(self.ROWS, 'articles', 'slim', {'pg_sift': 'sift', 'Elasticsearch': 'es', 'PostgreSQL': 'pg'}, [('and', 'and')]))
        self.assertIn('| pg_sift | **2.00 · 0.50 · 0.25** |', text)
        self.assertIn('| Elasticsearch | 4.00 · 2.00 · n/a (2.0x · 4.0x · n/a) |', text)
        self.assertIn('`n/a`: the engine reports no count of the data a query touched.', text)


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
