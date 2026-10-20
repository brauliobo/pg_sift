#!/usr/bin/env python3
"""Exact dependency selections and real Git rename/deletion semantics (no builds)."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('perf_select', ROOT / 'scripts/perf-select.py')
SELECT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SELECT)


class SelectionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.groups, cls.rows = SELECT.read_map(ROOT / 'scripts/areas.tsv')
        cls.jobs = SELECT.job_names(ROOT)

    def selected(self, *paths):
        return SELECT.select(paths, self.groups, self.rows, self.jobs)

    def test_exact_owners(self):
        for path, expected in {
            'src/bm25/sift_query.c': list(SELECT.ALL),
            'src/col/sift_colz.c': ['smoke', 'agg', 'storage'],
            'src/agg/sift_agg.c': ['smoke', 'agg', 'storage'],
            'src/table/sift_table.c': ['smoke', 'storage'],
            'test/sql/col_search.sql': ['smoke', 'agg', 'storage'],
            'test/sql/count_scan.sql': ['smoke', 'bm25'],
            'sqlite/host/host.h': ['smoke'],
            '.github/workflows/assert.yml': ['smoke'],
            'scripts/source-snapshot.sh': ['smoke'],
        }.items():
            with self.subTest(path=path):
                self.assertEqual(self.selected(path), expected)

    def test_shared_and_unknown(self):
        for path in ('src/core/sift_store.c', 'src/core/sift_codec.h',
                     'src/portable.list', 'Makefile', 'src/areas.mk',
                     'pg_sift--3.0.sql', 'pg_sift.control', 'sql/pg_sift--3.0.sql',
                     'scripts/pgtest.sh', 'scripts/areas.tsv', 'new/unknown.c',
                     '.github/workflows/performance.yml', 'scripts/perf-select.py',
                     'bench/steps/perf_gate.py', 'bench/perf/cases.json'):
            with self.subTest(path=path):
                self.assertEqual(self.selected(path), list(SELECT.ALL))

    def test_docs_and_unions(self):
        self.assertEqual(self.selected(), [])
        self.assertEqual(self.selected('README.md', 'docs/USAGE.md', 'LICENSE'), [])
        self.assertEqual(self.selected('README.md', 'sqlite/new.c'), ['smoke'])
        self.assertEqual(self.selected('src/table/a.c', 'test/sql/count_scan.sql'),
                         ['smoke', 'bm25', 'storage'])

    def test_map_is_authoritative(self):
        groups = {'bm25': '^search$', 'col': '^column$', 'agg': '^aggregate$', 'tbl': '^table$'}
        rows = [(SELECT.re.compile('^relocated/'), '{tbl}')]
        self.assertEqual(SELECT.select(['relocated/file.c'], groups, rows, set()),
                         ['smoke', 'storage'])
        self.assertEqual(SELECT.select(['unknown.c'], groups, rows, set()), list(SELECT.ALL))

    def test_cli_tiers(self):
        sha = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
        with tempfile.TemporaryDirectory(prefix='perf-select-output-') as tmp:
            output = Path(tmp) / 'selection.json'
            for tier, expected in (('pr', []), ('nightly', list(SELECT.ALL))):
                result = subprocess.check_output([
                    'python3', str(ROOT / 'scripts/perf-select.py'),
                    '--baseline-ref', sha, '--candidate-ref', sha,
                    '--tier', tier, '--output', str(output)], text=True)
                selection = SELECT.json.loads(output.read_text())
                self.assertEqual(selection['groups'], expected)
                self.assertEqual(selection['candidate_ref'], sha)
                self.assertIn('run=' + str(bool(expected)).lower(), result)

    def test_rename_and_deletion(self):
        with tempfile.TemporaryDirectory(prefix='perf-select-test-') as tmp:
            root = Path(tmp)
            def git(*args):
                return subprocess.check_output(['git', '-C', tmp, *args], text=True).strip()
            git('init', '-q')
            git('config', 'user.email', 'test@example.invalid')
            git('config', 'user.name', 'Selector test')
            (root / 'src/table').mkdir(parents=True)
            (root / 'src/table/old.c').write_text('source\n')
            git('add', 'src/table/old.c')
            git('commit', '-qm', 'test: initial fixture')
            baseline = git('rev-parse', 'HEAD')
            git('mv', 'src/table/old.c', 'README.md')
            git('commit', '-qm', 'test: rename fixture')
            paths = SELECT.changed_paths(root, baseline, 'HEAD')
            self.assertEqual(sorted(paths), ['README.md', 'src/table/old.c'])
            self.assertEqual(self.selected(*paths), ['smoke', 'storage'])
            git('rm', 'README.md')
            git('commit', '-qm', 'test: delete fixture')
            self.assertEqual(SELECT.changed_paths(root, 'HEAD~1', 'HEAD'), ['README.md'])
            self.assertEqual(self.selected(*SELECT.changed_paths(root, baseline, 'HEAD')),
                             ['smoke', 'storage'])


if __name__ == '__main__':
    unittest.main()
