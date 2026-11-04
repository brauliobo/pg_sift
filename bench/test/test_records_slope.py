"""The slope table of the quick release profile (bench/steps/records/slope.py)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps', 'records'))
import test_records as base  # noqa: E402  (its fixtures; the module, so that its tests are not found twice)


class Slope(unittest.TestCase):
    def rows(self, n, sift_ms, heap_ms, statement):
        out = []
        for arm, mode, ms in (('sift', 'forced', sift_ms), ('heap', 'forced', heap_ms), ('sift', 'default', sift_ms)):
            for rep in (1, 2, 3):
                r = {k: v for k, v in base.BASE.items()}
                r.update(statement=statement, rows=n, arm=arm, mode=mode, structure=arm, temp='warm', rep=rep, exec_ms=ms, wall_ms=ms * 1.3)
                out.append(r)
        return out

    def test_a_case_whose_pg_sift_time_grows_faster_than_the_rows_is_marked(self):
        import slope
        small = self.rows(100000, 1.0, 40.0, 'flat') + self.rows(100000, 1.0, 40.0, 'steep') + self.rows(100000, 0.05, 40.0, 'tiny')
        large = self.rows(300000, 2.0, 120.0, 'flat') + self.rows(300000, 4.0, 120.0, 'steep') + self.rows(300000, 0.5, 120.0, 'tiny')
        got = {r[0]: r for r in slope.slope(small, large)}
        self.assertEqual({k: v[7] for k, v in got.items()}, {'flat': '', 'steep': 'faster than the rows', 'tiny': ''})   # a time under 0.2 ms is not read as a slope
        self.assertEqual(got['flat'][3:7], (1.0, 2.0, 40.0, 120.0))
        self.assertIn('| `steep` | 100,000 -> 300,000 | 1 -> 4 | 4.0x | 40 -> 120 | 3.0x | faster than the rows |', slope.table(sorted(got.values())))
        self.assertEqual(slope.slope(small, self.rows(300000, 2.0, 120.0, 'other')), [])   # only the statements measured at both sizes

    def test_a_case_without_a_plan_of_postgresql_shows_its_own_growth(self):
        import slope
        only = lambda n, ms: [r for r in self.rows(n, ms, 1.0, 'build') if r['arm'] == 'sift']   # noqa: E731
        got = slope.slope(only(100000, 1.0), only(300000, 2.0))
        self.assertEqual([(r[0], r[5], r[6]) for r in got], [('build', None, None)])
        self.assertIn('| `build` | 100,000 -> 300,000 | 1 -> 2 | 2.0x | - | - |  |', slope.table(got))


if __name__ == '__main__':
    unittest.main()
