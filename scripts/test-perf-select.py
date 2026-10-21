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
            'src/bm25/query.c': list(SELECT.ALL),
            'src/col/compress.c': ['smoke', 'agg', 'storage'],
            'src/agg/sift_agg.c': ['smoke', 'agg'],
            'src/agg/sift_aggplan.c': ['smoke', 'agg'],
            'src/table/am.c': ['smoke', 'storage'],
            'sqlite/host/host.h': ['smoke'],
            '.github/workflows/assert.yml': [],
            'scripts/source-snapshot.sh': ['smoke'],
            'scripts/port-isa-check.sh': ['smoke'],
        }.items():
            with self.subTest(path=path):
                self.assertEqual(self.selected(path), expected)

    def test_shared_and_unknown(self):
        for path in ('src/core/store.c', 'src/core/sift_codec.h',
                     'src/portable.list', 'Makefile', 'src/areas.mk',
                     'pg_sift--3.0.sql', 'pg_sift.control', 'sql/pg_sift--3.0.sql',
                     'scripts/harden.mk', 'new/unknown.c'):
            with self.subTest(path=path):
                self.assertEqual(self.selected(path), list(SELECT.ALL))

    def test_paths_that_cannot_change_the_extension_select_nothing(self):
        # Both revisions would build identical extensions: a paired run measures only noise.
        for path in ('scripts/pgtest.sh', 'scripts/ci.sh', 'scripts/areas.tsv', 'scripts/areas.sh',
                     'test/t/lib/SiftTest.pm', '.github/workflows/ci.yml',
                     '.github/workflows/performance.yml', 'scripts/perf-select.py', 'scripts/test-perf-select.py',
                     'bench/steps/perf_gate.py', 'bench/perf/catalog.py', 'bench/perf/test_insert_trajectory.py',
                     'bench/steps/store.py', 'Formula/pg_sift.rb',
                     'test/sql/col_search.sql', 'test/sql/count_scan.sql', 'test/expected/count_scan_1.out',
                     'test/col/fixtures/k_keys_9.sql', 'test/isolation/specs/agg_correlated_snapshot.spec',
                     'test/t/001_crash.pl', 'test/fixtures/compat30/a', 'test/fuzz/a', 'test/diff/a'):
            with self.subTest(path=path):
                self.assertEqual(self.selected(path), [])

    def test_one_buildable_path_keeps_the_gate_armed(self):
        # A harness change never hides a real source change in the same diff.
        self.assertEqual(self.selected('scripts/ci.sh', 'bench/perf/catalog.py', 'src/table/am.c'),
                         ['smoke', 'storage'])
        self.assertEqual(self.selected('scripts/areas.sh', 'src/core/store.c'), list(SELECT.ALL))
        self.assertEqual(self.selected('scripts/ci.sh', 'sqlite/new.c'), ['smoke'])
        self.assertEqual(self.selected('test/sql/count_scan.sql', 'src/table/am.c'), ['smoke', 'storage'])

    def test_aggregate_change_with_its_tests_selects_only_the_aggregate_group(self):
        # The diff of an aggregate PR: sources, tests of every kind, fixtures and documentation.
        self.assertEqual(self.selected(
            'src/agg/sift_agg.c', 'src/agg/sift_aggspec.h', 'docs/AGG.md', 'README.md', 'test/sql/agg_semi_lut.sql',
            'test/expected/agg_semi_lut.out', 'test/col/fixtures/k_keys_9.sql',
            'test/isolation/specs/agg_correlated_snapshot.spec'), ['smoke', 'agg'])
        # Only the aggregate sources keep the aggregate stack to the aggregate group: the column sources feed the table readers.
        self.assertEqual(self.selected('src/col/compress.c', 'test/sql/agg_semi_lut.sql'), ['smoke', 'agg', 'storage'])

    def test_aggregate_change_still_runs_every_correctness_job_of_the_stack(self):
        # The focused gate ignores {ci}; the CI jobs of the column and table groups keep running for an aggregate change.
        regex = subprocess.check_output(['bash', str(ROOT/'scripts/ci.sh'), '--areas-regex', str(ROOT/'scripts/areas.tsv'),
                                         'src/agg/sift_agg.c'], text=True).strip()
        jobs = ['installcheck-agg_semi_lut', 'installcheck-col_search', 'installcheck-tbl_cap', 'difftest-col', 'isolation', 'fuzz', 'installcheck-count_scan']
        picked = subprocess.check_output(['bash', '-c', 're=$1; shift; for j in "$@"; do [[ $j =~ $re ]] && echo "$j"; done; :', 'x', regex, *jobs], text=True).split()
        self.assertEqual(picked, jobs[:5])

    def test_docs_and_unions(self):
        self.assertEqual(self.selected(), [])
        self.assertEqual(self.selected('README.md', 'docs/USAGE.md', 'LICENSE'), [])
        self.assertEqual(self.selected('README.md', 'sqlite/new.c'), ['smoke'])
        self.assertEqual(self.selected('src/table/a.c', 'scripts/old-lib-check.sh'),
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
                              ('.',list(SELECT.ALL)), ('.|{perf}',[]), ('{perf}',[]), ('-',[])):
            with self.subTest(rhs=rhs):
                self.fixture(groups, [('^path/',rhs)], 'path/a', {'shared','table','column'}, expected)
        self.fixture(groups, [('^path/','{tbl}'), ('/a$','{agg}')], 'path/a', {'table','agg'}, ['smoke','agg','storage'])
        for capture in (1,2,3):
            rows = [('^(shared)(shared)(shared)$', '^'+chr(92)+str(capture)+'$')]
            self.fixture(groups, rows, 'sharedsharedshared', {'shared'}, list(SELECT.ALL))

    def test_actual_ci_capture_expansion(self):
        for path,expected in {
            'test/sql/col_search.sql':'^$|^installcheck-col_search$|(^gh:ci$)',
            'test/t/001_crash.pl':r'^$|^tap(-shell)?-001_crash$|(^gh:ci$)',
            'test/expected/count_scan_1.out':'^$|^installcheck-count_scan_1$|(^gh:ci$)|^installcheck-count_scan$|(^gh:ci$)',
            'scripts/source-snapshot.sh':'^$|^(hardened|assert|valgrind)$',
            'scripts/areas.sh':'^$|.|(^gh:perf$)',
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
