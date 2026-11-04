"""The plan columns of schema 7: the shape and hash of a plan, the match of the planner's pick to an arm, and the checker's rules on them."""
import copy
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps', 'records'))
import test_records as base  # noqa: E402  (its fixtures; the module, so that its tests are not found twice)
import collect  # noqa: E402
import report  # noqa: E402


class PlanShape(unittest.TestCase):
    JOIN = {'Node Type': 'Hash Join', 'Join Type': 'Inner', 'Total Cost': 1.0, 'Actual Rows': 9, 'Plans': [
        {'Node Type': 'Seq Scan', 'Relation Name': 'orders', 'Actual Total Time': 3.0},
        {'Node Type': 'Hash', 'Plans': [{'Node Type': 'Custom Scan', 'Custom Plan Provider': 'SiftAgg', 'Relation Name': 'probe'}]}]}

    def test_the_shape_names_the_join_method_and_what_is_read_and_nothing_that_varies_between_runs(self):
        shape = collect.plan_shape(self.JOIN)
        self.assertEqual(shape, 'Hash Join[Inner](Seq Scan orders, Hash(Custom Scan probe))')
        slower = copy.deepcopy(self.JOIN)
        slower.update({'Total Cost': 99.0, 'Actual Rows': 1})
        slower['Plans'][0]['Actual Total Time'] = 50.0
        self.assertEqual(collect.plan_shape(slower), shape)
        loop = dict(copy.deepcopy(self.JOIN), **{'Node Type': 'Nested Loop'})
        self.assertNotEqual(collect.plan_hash(collect.plan_shape(loop)), collect.plan_hash(shape))
        self.assertEqual(collect.plan_shape({'Node Type': 'Aggregate', 'Strategy': 'Plain', 'Partial Mode': 'Simple', 'Plans': [{'Node Type': 'Seq Scan', 'Parallel Aware': True, 'Relation Name': 't'}]}),
                         'Aggregate(Parallel Seq Scan t)')

    def rows(self, hashes, default):
        """the three repetitions of the default plan and of a forced sift arm for each option of HASHES, which plan each ran"""
        out = []
        for option, plan in [('default-row', default)] + list(hashes.items()):
            for rep in (1, 2, 3):
                out.append(dict(base.BASE, mode='default' if option == 'default-row' else 'forced', arm='sift', arm_option='default' if option == 'default-row' else option, structure='sift', picked=1,
                                rep=rep, plan_shape=plan, plan_hash=collect.plan_hash(plan)))
        return out

    def test_the_default_row_names_the_arm_that_ran_its_plan_and_the_picked_flags_follow_the_plan(self):
        rows = self.rows({'default': 'Hash Join(Seq Scan t, Custom Scan s)', 'nl': 'Nested Loop(Seq Scan t, Custom Scan s)', 'other': 'Merge Join(Seq Scan t, Custom Scan s)'}, 'Nested Loop(Seq Scan t, Custom Scan s)')
        collect.match_default(rows)
        picked = {(r['mode'], r['arm_option']): r['picked'] for r in rows}
        self.assertEqual(picked, {('default', 'nl'): 1, ('forced', 'default'): 0, ('forced', 'nl'): 1, ('forced', 'other'): 0})
        # two arms that ran the plan: both are picked, the default row names the one called default
        rows = self.rows({'default': 'Hash Join(Seq Scan t)', 'also': 'Hash Join(Seq Scan t)', 'nl': 'Nested Loop(Seq Scan t)'}, 'Hash Join(Seq Scan t)')
        collect.match_default(rows)
        self.assertEqual({(r['mode'], r['arm_option'], r['picked']) for r in rows}, {('default', 'default', 1), ('forced', 'default', 1), ('forced', 'also', 1), ('forced', 'nl', 0)})
        # no arm ran the plan the planner chose: the default row keeps its option and every forced arm is not picked
        rows = self.rows({'default': 'Hash Join(Seq Scan t)'}, 'Merge Join(Seq Scan t)')
        collect.match_default(rows)
        self.assertEqual({(r['mode'], r['arm_option'], r['picked']) for r in rows}, {('default', 'default', 1), ('forced', 'default', 0)})

    def test_the_report_names_the_option_of_the_arm_that_the_planner_chose(self):
        rs = [dict(r, arm_option='nl') if r['mode'] == 'default' else r for r in base.clean()]
        self.assertIn('default plan: sift nl', report.report(rs))

    def test_the_checker_refuses_a_read_without_its_plan_a_hash_that_is_not_the_shapes_a_plan_that_changed_and_a_wrong_picked_flag(self):
        problems, clean, shape = base.problems, base.clean, base.shape
        self.assertEqual(problems(clean()), [])
        self.assertTrue(any('PLAN MISSING' in p for p in problems(base.mutate(plan_shape=None, plan_hash=None))))
        self.assertTrue(any('PLAN HASH' in p and 'is not the hash' in p for p in problems(base.mutate(plan_hash='000000000000'))))
        flipped = [dict(r, plan_shape='Index Scan t_ix', plan_hash=collect.plan_hash('Index Scan t_ix')) if (r['mode'], r['arm'], r['temp'], r['rep']) == ('forced', 'sift', 'warm', 3) else r for r in clean()]
        self.assertTrue(any('PLAN CHANGED' in p for p in problems(flipped)))
        # the forced btree arm ran the plan of the default row: it must be picked, and the default row must not name an arm that ran another plan
        same = [dict(r, **shape('sift')) if r['arm'] == 'btree' and r['mode'] == 'forced' else r for r in clean()]
        self.assertTrue(any('PICKED FLAG' in p and 'the same as the default plan' in p for p in problems(same)))
        named = [dict(r, arm_option='default', plan_shape='Index Scan t_ix', plan_hash=collect.plan_hash('Index Scan t_ix')) if r['mode'] == 'default' else r for r in clean()]
        self.assertTrue(any('DEFAULT OPTION' in p for p in problems(named)))
        # a row of an older schema says "not recorded" and is accepted
        old = [dict(r, plan_shape='not recorded (schema 6)', plan_hash=None, _schema=6) for r in clean()]
        self.assertEqual(problems(old), [])


if __name__ == '__main__':
    unittest.main()
