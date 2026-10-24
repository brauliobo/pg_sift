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


class Takeaway(unittest.TestCase):
    def fig(self, case, native, sift, others, plan):
        c = type('C', (), dict(id=case))
        return dict(case=c, metric='ms', native=native, sift=sift, others=others, plan=plan, kept=False, noisy=False)

    def test_wins_ties_kept_plans_and_settings_that_do_not_help(self):
        import cases_render as r
        figs = [self.fig('a', 20.0, 2.0, {'scan': 5.0}, 'Top-N'), self.fig('b', 20.0, 2.0, {'zone_off': 1.9}, 'Top-N'), self.fig('c', 1.0, 1.0, {}, 'Seq Scan')]
        figs[2]['kept'] = True
        text = r.takeaway(figs)
        self.assertIn('faster than PostgreSQL on the same rows (`sift.enable_agg = off`) on 2 of 2 numbers (10x to 10x)', text)
        self.assertIn('brings no gain (its variant is as fast, within 5%) on 1: b ms `zone_off`', text)
        self.assertIn("on 1: c ms", text)
        slow = self.fig('d', 1.0, 1.5, {}, 'Seq Scan')
        slow['kept'] = True
        self.assertEqual(r.same_plan(slow), 'same plan (1.5x slower)')
        self.assertEqual(r.same_plan(figs[2]), 'same plan')

    def test_ranked_puts_losses_before_ties_and_wins(self):
        import cases_render as r
        win, loss, tie = self.fig('w', 20.0, 2.0, {}, 'Top-N'), self.fig('l', 1.0, 2.0, {}, 'Top-N'), self.fig('t', 1.0, 1.0, {}, 'Seq Scan')
        slow_kept = self.fig('k', 1.0, 1.5, {}, 'Seq Scan')
        slow_kept['kept'] = True
        self.assertEqual([r.ranked(f) for f in (win, loss, tie, slow_kept)], [2, 0, 1, 0])


class Scale(unittest.TestCase):
    def pts(self, *ratios):
        return [dict(rows=200000 * 10 ** i, native=r, sift=1.0, kept=False, noisy=False, plan='') for i, r in enumerate(ratios)]

    def test_crossover(self):
        import cases_render as r
        self.assertEqual(r.crossover(self.pts(3.0, 20.0)), 'wins from 200k rows')
        self.assertEqual(r.crossover(self.pts(0.5, 2.0)), 'about 632k rows (between 200k and 2M, wins from 2M)')
        self.assertEqual(r.crossover(self.pts(0.5, 0.4)), 'none up to 2M rows (2.5x slower at the largest, the ratio is falling)')
        self.assertEqual(r.crossover(self.pts(2.0, 0.5, 4.0)), 'about 4.31M rows (between 2M and 20M, wins from 20M)')

    def test_kept_plan_is_not_a_win(self):
        import cases_render as r
        pts = self.pts(1.0, 1.0)
        for p in pts:
            p['kept'] = True
        self.assertEqual(r.crossover(pts), "the planner keeps PostgreSQL's plan up to 2M rows")


class PlanShape(unittest.TestCase):
    def test_numbers_do_not_change_the_shape_but_a_node_does(self):
        a = 'HashAggregate  (cost=1 rows=2) (actual time=1 rows=3 loops=1)\n  Group Key: x\n  ->  Seq Scan on t  (cost=1)\nPlanning Time: 0.1 ms'
        self.assertEqual(cases.plan_shape(a), cases.plan_shape(a.replace('rows=3', 'rows=9').replace('0.1', '7')))
        self.assertNotEqual(cases.plan_shape(a), cases.plan_shape(a.replace('Seq Scan on t', 'Index Scan using i on t')))
