"""LIMIT + OFFSET of a ranked query must not overflow: a bounded query never becomes an unbounded scan, and LIMIT 0
returns no rows.

    python3 -I tests/test_limit_overflow.py [-v]           (needs build/sift.so)
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_sift import TmpDb, connect  # noqa: E402

INT64_MAX = 9223372036854775807
RANKED = "SELECT rowid FROM t WHERE t MATCH 'alpha' ORDER BY rank LIMIT %s OFFSET %s"


class LimitOverflow(TmpDb):
    def setUp(self):
        super().setUp()
        self.db = connect(self.path)
        self.db.execute('CREATE VIRTUAL TABLE t USING sift(body, stemmer=none)')
        self.db.execute('BEGIN')
        for i in range(1, 1001):
            self.db.execute('INSERT INTO t(rowid, body) VALUES (?, ?)', (i, 'alpha beta%d' % i))
        self.db.execute('COMMIT')

    def rows(self, limit, offset):
        return [r[0] for r in self.db.execute(RANKED % (limit, offset))]

    def test_offset_int64_max_is_empty(self):
        self.assertEqual(self.rows(10, INT64_MAX), [])

    def test_limit_int64_max_with_offset_is_bounded_by_the_matches(self):
        self.assertEqual(len(self.rows(INT64_MAX, 990)), 10)

    def test_limit_and_offset_int64_max(self):
        self.assertEqual(self.rows(INT64_MAX, INT64_MAX), [])

    def test_limit_zero_returns_no_rows(self):
        self.assertEqual(self.rows(0, 0), [])
        self.assertEqual(self.rows(0, 5), [])

    def test_negative_limit_is_unbounded_with_an_offset(self):
        self.assertEqual(len(self.rows(-1, 5)), 995)

    def test_ordinary_paging_is_unchanged(self):
        every = self.rows(1000, 0)
        self.assertEqual(self.rows(10, 20), every[20:30])
        self.assertEqual(self.rows(5, 0), every[:5])


if __name__ == '__main__':
    unittest.main()
