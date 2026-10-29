#!/usr/bin/env python3
"""Tests of the pure parts of the duckdb benchmark: the result comparison of steps/duckdb.py and the table of steps/extras.py. Run: python3 -I bench/test/test_duckdb.py"""
import os, sys, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'steps'))
import duckdb  # noqa: E402
import extras  # noqa: E402

CASES = ['orders-per-country', 'search-count', 'search-newest']
META = dict(date='2026-01-01', cpu='CPU, 16 threads', postgresql='PostgreSQL 18', pg_duckdb='1.1.1', duckdb='DuckDB v1.4.3', orders='200000')


def rows():
    v = {('orders-per-country', 'pg'): 5, ('orders-per-country', 'sift'): 1, ('orders-per-country', 'duckdb_heap'): 2, ('orders-per-country', 'duckdb_parquet'): 3,
         ('search-count', 'pg'): 4, ('search-count', 'sift'): 2, ('search-count', 'duckdb_heap'): 9, ('search-count', 'duckdb_parquet'): 8,
         ('search-newest', 'pg'): 3, ('search-newest', 'sift'): 2}
    out = []
    for c in CASES:
        for a in ('pg', 'sift', 'duckdb_heap', 'duckdb_parquet'):
            ms = v.get((c, a))
            note = 'substring LIKE form (tsvector is not supported)' if c == 'search-count' and a.startswith('duckdb') else 'not supported: x' if ms is None else ''
            out.append(dict(version='v', case=c, arm=a, metric='warm_ms', value='NA' if ms is None else str(ms), note=note))
    return out


class Compare(unittest.TestCase):
    def test_same(self):
        for a, b, want in [
            (['a|1', 'b|2'], ['b|2', 'a|1'], True),                 # any order
            (['a|26.4375'], ['a|26.43750000000001'], True),         # an average of money: numeric in PostgreSQL, double in DuckDB
            (['a|1'], ['a|2'], False),
            (['a|1'], ['a|1', 'b|2'], False),
            (['x'], ['y'], False),
        ]:
            self.assertEqual(duckdb.same(a, b), want, (a, b))


class Render(unittest.TestCase):
    def test_table(self):
        text = '\n'.join(extras.duckdb_section(META, rows() + [dict(version='v', case='load', arm=a, metric='load_s', value='1') for a in ('pg', 'sift', 'duckdb_parquet')]
                                                + [dict(version='v', case='load', arm=a, metric='table_mib', value='1') for a in ('pg', 'sift', 'duckdb_parquet')]))
        self.assertIn('| orders-per-country |', text)
        self.assertIn('**1.00 · n/a · n/a**', text)                              # the fastest of the row in bold
        self.assertIn('(4.0x · n/a · n/a) †', text)                                # the substitute statement is marked
        self.assertIn('not supported', text)                         # and what pg_duckdb cannot run is said, not left out
        self.assertIn('pg_sift on 3 (orders-per-country, search-count, search-newest)', text)
        self.assertIn('Not supported by pg_duckdb: search-newest', text)
        self.assertIn('faster on 1 (orders-per-country)', text)          # the summary names who wins where
        for line in text.split('\n'):
            if line.startswith('|'):
                self.assertLessEqual(line.count('|') - 1, 7, line)    # at most 7 columns: GitHub scrolls wider tables sideways


if __name__ == '__main__':
    unittest.main()
