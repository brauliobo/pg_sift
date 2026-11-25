#!/usr/bin/env python3
"""Tests of the pure parts of the duckdb benchmark: the result comparison of steps/duckdb.py. Run: python3 -I bench/test/duckdb.py"""
import os, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'steps'))
import duckdb  # noqa: E402
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)


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


class RivalCache(unittest.TestCase):
    ROWS = [dict(version='v6', case=c, arm=a, metric='warm_ms', value=v, min=v, max=v, n=3, note='', load='0.1') for c, a, v in
            (('02', 'pg', '190.5'), ('02', 'duckdb_heap', '383.0'), ('02', 'duckdb_parquet', '8.9'), ('02', 'sift', '0.8'))]

    def test_only_the_rival_lanes_are_kept_and_they_come_back_with_their_metadata(self):
        meta = dict(settings='threads 3', engine='v1.4.3', duck_server='PostgreSQL 18.1 with pg_duckdb')
        with tempfile.TemporaryDirectory() as d:
            self.assertFalse(os.path.exists(os.path.join(d, 'complete')))
            duckdb.save_rivals(d, self.ROWS, meta)
            self.assertTrue(os.path.exists(os.path.join(d, 'complete')))
            got_meta, got = duckdb.load_rivals(d)
        self.assertEqual({r['arm'] for r in got}, {'pg', 'duckdb_heap', 'duckdb_parquet'})
        self.assertEqual({k: got_meta[k] for k in meta}, meta)
        self.assertEqual([(r['case'], r['arm'], r['value']) for r in got], [('02', 'pg', '190.5'), ('02', 'duckdb_heap', '383.0'), ('02', 'duckdb_parquet', '8.9')])

    def test_the_server_line_names_both_servers(self):
        self.assertEqual(duckdb.server_label('PostgreSQL 18.7 with pg_sift', 'PostgreSQL 18.1 with pg_duckdb'), 'PostgreSQL 18.7 with pg_sift, PostgreSQL 18.1 with pg_duckdb')


if __name__ == '__main__':
    unittest.main()
