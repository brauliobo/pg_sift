"""The 6.0 docs generators: the README rows and the docs/BENCHMARKS.md blocks come from docs/results/ (the committed baseline without it), the read stream switch of the scaling probes
exists only in SIFT_TESTING builds, and `scripts/gen-docs.py --check` renders new files of the worktree too. Run: python3 -I bench/test/f2_docs.py"""
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(HERE, '..', 'steps'))
sys.path.insert(0, os.path.join(HERE, '..', 'steps', 'f2'))
sys.path.insert(0, os.path.join(HERE, '..'))
import readme_charts  # noqa: E402
import resultfile  # noqa: E402
import scaling_io  # noqa: E402
import benchmarks  # noqa: E402
import readme  # noqa: E402
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)
from charts import data as chart_data, pg_queries, theme  # noqa: E402


def store_json(path, rows):
    base = dict(dataset='store', tier='10M-c3', unit='ms', min=0, max=0, stderr=None, n=1, values=[])
    with open(path, 'w') as f:
        json.dump({'meta': {'tiers': {'10M-c3': {}}}, 'rows': [dict(base, **r) for r in rows]}, f)


def latency(engine, cls, value):
    return dict(engine=engine, **{'class': cls}, variant='warm', metric='latency_ms', value=value)


def write_scope_stock(path, sift_ms, stock_ms):
    meta = dict(arms='stock_default,sift_scope', tenants='1', classes='sum_by_month', facts='2400007', pg_sift='6.0', date='2026-12-01')
    rows = [dict(version='6.0', case='t1:sum_by_month', arm=arm, metric=m, value=v) for arm, v in (('stock_default', stock_ms), ('sift_scope', sift_ms)) for m in ('warm_ms', 'cold_ms')]
    rows += [dict(version='6.0', case='t1:sum_by_month', arm=arm, metric='md5_equal', value=1) for arm in ('stock_default', 'sift_scope')]
    resultfile.write(path, 'scope', meta, rows)


class ReadmeRows(unittest.TestCase):
    def render(self, results):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, 'docs', 'results'))
            results(os.path.join(root, 'docs', 'results'))
            readme_path = os.path.join(root, 'README.md')
            for slug in readme.SECTIONS:
                resultfile.replace_block(readme_path, 'readme-' + slug, ['x'])
            readme.render(root)
            return open(readme_path).read()

    def test_every_section_is_a_block_with_its_picture_and_a_closed_details(self):
        text = self.render(lambda d: None)
        for slug in readme.SECTIONS:
            self.assertIn('<!-- bench:readme-%s:begin -->' % slug, text)
            self.assertIn('srcset="docs/charts/readme-%s-dark.svg"' % slug, text)
            self.assertIn('src="docs/charts/readme-%s-light.svg"' % slug, text)
        self.assertEqual(text.count('<details open>'), 1)
        details = len(readme.SECTIONS) - 1
        self.assertEqual(text.count('<details>'), details - 1)
        self.assertEqual(text.count('</details>'), details)
        self.assertEqual(text.count('</summary>\n\n'), details)
        self.assertEqual(text.count('\n\n</details>'), details)

    def test_without_results_the_rows_are_the_committed_baseline(self):
        text = self.render(lambda d: None)
        for cell in ('| Revenue per month (`sum`) | **0.93 ms** | 23.8 ms | 99.2 ms | 1,377 ms |', '| Joined top-k\\* | 6,689 | **14,037** | 431 | 110 |',
                     '10M orders title search `count(*)`: 2.35 ms vs Elasticsearch 11.6 ms and PG GIN 78.5 ms.',
                     '| `ivm` | **32 / 2 / 30** | **34 / 3 / 27** | 43.3 ms vs **2.18 ms** |', '| 10M orders (`sift_table`) | **457 MiB** | 1,549 MiB (PG 18) | **3.4× smaller** |',
                     '| Scoped build RSS slope | `gate` ≤ 0.5 B/row | 470.8 B/row (v5.1) | 6.0 run pending |',
                     'ClickHouse arms (`ch_join`, `ch_proj`): not measured yet.', '*Not measured for 6.0 yet: `bench/bench clickbench run`'):
            self.assertIn(cell, text)

    def test_the_rows_follow_the_results_files(self):
        def results(d):
            store_json(os.path.join(d, 'store-scaling.json'), [latency(e, c, v) for c, vals in (('search-count', (1.5, 9.0, 60.0)), ('search-newest', (7.0, 5.0, 100.0))) for e, v in zip(('sift', 'es', 'pg'), vals)])
            write_scope_stock(os.path.join(d, 'scope-stock.tsv'), 1.0, 300.0)
        text = self.render(results)
        self.assertIn('10M orders title search `count(*)`: 1.50 ms vs Elasticsearch 9.00 ms and PG GIN 60.0 ms.', text)
        self.assertIn('Tenant 1 `sum_by_month` on 2.4M events: `sift_scope` 1.00 ms vs 300 ms on stock PG (300×); 1 of 1 warm cells faster than stock PG (`v6.0`).', text)
        self.assertIn('| `stock_default` | **1 / 0 / 0** | **1 / 0 / 0** | 300 ms vs **1.00 ms** |', text)

    def test_the_clickbench_table_is_the_geomean_of_each_arm(self):
        def results(d):
            rows = [dict(case=c, arm=a, metric=m, value=v) for c in ('Q0', 'Q1') for a, w in (('pg', 10), ('sift_table', 1)) for m, v in (('warm_ms', w), ('md5_equal', 1))]
            resultfile.write(os.path.join(d, 'clickbench.tsv'), 'clickbench', dict(tier='5M', source='synthetic', date='2026-12-01'), rows)
        text = self.render(results)
        self.assertIn('| `sift_table` | 1.00 ms | — | 2 of 2 | 2 / 2 |', text)
        self.assertIn('| PG 18 | 10.0 ms | — | 0 of 2 | 2 / 2 |', text)
        self.assertIn('`sift_table` has the lowest warm geomean over 2 queries', text)

    def test_the_storage_notes_follow_the_size_rows(self):
        with tempfile.TemporaryDirectory() as d:
            sizes = [dict(engine=e, **{'class': '-'}, variant=v, metric='size_mib', value=x) for e, v, x in (
                ('ch', '-', 400.0), ('sift_table', '-', 500.0), ('citus', '-', 450.0), ('sift', '-', 1300.0), ('es', '-', 1200.0), ('pg', '-', 1500.0), ('sift', 'table', 1000.0), ('pg', 'table', 1000.0))]
            store_json(os.path.join(d, 'store-scaling.json'), sizes + [latency(e, c, 1.0) for c in readme_charts.QUERY_SLUG.values() for e in ('sift', 'ch', 'pg', 'es')])
            notes = {label: note for label, _, _, note in readme_charts.load_live_data(d)['storage_orders']}
        self.assertEqual(notes['pg_sift (sift_col + heap)'], '1,000 MiB heap + 300 idx')
        self.assertEqual(notes['PostgreSQL 18 (stock)'], '1,000 MiB heap + 500 idx')


class BenchmarksBlocks(unittest.TestCase):
    def page(self, root, *names):
        """every page of docs/benchmarks/ with the blocks of `names` it holds (render reads all of them); returns the text of them all"""
        os.makedirs(os.path.join(root, 'docs', 'results'), exist_ok=True)
        os.makedirs(os.path.join(root, 'docs', 'benchmarks'), exist_ok=True)
        for slug, blocks in benchmarks.PAGES.items():
            open(os.path.join(root, 'docs', 'benchmarks', slug + '.md'), 'w').close()
            for name in (n for n in names if n in blocks):
                resultfile.replace_block(os.path.join(root, 'docs', 'benchmarks', slug + '.md'), name, ['committed'])

    def text(self, root):
        return ''.join(open(os.path.join(root, 'docs', 'benchmarks', slug + '.md')).read() for slug in benchmarks.PAGES)

    def test_a_cross_engine_block_without_its_results_keeps_the_committed_text(self):
        with tempfile.TemporaryDirectory() as root:
            self.page(root, *benchmarks.BLOCKS)
            for name in ('v6-orders-10m', 'v6-search-warm', 'v6-size-orders'):
                self.assertIsNone(benchmarks.BLOCKS[name](root))
            benchmarks.render(root)
            text = self.text(root)
        self.assertIn('<!-- bench:v6-orders-10m:begin -->\ncommitted\n<!-- bench:v6-orders-10m:end -->', text)
        self.assertIn('| `ivm` | **32 / 2 / 30** | **34 / 3 / 27** | 43.3 ms vs **2.18 ms** | 58.4 ms vs **1.52 ms** |', text)

    def test_an_arm_that_ran_is_a_column_of_the_orders_tables(self):
        with tempfile.TemporaryDirectory() as root:
            self.page(root, 'v6-orders-10m')
            store_json(os.path.join(root, 'docs', 'results', 'store-scaling.json'),
                       [latency(e, s, 1.0) for s in readme_charts.QUERY_SLUG.values() for e in ('sift', 'sift_table', 'ch', 'ch_proj', 'es', 'pg')])
            benchmarks.render(root)
            head = self.text(root).split('\n')[1]
        self.assertEqual(head, '| Query (10M orders) | `sift_col` | `sift_table` | ClickHouse | ClickHouse + PROJECTION | Elasticsearch | PG 18 |')

    def test_the_6_0_columns_are_tbd_until_v6_core_exists_then_the_numbers(self):
        with tempfile.TemporaryDirectory() as root:
            self.page(root, 'v6-scope-build', 'v6-memory', 'v6-scaling')
            benchmarks.render(root)
            before = self.text(root)
            core = open(readme_charts.MEMORY_BASELINE).read().replace('\t1.79\tx\t', '\t1.10\tx\t')
            with open(os.path.join(root, 'docs', 'results', 'v6-core.tsv'), 'w') as f:
                f.write(core)
            benchmarks.render(root)
            after = self.text(root)
        self.assertIn('| Plain scoped `CREATE INDEX` wall time | 1.79× entity | ≤ 1.15× entity | *TBD* |', before)
        self.assertIn('| Plain scoped `CREATE INDEX` wall time | 1.79× entity | ≤ 1.15× entity | 1.10× entity |', after)
        self.assertNotIn('*TBD*', after)
        self.assertIn('*Not measured for 6.0 yet: `bench/bench scaling-io --docs`', before)

    def test_the_scaling_block_is_the_warm_ladder_and_the_cold_throughput(self):
        columns = scaling_io.COLUMNS
        rows = [dict.fromkeys(columns, '') | dict(suite='A', probe='A1_scan_filter', workers=w, speedup=sp, gate_pass='-', io_method='-', read_stream='-') for w, sp in ((1, '1.00'), (4, '3.60'))]
        rows += [dict.fromkeys(columns, '') | dict(suite='B', probe='B1_cold_wide_scan', workers=4, io_method=m, read_stream='on', throughput=t, gate_pass='pass') for m, t in (('sync', '900'), ('io_uring', '2400'))]
        with tempfile.TemporaryDirectory() as root:
            self.page(root, 'v6-scaling')
            with open(os.path.join(root, 'docs', 'results', 'v6-scaling.tsv'), 'w') as f:
                f.write('# cpu=test\n' + '\t'.join(columns) + '\n' + ''.join('\t'.join(str(r[c]) for c in columns) + '\n' for r in rows))
            benchmarks.render(root)
            text = self.text(root)
        self.assertIn('| A1_scan_filter | 1.00× | 3.60× |', text)
        self.assertIn('| B1_cold_wide_scan | 2,400 MB/s (4w) | 900 MB/s (4w) |', text)

    def test_every_block_of_the_pages_has_a_renderer_and_sits_on_its_page(self):
        for slug, names in benchmarks.PAGES.items():
            with open(os.path.join(ROOT, 'docs', 'benchmarks', slug + '.md')) as f:
                found = set(re.findall(r'<!-- bench:(v6-[\w-]+):begin', f.read()))
            self.assertEqual(found, set(names), slug)
        self.assertEqual({n for names in benchmarks.PAGES.values() for n in names}, set(benchmarks.BLOCKS))


def write_cases(path, ratios):
    """a docs/results/cases.tsv shaped file: per case, the sift lane at 1 ms and each rival lane at sift ms × its ratio"""
    rows = [dict(case=case, arm=arm, metric=m, value=v) for case, rivals in ratios.items() for arm, ms in (('sift', 1.0), *((a, r) for a, r in rivals.items()))
            for m, v in (('ms', ms), ('minstr', ms * 10), ('buffers', 100))]
    resultfile.write(path, 'cases', dict(orders=300000, postgresql='18.6', pg_sift='6.0', build='abcdef', date='2026-12-01'), rows)


class PgQueries(unittest.TestCase):
    def data(self, ratios=None):
        with tempfile.TemporaryDirectory() as d:
            if ratios:
                write_cases(os.path.join(d, 'cases.tsv'), ratios)
            return readme_charts.load_live_data(d)

    def svgs(self, data):
        return {mode: pg_queries.render(data, mode) for mode in ('light', 'dark')}   # render runs Canvas.check: no label overlaps or leaves the canvas

    def every_case(self):
        ratios = {r['case']: {'native': 50.0} for r in chart_data.baseline()['pg_cases']['rows']}
        ratios['jsonb_path_exists_gin'] = {'native': 40.0, 'gin': 0.5}
        ratios['jsonb_typeof_groupby'] = {'native': 9.0, 'expr_btree': 3.0}
        ratios['keyset_page'] = {'native': 20.0, 'btree': 0.4}
        return ratios

    def test_without_results_the_24_baseline_cases_are_measured_and_the_17_new_ones_pending(self):
        data = self.data()
        rows = data['pg_cases']['rows']
        self.assertEqual((len(rows), len(chart_data.measured(rows))), (41, 24))
        self.assertEqual({r['case'] for r in rows if r['ratio'] is None and r['sift']['ms'] is None and r['pg']['ms'] is None}, {r['case'] for r in rows} - {
            r['case'] for r in chart_data.measured(rows)})
        svg = self.svgs(data)['light']
        self.assertIn('4 new 6.0 cases · pending bench/bench cases --headline --docs', svg)
        self.assertIn('bm25_hybrid_groupby pending', svg)
        self.assertEqual(svg.count('pending 6.0 run'), 1)

    def test_every_case_measured_picks_the_fastest_rival_and_a_gin_win_is_amber(self):
        data = self.data(self.every_case())
        self.assertEqual(data['pg_cases']['meta']['source'], 'live')
        rows = {r['case']: r for r in data['pg_cases']['rows']}
        self.assertEqual((rows['jsonb_path_exists_gin']['against'], rows['jsonb_path_exists_gin']['ratio']), ('GIN', 0.5))
        self.assertEqual((rows['jsonb_typeof_groupby']['against'], rows['jsonb_typeof_groupby']['ratio']), ('expr b-tree', 3.0))
        svg = self.svgs(data)
        self.assertNotIn('pending', svg['light'])
        amber = theme.PALETTE['light']['amber']
        self.assertIn('<circle cx="%s" cy="88" r="5" fill="%s"/>' % (theme.n(pg_queries.X(0.5)), amber), svg['light'])

    def test_a_run_of_some_cases_overlays_them_on_the_baseline(self):
        data = self.data({'bm25_hybrid_groupby': {'native': 30.0, 'gin': 6.0}, 'softdel_live_groupby': {'native': 100.0}})
        meta, rows = data['pg_cases']['meta'], {r['case']: r for r in data['pg_cases']['rows']}
        self.assertEqual((meta['source'], meta['live']), ('mixed', 2))
        self.assertEqual((rows['bm25_hybrid_groupby']['ratio'], rows['softdel_live_groupby']['ratio'], rows['lateral_first_row']['ratio']), (6.0, 100.0, 347))
        self.assertEqual(len(chart_data.measured(data['pg_cases']['rows'])), 25)
        self.svgs(data)

    def test_the_readme_table_is_one_flagship_per_family(self):
        for ratios, pending in ((None, 1), (self.every_case(), 0)):
            with tempfile.TemporaryDirectory() as root:
                os.makedirs(os.path.join(root, 'docs', 'results'))
                if ratios:
                    write_cases(os.path.join(root, 'docs', 'results', 'cases.tsv'), ratios)
                lines = readme.pg_queries(readme_charts.load_live_data(os.path.join(root, 'docs', 'results')), root)
            table = [l for l in lines if l.startswith('| ') and not l.startswith('| PostgreSQL shape')]
            self.assertEqual(len(table), 6)
            self.assertTrue(all(l.count(' | ') == 4 for l in table))
            self.assertEqual(sum('*pending 6.0 run*' in l for l in table), pending)

    def test_the_docs_table_groups_the_41_cases_by_family(self):
        with tempfile.TemporaryDirectory() as root:
            lines = benchmarks.render_pg_queries(root)
        self.assertEqual(sum(l.startswith('| `') for l in lines), 41)
        self.assertEqual(sum('*pending 6.0 run*' in l for l in lines), 17)
        self.assertEqual([l for l in lines if l.startswith('**')][3], '**Text, Search & Native Types** (4 cases)')


class ReadStreamSwitch(unittest.TestCase):
    class Pg:
        def __init__(self, registered):
            self.registered, self.sql = registered, []

        def query_scalar(self, sql):
            self.sql.append(sql)
            return '1' if self.registered else '0'

    def test_only_a_build_that_registers_the_setting_has_the_switch(self):
        self.assertTrue(scaling_io.has_read_stream_switch(self.Pg(True)))
        self.assertFalse(scaling_io.has_read_stream_switch(self.Pg(False)))

    def test_the_probe_loads_the_library_first(self):
        pg = self.Pg(True)
        scaling_io.has_read_stream_switch(pg)
        self.assertTrue(pg.sql[0].startswith("LOAD 'pg_sift';"))


class GenDocsFiles(unittest.TestCase):
    def test_the_worktree_files_are_the_tracked_the_new_and_not_the_ignored(self):
        spec = importlib.util.spec_from_file_location('gen_docs', os.path.join(ROOT, 'scripts', 'gen-docs.py'))
        gen_docs = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gen_docs)
        with tempfile.TemporaryDirectory() as repo:
            git = lambda *a: subprocess.run(['git', '-C', repo, *a], check=True, capture_output=True)
            git('init', '-q')
            for name, text in (('tracked.py', 'a'), ('new.py', 'b'), ('junk.pyc', 'c'), ('.gitignore', '*.pyc\n')):
                with open(os.path.join(repo, name), 'w') as f:
                    f.write(text)
            git('add', 'tracked.py')
            self.assertEqual(sorted(os.fsdecode(f) for f in gen_docs.worktree_files(repo)), ['.gitignore', 'new.py', 'tracked.py'])


if __name__ == '__main__':
    unittest.main()
