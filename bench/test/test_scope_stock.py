#!/usr/bin/env python3
"""bench/steps/scope_stock_arms.py and scope_stock_tables.py without a server: the classes are the files of bench/sql/scope_stock/queries, a run's raw tables become the one
results file, the tally of cells won, tied and lost follows its margins, and the generated blocks are a function of the results file and fit the rules of the README tables.

    python3 -I bench/test/test_scope_stock.py
"""
import argparse
import importlib.util
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
STEPS = os.path.join(HERE, '..', 'steps')
sys.path.insert(0, STEPS)
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)
import scope_stock_arms as arms  # noqa: E402
import scope_stock_tables as tables  # noqa: E402

CLASSES = ('sum_by_month', 'latest_20_rows')
# arm: (warm_ms of tenant 1 for each class, the same of tenant 4)
READS = {'stock_default': ((300.0, 250.0), (20.0, 18.0)), 'sift_scope': ((2.0, 0.5), (0.5, 0.3)), 'ivm': ((1.0, None), (0.55, None))}
CELL_HEAD = 'arm\ttenant\tclass\twarm_minstr\twarm_ms\twarm_hit\twarm_read\tcold_minstr\tcold_ms\tcold_hit\tcold_read\tmd5\tmd5_ok\tplan\n'


def tsv(path, head, rows):
    with open(path, 'w') as f:
        f.write(head + ''.join('\t'.join(map(str, r)) + '\n' for r in rows))


def raw_run(d):
    with open(os.path.join(d, 'run.meta'), 'w') as f:
        f.write(''.join('%s\t%s\n' % kv for kv in dict(
            facts=240000, member_total=2648, pairs=3600, tenant_total=32, postgresql='18.6', pg_sift='5.0 (abc)', pg_ivm='1.17', size='quick', date='2026-11-13',
            arms=','.join(READS), skipped='none', tenants='1,4', classes=','.join(CLASSES)).items()))
    tsv(os.path.join(d, 'fixture.tsv'), 'tenant\tmember_total\tevents\tevents_pct\n', [(1, 1149, 120000, 49.5), (4, 268, 14000, 6.0)])
    for arm, (t1, t4) in READS.items():
        cells = [(arm, t, c, ms * 10, ms, 40, 3, ms * 12, ms * 2, 4, 30, 'md5' + c, 'ok', 'Scope Index')
                 for t, vals in ((1, t1), (4, t4)) for c, ms in zip(CLASSES, vals) if ms is not None]
        tsv(os.path.join(d, arm + '.cells.tsv'), CELL_HEAD, cells)
        tsv(os.path.join(d, arm + '.build.tsv'), 'arm\tbuild_s\tbuild_wal_bytes\tadded_bytes\tnote\n', [(arm, 2.0, 1000, 5e6, '')])
        ops = ['insert 10k events', 'pair grant, median member'] + (['promotion of 10k events'] if arm == 'sift_scope' else [])
        tsv(os.path.join(d, arm + '.writes.tsv'), 'arm\toperation\tstatements\tminstr\tms\twal_bytes\n', [(arm, op, 1, 150.5, 12.3, 2.2e6) for op in ops])
        tsv(os.path.join(d, arm + '.dirty.tsv'), 'arm\ttenant\tclass\twarm_minstr\twarm_ms\tmd5\n', [(arm, 1, 'sum_by_month', 5.0, 3.0, 'x')])


def collected(d):
    raw_run(d)
    out = os.path.join(d, 'scope-stock.tsv')
    arms.cmd_collect(argparse.Namespace(raw=d, out=out))
    return out


def readme_rules():
    spec = importlib.util.spec_from_file_location('check_readme_tables', os.path.join(HERE, '..', '..', 'scripts', 'check-readme-tables.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


class Queries(unittest.TestCase):
    def test_every_class_is_a_file_with_an_order_and_the_arms_take_their_sections(self):
        self.assertEqual(len(arms.QUERIES), 9)
        self.assertEqual(list(arms.semi_classes()), list(arms.QUERIES))
        self.assertNotIn('latest_20_rows', arms.rollup_classes())
        self.assertNotIn('latest_20_rows', arms.ivm_classes())
        self.assertIn('LATERAL', arms.lateral_classes()['latest_20_rows'])
        self.assertNotIn('LATERAL', arms.semi_classes()['latest_20_rows'])
        self.assertTrue(all('tenant_id = {t}' in q for q in arms.copy_classes('events_t').values()))

    def test_no_placeholder_is_left_but_the_tenant(self):
        for make in (arms.semi_classes, arms.lateral_classes, arms.rollup_classes, arms.ivm_classes, lambda: arms.copy_classes('events_t')):
            for name, q in make().items():
                self.assertNotRegex(q.replace('{t}', ''), r'\{[a-z]+\}', name)


class Tally(unittest.TestCase):
    def test_margins(self):
        self.assertTrue(tables.faster(1.0, 2.0))
        self.assertFalse(tables.faster(1.0, 1.09), 'within 0.1 ms')
        self.assertFalse(tables.faster(100.0, 109.0), 'within 10%')
        self.assertTrue(tables.faster(100.0, 112.0))
        self.assertFalse(tables.faster(2.0, 1.0))

    def test_per_rival_counts_and_where_the_rival_wins(self):
        with tempfile.TemporaryDirectory() as d:
            R = tables.Results(collected(d))
        self.assertEqual(tables.tally_row(R, 'stock_default', 'warm_ms')[:4], (4, 4, 0, 0))
        n, won, tie, lost, where = tables.tally_row(R, 'ivm', 'warm_ms')
        self.assertEqual((n, won, tie, lost), (2, 0, 1, 1))   # tenant 1: 2.0 against 1.0 ms is a loss; tenant 4: 0.5 against 0.55 a tie
        self.assertEqual(where, {'sum_by_month': 1})


class Collect(unittest.TestCase):
    def test_the_results_file_holds_every_cell_and_says_what_it_measured(self):
        with tempfile.TemporaryDirectory() as d:
            R = tables.Results(collected(d))
        self.assertEqual(R.arms, list(READS))
        self.assertEqual(R.get(1, 'sum_by_month', 'sift_scope', 'warm_ms'), 2.0)
        self.assertIsNone(R.get(1, 'latest_20_rows', 'ivm', 'warm_ms'))
        self.assertEqual(R.get(4, 'sum_by_month', 'sift_scope', 'md5_equal'), 1)
        self.assertEqual(R.v[('write:insert 10k events', 'sift_scope', 'minstr')], 150.5)
        self.assertEqual(R.meta['pg_ivm'], '1.17')
        self.assertEqual(R.v[('t1', 'fixture', 'events_pct')], 49.5)


class Render(unittest.TestCase):
    PAGES = {'README.md': 'scope-stock-readme', 'docs/COL-SCOPE.md': 'scope-stock-tally scope-stock-geomean scope-stock-costs'}

    def tree(self, root):
        for page, names in self.PAGES.items():
            os.makedirs(os.path.dirname(os.path.join(root, page)), exist_ok=True)
            with open(os.path.join(root, page), 'w') as f:
                f.write('# page\n\n' + ''.join('<!-- bench:%s:begin x -->\n<!-- bench:%s:end -->\n\n' % (n, n) for n in names.split()) + 'after\n')

    def test_blocks_are_a_function_of_the_results_and_fit_the_readme_rules(self):
        with tempfile.TemporaryDirectory() as d:
            results = collected(d)
            root = os.path.join(d, 'tree')
            self.tree(root)
            tables.render(results, root)
            first = {p: open(os.path.join(root, p)).read() for p in self.PAGES}
            tables.render(results, root)
            self.assertEqual(first, {p: open(os.path.join(root, p)).read() for p in self.PAGES})
        readme = first['README.md']
        self.assertIn('| `latest_20_rows` | 250 / 500 ·', readme)   # stock_default, tenant 1: warm ms / cold ms · M instructions · blocks
        self.assertIn('n/a', readme)
        self.assertTrue(readme.endswith('after\n'))
        rules = readme_rules()
        self.assertEqual(rules.problems(readme) + rules.triples(readme), [])
        self.assertIn('`ivm` | 2 | 0 / 1 / 1', first['docs/COL-SCOPE.md'])


if __name__ == '__main__':
    unittest.main()
