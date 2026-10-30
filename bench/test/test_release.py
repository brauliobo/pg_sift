#!/usr/bin/env python3
"""Tests of the release run (bench/steps/release.sh): the preset `release`, its minutes against the budget, the stages it covers and the inputs key that lets the generic stage be carried.
Run: python3 -I bench/test/test_release.py"""
import os, subprocess, sys, unittest
from types import SimpleNamespace

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'bench', 'steps'))
import budget  # noqa: E402
import generic  # noqa: E402

BUDGET_MINUTES = 120   # never more; the plan is checked against it before anything starts


class Preset(unittest.TestCase):
    def test_every_stage_has_minutes_and_a_reason_and_the_total_is_inside_the_budget(self):
        p = budget.presets('release')
        stages = [s for s, _, _ in budget.RELEASE_STAGES]
        self.assertEqual(stages, ['store', 'updates', 'compression', 'stats', 'gap', 'search', 'cases', 'duckdb', 'generic', 'scaling', 'analytics'])
        self.assertEqual(set(stages), set(budget.RELEASE_WHY))
        rows = budget.release_table()
        total = int(rows[-1].split('|')[2].strip(' *'))
        self.assertLessEqual(total, BUDGET_MINUTES)
        self.assertGreater(int(p['P_MIN_STORE_CACHED']), 0)

    def test_the_cut_parameters_are_the_ones_the_table_says(self):
        p, std = budget.presets('release'), budget.presets('standard')
        for key in ('P_RUNS', 'P_WARMUPS', 'P_COLD', 'P_CPUS_LIST', 'P_RATES', 'P_ROWS'):   # the key of a cached store cell
            self.assertEqual(p[key], std[key], key)
        self.assertEqual((p['P_S_REPS'], p['P_S_REPS_BIG'], p['P_CASE_ROUNDS'], p['P_LANE_BUDGET']), ('1', '1', '1', '4'))


class Lanes(unittest.TestCase):
    def test_the_created_at_order_variant_is_a_pg_sift_table_lane_of_the_default_pass(self):
        import store_lib
        e = 'sift_table_ord'
        self.assertIn(e, store_lib.DEFAULT_ENGINES)
        self.assertIn(e, store_lib.PG_ENGINES)
        self.assertEqual(store_lib.DDL[e], store_lib.DDL['sift_table'])   # the same table and indexes, only the order of the load differs
        import triple
        self.assertIn(e, triple.MINE)


class CasesMemory(unittest.TestCase):
    def reserve(self, available_gb):
        out = subprocess.run(['bash', '-c', 'cd %s && BENCH=bench P_ROWS=1000000 && eval "$(sed -n "/^cases_memory() {/,/^}/p" bench/steps/final.sh)" && '
                              'awk() { echo %d; } && cases_memory && echo "$CASES_X $CASES_RESERVE_GB $CASES_CAP"' % (REPO, available_gb)], capture_output=True, text=True).stdout.split()
        return out[0], int(out[1]), out[2]

    def test_the_reserve_is_3x_with_a_24g_cap_when_the_host_has_3x_the_plan_and_8_gb_else_2x_with_16g(self):
        plan = self.reserve(1000)[1] // 3   # the plan of the cases at 1,000,000 orders, in GB
        self.assertGreater(plan, 0)
        for available, expected in ((3 * plan + 8, ('3', 3 * plan, '24G')), (3 * plan + 7, ('2', 2 * plan, '16G')), (4, ('2', 2 * plan, '16G'))):
            self.assertEqual(self.reserve(available), expected, available)


class StageList(unittest.TestCase):
    def test_final_plans_the_stages_of_every_table(self):
        out = subprocess.run(['bash', '-c', 'cd %s && bench/bench final --size release --dry-run 2>&1 | sed -n "/^Release pass/,/^Total/p"' % REPO], capture_output=True, text=True).stdout
        for stage in ('store', 'updates', 'compression', 'stats', 'gap', 'search', 'cases', 'duckdb', 'generic', 'scaling', 'analytics'):
            self.assertRegex(out, r'\n  %s +(about|carried|skipped)' % stage)
        self.assertIn('budget 120 min', out)


class InputsKey(unittest.TestCase):
    def key(self, **kw):
        a = SimpleNamespace(suites='tpch,hits,tseries,orders', rows=1000000, runs=3, cpus=3, memory='8g')
        a.__dict__.update(kw)
        return generic.inputs_key(a, generic.suite_cases(a.suites.split(',')), ['pg_duckdb', 'clickhouse'])

    def test_the_key_follows_the_rows_and_the_runs_and_not_the_pg_sift_sources(self):
        self.assertEqual(self.key(), self.key())
        self.assertNotEqual(self.key(), self.key(rows=200000))
        self.assertNotEqual(self.key(), self.key(runs=5))


if __name__ == '__main__':
    unittest.main()
