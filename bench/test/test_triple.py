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


class Factors(unittest.TestCase):
    def test_a_missing_or_zero_reference_has_no_factor(self):
        self.assertEqual(triple.factors((10.0, MISSING, 4.0), (2.0, 3.0, 0)), (5.0, None, None))

    def test_glance_cell_counts_the_time_and_ranges_the_measures(self):
        import compare
        pairs = [('a', 2.0, 3.0, None), ('b', 20.0, 0.5, None), ('c', 1.0, None, None), ('d', 0.5, 1.0, None)]
        self.assertEqual(compare.cell(pairs), '2 of 4 (2.0x to 20x · 0.5x to 3.0x · n/a), 1 tie; **slower on d 2.0x**')
        self.assertEqual(compare.cell([('a', 3.0)]), '1 of 1 (3.0x · n/a · n/a)')
        self.assertEqual(compare.cell([]), 'n/a')


class CaseFig(unittest.TestCase):
    ROWS = [dict(case='c', arm='sift', metric='cpu_ms', value='5'), dict(case='c', arm='sift', metric='buffers', value='128')]

    def test_cpu_and_buffers_of_a_lane(self):
        import cases_render
        self.assertEqual(cases_render.case_fig(self.ROWS, 'c', 'sift', 2.0), (2.0, 5.0, 1.0))   # 128 blocks of 8 kB are 1 MB
        got = cases_render.case_fig(self.ROWS, 'c', 'native', 2.0)
        self.assertEqual(got[0], 2.0)
        self.assertIsInstance(got[1], NA)
        self.assertIsInstance(got[2], NA)

    def test_instrumented_cpu_stands_in_and_the_cold_run_has_its_time_and_cpu(self):
        import cases_render
        rows = [dict(case='c', arm='sift', metric=m, value=v) for m, v in (('io_warm_cpu_ms', '4'), ('io_cold_ms', '9'), ('io_cold_cpu_ms', '7'))]
        self.assertEqual(cases_render.case_fig(rows, 'c', 'sift', 2.0)[1], 4.0)
        self.assertEqual(cases_render.cold_fig(rows, 'c', 'sift', 3.0), (9.0, 7.0, 3.0))
        got = cases_render.cold_fig(rows, 'c', 'native', 3.0)
        self.assertIsInstance(got[0], NA)
        self.assertEqual(got[2], 3.0)


class UpdatesFig(unittest.TestCase):
    class S:
        lanes = ['pg', 'sift']
        sizes = [1000]
        VALUES = {('pg', 'phase_s'): 0.5, ('pg', 'phase_cpu_ms'): 400.0, ('pg', 'wal_mib'): 10.0, ('sift', 'phase_s'): 0.25, ('sift', 'phase_cpu_ms'): 200.0, ('sift', 'wal_mib'): 5.0, ('sift', 'tps'): 500.0}

        def med(self, size, lane, cls, variant, metric):
            return self.VALUES.get((lane, metric))

    def test_phase_seconds_become_milliseconds_and_missing_measures_are_reasons(self):
        import section_render
        S = self.S()
        self.assertEqual(section_render.fig(S, 1000, 'sift', '-', 'u', ('phase_s', section_render.SECONDS), 'phase_cpu_ms', 'wal_mib'), (250.0, 200.0, 5.0))
        got = section_render.fig(S, 1000, 'pg', '-', 'u', 'tps', 'phase_cpu_ms', 'wal_mib')
        self.assertIsNone(got)
        reason = triple.NA('a probe')
        self.assertEqual(section_render.fig(S, 1000, 'sift', '-', 'u', ('tps', lambda v: 1e6 / v), reason, 'nothing')[1:], (reason, triple.NA(triple.NOT_MEASURED)))

    def test_table_compares_postgres_with_the_best_sift_lane(self):
        import section_render
        lines = '\n'.join(section_render.triples(self.S(), 1000, [('update', ('-', 'u', ('phase_s', section_render.SECONDS), 'phase_cpu_ms', 'wal_mib'))])[0])
        self.assertIn('| `sift_col` | **250 · 200 · 5.00** |', lines)
        self.assertIn('| PostgreSQL | 500 · 400 · 10.0 (2.0x · 2.0x · 2.0x) |', lines)


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
