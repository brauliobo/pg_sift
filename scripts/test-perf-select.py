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
        cls.jobs = SELECT.job_names(ROOT)

    def selected(self, *paths):
        return SELECT.select(paths, ROOT / 'scripts/areas.tsv', self.jobs)

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
            'scripts/port-isa-check.sh': ['smoke'],
        }.items():
            with self.subTest(path=path):
                self.assertEqual(self.selected(path), expected)

    def test_shared_and_unknown(self):
        for path in ('src/core/sift_store.c', 'src/core/sift_codec.h',
                     'src/portable.list', 'Makefile', 'src/areas.mk',
                     'pg_sift--3.0.sql', 'pg_sift.control', 'sql/pg_sift--3.0.sql',
                     'scripts/pgtest.sh', 'scripts/areas.tsv', 'scripts/areas.sh', 'new/unknown.c',
                     '.github/workflows/performance.yml', 'scripts/perf-select.py',
                     'bench/steps/perf_gate.py', 'bench/perf/cases.json', 'bench/perf/test_insert_trajectory.py'):
            with self.subTest(path=path):
                self.assertEqual(self.selected(path), list(SELECT.ALL))

    def test_docs_and_unions(self):
        self.assertEqual(self.selected(), [])
        self.assertEqual(self.selected('README.md', 'docs/DESIGN.md', 'LICENSE'), [])
        self.assertEqual(self.selected('README.md', 'sqlite/new.c'), ['smoke'])
        self.assertEqual(self.selected('src/table/a.c', 'test/sql/count_scan.sql'),
                         ['smoke', 'bm25', 'storage'])

    def fixture(self, groups, rows, path, jobs, expected):
        with tempfile.TemporaryDirectory(prefix='areas-parity-') as tmp:
            mapping = Path(tmp) / 'areas.tsv'
            mapping.write_text(''.join('='+k+'\t'+v+'\n' for k,v in groups.items()) +
                               ''.join(k+'\t'+v+'\n' for k,v in rows))
            self.assertEqual(SELECT.select([path], mapping, jobs), expected)
            # The actual CI consumer and shared CLI must emit identical EREs.
            ci = subprocess.check_output(['bash',str(ROOT/'scripts/ci.sh'),'--areas-regex',str(mapping),path],text=True)
            shared = subprocess.check_output(['bash',str(ROOT/'scripts/areas.sh'),'--regex',str(mapping),path],text=True)
            self.assertEqual(ci,shared)
            # Match the resulting expression with Bash, never Python re.
            affected = subprocess.check_output(['bash','-c',
                're=$1; shift; for job in "$@"; do [[ $job =~ $re ]] && echo "$job"; done; :',
                'parity',ci.strip(),*sorted(jobs)],text=True).splitlines()
            return affected

    def test_map_is_authoritative(self):
        groups = {'bm25':'^search$', 'col':'^column$', 'agg':'^aggregate$', 'tbl':'^table$'}
        self.fixture(groups, [('^relocated/','{tbl}')], 'relocated/a.c', {'table'}, ['smoke','storage'])
        self.fixture(groups, [('^relocated/','{tbl}')], 'unknown.c', {'table'}, list(SELECT.ALL))

    def test_bash_ere_contract(self):
        groups = {'bm25': '^installcheck-ab-x-y$', 'col': '^installcheck-col_[[:digit:]]{2,3}$',
                  'agg': '^aggregate$', 'tbl': '^installcheck-a--$'}
        jobs = {'installcheck-ab-x-y','installcheck-a--','installcheck-col_42'}
        fixtures = [
            ('src/table/a.c', '^src/([[:alpha:]]+)/', '{tbl}', ['smoke','storage'], ['installcheck-a--']),
            ('numeric/a.c', '^numeric/', '^installcheck-col_[0-9]{2}$', ['smoke','agg','storage'], ['installcheck-col_42']),
            ('numeric/a.c', '^numeric/', '^installcheck-col_[[:digit:]]{2,3}$', ['smoke','agg','storage'], ['installcheck-col_42']),
            ('test/abxy', '^test/(a|ab)(x)?(y)?', r'^installcheck-\1-\2-\3$', ['smoke','bm25'], ['installcheck-ab-x-y']),
            ('test/a', '^test/(a|ab)(x)?(y)?$', r'^installcheck-\1-\2-\3$', ['smoke','storage'], ['installcheck-a--']),
        ]
        for path,pattern,value,expected,affected in fixtures:
            with self.subTest(path=path,pattern=pattern):
                self.assertEqual(self.fixture(groups, [(pattern,value)], path, jobs, expected), affected)

    def test_explicit_and_residual_owners_and_markers(self):
        groups = {'bm25':'^shared$', 'col':'^(shared|column)$', 'agg':'^agg$', 'tbl':'^(shared|table)$', 'other':'^column$', 'perf':'^gh:perf$'}
        for rhs, expected in (('{tbl}', ['smoke','storage']),
                              ('{tbl}|{other}', ['smoke','agg','storage']),
                              ('^shared$', list(SELECT.ALL)),
                              ('.',list(SELECT.ALL)), ('{perf}',list(SELECT.ALL)), ('-',['smoke'])):
            with self.subTest(rhs=rhs):
                self.fixture(groups, [('^path/',rhs)], 'path/a', {'shared','table','column'}, expected)
        self.fixture(groups, [('^path/','{tbl}'), ('/a$','{agg}')], 'path/a', {'table','agg'}, ['smoke','agg','storage'])
        for capture in (1,2,3):
            rows = [('^(shared)(shared)(shared)$', '^'+chr(92)+str(capture)+'$')]
            self.fixture(groups, rows, 'sharedsharedshared', {'shared'}, list(SELECT.ALL))

    def test_actual_ci_capture_expansion(self):
        for path,expected in {
            'test/sql/col_search.sql':'^$|^installcheck-col_search$',
            'test/t/001_crash.pl':r'^$|^tap(-shell)?-001_crash$',
            'test/expected/count_scan_1.out':'^$|^installcheck-count_scan_1$|^installcheck-count_scan$',
            'scripts/source-snapshot.sh':'^$|^(hardened|assert|valgrind)$',
            'scripts/areas.sh':'^$|.',
        }.items():
            with self.subTest(path=path):
                actual = subprocess.check_output(['bash',str(ROOT/'scripts/ci.sh'),'--areas-regex',
                                                   str(ROOT/'scripts/areas.tsv'),path],text=True).strip()
                self.assertEqual(actual,expected)

    def test_invalid_ere_is_error_in_both_consumers(self):
        with tempfile.TemporaryDirectory(prefix='areas-invalid-') as tmp:
            mapping = Path(tmp)/'areas.tsv'
            mapping.write_text('=tbl\t^table$\n[invalid\t{tbl}\n')
            for command in ([str(ROOT/'scripts/ci.sh'),'--areas-regex',str(mapping),'path'],
                            [str(ROOT/'scripts/areas.sh'),'--owners',str(mapping),'tbl','--','path']):
                result = subprocess.run(['bash',*command],input='',text=True,capture_output=True)
                self.assertEqual(result.returncode,2)
                self.assertIn('invalid ERE',result.stderr)

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
