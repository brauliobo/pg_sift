"""The pure parts of bench/steps/cases.py and cases_discover.py: the claim rule of the CHANGELOG gate, the shape signature, the ranking of candidates."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
import cases  # noqa: E402
import cases_discover as disc  # noqa: E402


class Claims(unittest.TestCase):
    def test_claims(self):
        for line, want in [
            ('- Added: pg_sift: a page of rows is 47.1 ms against 2.7 with the scan.', True),
            ('- Changed: pg_sift: 12 -> 3 ms per group', True),
            ('- Changed: pg_sift: 5.5 -> 0.34 ms, 3x faster', True),
            ('- Changed: pg_sift: 17.84M to 17.19M user instructions', True),
            ('- Added: pg_sift: the faster path is taken when it can be', False),
            ('- Fixed: pg_sift: a crash on a bad name', False),
            ('- Compatibility: 3.2 to 3.3 against nothing 1 to 2', False),
        ]:
            self.assertEqual(cases.claims(line), want, line)


class Signature(unittest.TestCase):
    def test_literals_and_names_do_not_matter(self):
        a = disc.signature("SELECT kind, count(*), sum(amount) FROM events WHERE ts >= date '2024-03-01' GROUP BY 1 ORDER BY 1")
        b = disc.signature("select country, count(*), sum(price) from orders where d >= date '2025-01-01' group by 1 order by 1")
        self.assertEqual(a, b)
        self.assertEqual(a, 'count sum group')

    def test_shapes_differ(self):
        self.assertNotEqual(disc.signature('SELECT count(DISTINCT k) FROM t'), disc.signature('SELECT count(*) FROM t'))
        self.assertIn('topn', disc.signature('SELECT id FROM t ORDER BY k LIMIT 10'))
        self.assertIn('window', disc.signature('SELECT count(*) OVER () FROM t'))


class Ranking(unittest.TestCase):
    def rec(self, sig, native, sift, weight=1.0, pushed=True):
        return dict(id=sig, sig=sig, native=native, sift=sift, weight=weight, pushed=pushed, source='t')

    def test_ranked_by_weight_times_gain_and_covered_shapes_left_out(self):
        recs = [self.rec('a', 40, 4, 1), self.rec('b', 30, 10, 10), self.rec('c', 50, 1), self.rec('d', 10, 9), self.rec('e', 40, 4, pushed=False), self.rec('f', 0.3, 0.1)]
        got = disc.rank(recs, {'c'}, 2.0)
        self.assertEqual([g['sig'] for g in got], ['b', 'a'])   # b: weight 10 x 3 beats a: 1 x 10; c is covered, d is under 2x, e is not pushed, f gains under 0.2 ms

    def test_a_different_answer_is_not_a_candidate(self):
        r = self.rec('a', 40, 4)
        r['differs'] = True
        self.assertEqual(disc.rank([r], set(), 2.0), [])


if __name__ == '__main__':
    unittest.main()
