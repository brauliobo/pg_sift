"""The budget table of `bench/bench release-quick` (bench/steps/final.sh --quick-table LOG, the internal entry that prints the table of a stage log and stops)."""
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)

FINAL = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps', 'final.sh')
LOG = [('headline', 900, 15), ('slope', 600, 11), ('cost', 370, 6), ('setup 1000000 orders', 196, 2), ('store', 600, 24), ('updates', 270, 5)]


def table(log, cold=False, budget=None):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, 'stages.tsv')
        with open(path, 'w') as f:
            f.writelines('%s\t%d\t%d\n' % row for row in log)
        env = {**os.environ, 'CACHE_COLD': '1' if cold else ''}
        r = subprocess.run(['bash', FINAL, '--quick', '--quick-table', path] + (['--budget', str(budget)] if budget is not None else []), capture_output=True, text=True, env=env)
    return r.returncode, r.stderr


class QuickTable(unittest.TestCase):
    def test_the_table_has_the_stages_of_the_pass_and_not_the_stages_inside_them(self):
        rc, out = table(LOG)
        self.assertEqual(rc, 0, out)
        self.assertIn('stage         planned  measured', out)
        self.assertIn('headline           15      15.0', out)
        self.assertNotIn('setup 1000000 orders', out)
        self.assertIn('total              %d      %.1f   (budget 60)' % (sum(r[2] for r in LOG if ' ' not in r[0]), sum(r[1] for r in LOG if ' ' not in r[0]) / 60), out)

    def test_a_store_stage_measured_with_a_cold_cache_says_so(self):
        self.assertNotIn('cache cold', table(LOG)[1])
        out = table(LOG, cold=True)[1]
        self.assertRegex(out, r'store\s+24\s+10\.0   cache cold')
        self.assertEqual(len([l for l in out.split('\n') if 'cache cold' in l]), 1)

    def test_a_pass_over_the_budget_exits_with_status_4_and_says_so(self):
        rc, out = table(LOG, budget=30)
        self.assertEqual(rc, 4, out)
        self.assertIn('OVER BUDGET: the quick pass took 46 min, the budget is 30 min', out)
        self.assertEqual(table(LOG, budget=0)[0], 0)   # 0: no check


if __name__ == '__main__':
    unittest.main()
