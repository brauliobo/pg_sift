#!/usr/bin/env python3
"""The affected-area router: the areas, plangate slices and scaling checks of a change, the Bash ERE engine it shares with ci.sh, and Git rename/deletion semantics (no build, no PostgreSQL)."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import os, sys  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../bench/steps'))
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)

ROOT = Path(__file__).resolve().parents[1]
MAP = ROOT / 'scripts/areas.tsv'
SPEC = importlib.util.spec_from_file_location('perf_select', ROOT / 'scripts/perf-select.py')
SELECT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SELECT)
SEVEN = [area for area in SELECT.AREAS if area != 'smoke']


def bash(*argv, stdin=''):
    return subprocess.check_output(['bash', *map(str, argv)], input=stdin, text=True)


class SelectionTest(unittest.TestCase):
    def check(self, paths, groups, slices=None, run=True):
        picked = SELECT.select(paths, MAP)
        self.assertEqual((picked['groups'], picked['plangate_slices'], picked['run']), (groups, groups if slices is None else slices, run), paths)
        return picked

    def test_docs_only_zero_work(self):
        self.check(['README.md', 'docs/AGG.md', 'CHANGELOG.md', 'LICENSE'], [], run=False)
        self.check([], [], run=False)

    def test_tests_and_harness_only_zero_work(self):
        picked = self.check(['test/sql/agg_v6_core_suite.sql', 'scripts/ci.sh', 'scripts/perf/gate.py', 'scripts/perf/canaries.py', '.github/workflows/performance.yml',
                             'bench/steps/perf_gate/run.py', 'scripts/perf-select.py', 'scripts/test-perf-select.py', 'scripts/areas.tsv', 'test/t/001_crash.pl'], [], run=False)
        self.assertEqual((picked['ab_groups'], picked['scaling_checks'], SELECT.commands(picked)), ([], [], {'gate': [], 'plangate': []}))

    def test_plan_files_run_the_plangate_smoke_slice_only(self):
        picked = self.check(['test/plan/shapes.sql', 'scripts/plangate.sh'], [], ['smoke'], run=False)
        self.assertEqual(picked['plangate_lanes'], ['stock', 'stock-4t'])

    def test_single_star_edit(self):
        self.check(['src/agg/v6/sip.c'], ['star'])
        self.check(['src/agg/v6/star/plan.c'], ['star'])

    def test_single_clickbench_edit(self):
        self.check(['src/agg/v6/topn/topn.c'], ['clickbench'])

    def test_single_coexist_cost_edit(self):
        self.check(['src/agg/cost/cost.c'], ['coexist', 'smoke'])

    def test_single_jsonb_edit(self):
        self.check(['src/col/shred/shred.c'], ['jsonb'])

    def test_single_table_edit(self):
        self.check(['src/table/f2/bulk_ins.c'], ['table'])

    def test_single_bm25_edit(self):
        self.check(['src/bm25/rank/search.c'], ['bm25'])

    def test_single_gpu_edit(self):
        self.check(['src/agg/gpu/v6.c'], ['gpu'])

    def test_parallel_framework_edit(self):
        self.check(['src/agg/par/morsel.c'], ['star', 'clickbench'])

    def test_shared_core_bounded_smoke(self):
        picked = self.check(['src/core/store/store.c', 'Makefile', 'sql/pg_sift--6.0.sql'], ['smoke'])
        self.assertEqual(picked['plangate_lanes'], ['stock', 'stock-4t'])

    def test_unmapped_new_src_failsafe(self):
        self.check(['src/new_subsystem/foo.c'], ['smoke'])
        self.check(['src/agg/exec/run.c'], ['smoke'])

    def test_multi_subsystem_union(self):
        self.check(['src/table/f2/scan.c', 'src/col/shred/shred.c', 'docs/USAGE.md'], ['jsonb', 'table'])

    def test_harness_never_hides_a_source_change(self):
        self.check(['scripts/ci.sh', 'bench/perf/catalog.py', 'test/sql/col_search.sql', 'src/table/am.c'], ['table'])

    def test_git_rename_deletion_no_renames(self):
        with tempfile.TemporaryDirectory(prefix='perf-select-test-') as tmp:
            def git(*args):
                return subprocess.check_output(['git', '-C', tmp, *args], text=True).strip()
            git('init', '-q')
            git('config', 'user.email', 'test@example.invalid')
            git('config', 'user.name', 'Selector test')
            (Path(tmp) / 'src/table').mkdir(parents=True)
            (Path(tmp) / 'src/table/old.c').write_text('source\n')
            git('add', 'src/table/old.c')
            git('commit', '-qm', 'test: initial fixture')
            baseline = git('rev-parse', 'HEAD')
            git('mv', 'src/table/old.c', 'README.md')
            git('commit', '-qm', 'test: rename fixture')
            paths = SELECT.changed_paths(tmp, baseline, 'HEAD')
            self.assertEqual(sorted(paths), ['README.md', 'src/table/old.c'])
            self.check(paths, ['table'])
            git('rm', 'README.md')
            git('commit', '-qm', 'test: delete fixture')
            self.assertEqual(SELECT.changed_paths(tmp, 'HEAD~1', 'HEAD'), ['README.md'])
            self.check(SELECT.changed_paths(tmp, baseline, 'HEAD'), ['table'])

    def test_nightly_tier_full(self):
        picked = SELECT.select([], MAP, 'nightly')
        self.assertEqual((picked['groups'], picked['plangate_slices'], picked['run']), (SEVEN, SEVEN, True))
        self.assertEqual(picked['plangate_lanes'], list(SELECT.LANE_FLAGS))
        self.assertEqual(picked['scaling_checks'], ['check-scaling-ops.sh --sql-only', 'tap-shell-scaling_slope'])
        self.assertEqual(SELECT.commands(picked)['gate'], [['python3', '-I', 'scripts/perf/gate.py', '--all']])

    def test_scaling_checks_follow_the_files_that_need_them(self):
        self.assertEqual(SELECT.select(['src/table/f2/dml.c'], MAP)['scaling_checks'], ['check-scaling-ops.sh --sql-only'])
        self.assertEqual(SELECT.select(['src/core/page.h'], MAP)['scaling_checks'], ['tap-shell-scaling_slope'])
        self.assertEqual(SELECT.select(['src/agg/v6/sip.c'], MAP)['scaling_checks'], [])

    def test_commands_run_only_the_area(self):
        gate = ['python3', '-I', 'scripts/perf/gate.py']
        for paths, expected in {
            ('src/agg/v6/sip.c',): {'gate': [gate + ['--areas', 'star']],
                                       'plangate': [['scripts/plangate.sh', '--runs', '0', '--slice', 'star'],
                                                    ['scripts/plangate.sh', '--runs', '0', '--lane', 'stock-4t', '--slice', 'star']]},
            ('src/table/f2/scan.c',): {'gate': [gate + ['--areas', 'table']], 'plangate': [['scripts/plangate.sh', '--runs', '0', '--table', '--slice', 'table']]},
            ('src/agg/gpu/v6.c',): {'gate': [gate + ['--areas', 'gpu']], 'plangate': [['scripts/plangate.sh', '--runs', '0', '--lane', 'gpu', '--slice', 'gpu']]},
            ('src/agg/cost/cost.c',): {'gate': [gate + ['--areas', 'coexist'], gate + ['--smoke']],
                                       'plangate': [['scripts/plangate.sh', '--runs', '0', '--slice', 'coexist,smoke'],
                                                    ['scripts/plangate.sh', '--runs', '0', '--lane', 'stock-4t', '--slice', 'coexist,smoke']]},
        }.items():
            with self.subTest(paths=paths):
                self.assertEqual(SELECT.commands(SELECT.select(paths, MAP)), expected)

    def test_the_catalog_groups_of_the_paired_runs(self):
        for path, expected in {'src/agg/v6/sip.c': ['smoke', 'agg'], 'src/table/am.c': ['smoke', 'storage'], 'src/bm25/rank/search.c': ['smoke', 'bm25'],
                               'src/core/store/store.c': ['smoke'], 'README.md': []}.items():
            with self.subTest(path=path):
                self.assertEqual(SELECT.select([path], MAP)['ab_groups'], expected)

    def test_ci_runs_the_perf_job_of_the_area_and_every_correctness_job_of_the_stack(self):
        regex = bash(ROOT / 'scripts/ci.sh', '--areas-regex', MAP, 'src/agg/v6/sip.c').strip()
        jobs = ['perf-gate-star', 'perf-gate-table', 'perf-gate-smoke', 'installcheck-agg_semi_lut', 'installcheck-col_search', 'installcheck-tbl_cap', 'difftest-col', 'isolation', 'fuzz']
        picked = bash('-c', 're=$1; shift; for j in "$@"; do [[ $j =~ $re ]] && echo "$j"; done; :', 'x', regex, *jobs).split()
        self.assertEqual(picked, ['perf-gate-star', 'installcheck-agg_semi_lut', 'installcheck-col_search', 'installcheck-tbl_cap', 'difftest-col', 'isolation'])

    def test_ci_runs_every_perf_job_for_the_gate_and_every_plan_lane_for_the_shapes(self):
        def picked(path, *jobs):
            regex = bash(ROOT / 'scripts/ci.sh', '--areas-regex', MAP, path).strip()
            return bash('-c', 're=$1; shift; for j in "$@"; do [[ $j =~ $re ]] && echo "$j"; done; :', 'x', regex, *jobs).split()
        self.assertEqual(picked('scripts/perf/gate.py', 'perf-gate-star', 'perf-gate-smoke', 'plan-gate'), ['perf-gate-star', 'perf-gate-smoke'])
        lanes = ['plan-gate', 'plan-gate-4t', 'plan-gate-4t-250k', 'plan-gate-tuned', 'plan-gate-table-tuned-250k', 'plan-gate-calibrate']
        self.assertEqual(picked('test/plan/shapes.sql', *lanes), lanes)

    def test_actual_ci_capture_expansion(self):
        for path, expected in {
            'test/sql/col_search.sql': '^$|^installcheck-col_search$|(^gh:ci$)',
            'test/t/001_crash.pl': r'^$|^tap(-shell)?-001_crash$|(^gh:ci$)',
            'test/expected/count_scan_1.out': '^$|^installcheck-count_scan_1$|(^gh:ci$)|^installcheck-count_scan$|(^gh:ci$)',
            'scripts/source-snapshot.sh': '^$|^(hardened|assert|valgrind)$',
            'scripts/areas.sh': '^$|.|(^gh:perf$)',
        }.items():
            with self.subTest(path=path):
                self.assertEqual(bash(ROOT / 'scripts/ci.sh', '--areas-regex', MAP, path).strip(), expected)

    def test_cli_tiers_and_files(self):
        sha = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
        script = ROOT / 'scripts/perf-select.py'
        with tempfile.TemporaryDirectory(prefix='perf-select-output-') as tmp:
            output = Path(tmp) / 'selection.json'
            for argv, groups in ((['--baseline-ref', sha, '--candidate-ref', sha], []), (['--baseline-ref', sha, '--candidate-ref', sha, '--tier', 'nightly'], SEVEN),
                                 (['--files', 'src/agg/v6/sip.c'], ['star']), (['--files', 'README.md'], [])):
                with self.subTest(argv=argv):
                    stdout = subprocess.check_output(['python3', str(script), *argv, '--output', str(output)], text=True)
                    selection = json.loads(output.read_text())
                    self.assertEqual(selection['groups'], groups)
                    self.assertIn('groups=' + ','.join(groups) + '\n', stdout)
                    self.assertIn('run=' + str(bool(groups)).lower(), stdout)
            self.assertNotEqual(subprocess.run(['python3', str(script)], capture_output=True).returncode, 0)


class EreEngineTest(unittest.TestCase):
    """scripts/areas.sh and scripts/ci.sh read one map with one Bash ERE engine: the same jobs, the same owners, an invalid ERE is an error of both."""
    def run_map(self, groups, rows, path, jobs, keys=None):
        with tempfile.TemporaryDirectory(prefix='areas-parity-') as tmp:
            mapping = Path(tmp) / 'areas.tsv'
            mapping.write_text(''.join('=' + k + '\t' + v + '\n' for k, v in groups.items()) + ''.join(k + '\t' + v + '\n' for k, v in rows))
            owners = bash(ROOT / 'scripts/areas.sh', '--owners', mapping, *(keys or groups), '--', path, stdin=''.join(job + '\0' for job in sorted(jobs))).split()
            ci = bash(ROOT / 'scripts/ci.sh', '--areas-regex', mapping, path)
            self.assertEqual(ci, bash(ROOT / 'scripts/areas.sh', '--regex', mapping, path))
            affected = bash('-c', 're=$1; shift; for job in "$@"; do [[ $job =~ $re ]] && echo "$job"; done; :', 'parity', ci.strip(), *sorted(jobs)).splitlines()
            return sorted(set(owners) - {'--'}), affected

    def test_bash_ere_parity_and_invalid_ere(self):
        groups = {'bm': '^installcheck-ab-x-y$', 'col': '^installcheck-col_[[:digit:]]{2,3}$', 'agg': '^aggregate$', 'tbl': '^installcheck-a--$'}
        jobs = {'installcheck-ab-x-y', 'installcheck-a--', 'installcheck-col_42'}
        for path, pattern, value, owners, affected in [
            ('src/table/a.c', '^src/([[:alpha:]]+)/', '{tbl}', ['tbl'], ['installcheck-a--']),
            ('numeric/a.c', '^numeric/', '^installcheck-col_[0-9]{2}$', ['col'], ['installcheck-col_42']),
            ('numeric/a.c', '^numeric/', '^installcheck-col_[[:digit:]]{2,3}$', ['col'], ['installcheck-col_42']),
            ('test/abxy', '^test/(a|ab)(x)?(y)?', r'^installcheck-\1-\2-\3$', ['bm'], ['installcheck-ab-x-y']),
            ('test/a', '^test/(a|ab)(x)?(y)?$', r'^installcheck-\1-\2-\3$', ['tbl'], ['installcheck-a--']),
        ]:
            with self.subTest(path=path, pattern=pattern, value=value):
                self.assertEqual(self.run_map(groups, [(pattern, value)], path, jobs), (owners, affected))
        with tempfile.TemporaryDirectory(prefix='areas-invalid-') as tmp:
            mapping = Path(tmp) / 'areas.tsv'
            mapping.write_text('=tbl\t^table$\n[invalid\t{tbl}\n')
            for command in ([ROOT / 'scripts/ci.sh', '--areas-regex', mapping, 'path'], [ROOT / 'scripts/areas.sh', '--owners', mapping, 'tbl', '--', 'path']):
                result = subprocess.run(['bash', *map(str, command)], input='', text=True, capture_output=True)
                self.assertEqual(result.returncode, 2)
                self.assertIn('invalid ERE', result.stderr)

    def test_explicit_and_residual_owners_and_markers(self):
        groups = {'bm': '^shared$', 'col': '^(shared|column)$', 'agg': '^agg$', 'tbl': '^(shared|table)$', 'other': '^column$', 'perf': '^gh:perf$'}
        jobs, keys = {'shared', 'table', 'column'}, ['bm', 'col', 'agg', 'tbl']
        for value, owners in (('{tbl}', ['tbl']), ('{tbl}|{other}', ['col', 'tbl']), ('^shared$', ['bm', 'col', 'tbl']), ('.', ['*']), ('.|{perf}', ['-']), ('{perf}', ['-']), ('-', ['-'])):
            with self.subTest(value=value):
                self.assertEqual(self.run_map(groups, [('^path/', value)], 'path/a', jobs, keys)[0], owners)
        self.assertEqual(self.run_map(groups, [('^path/', '{tbl}'), ('/a$', '{agg}')], 'path/a', {'table', 'agg'}, keys)[0], ['agg', 'tbl'])
        for capture in (1, 2, 3):
            rows = [('^(shared)(shared)(shared)$', '^' + chr(92) + str(capture) + '$')]
            self.assertEqual(self.run_map(groups, rows, 'sharedsharedshared', {'shared'}, keys)[0], ['bm', 'col', 'tbl'])

    def test_owners_are_printed_for_each_path(self):
        out = bash(ROOT / 'scripts/areas.sh', '--owners', MAP, 'perf_star', 'perf_table', '--', 'src/agg/v6/sip.c', 'README.md', 'src/table/am.c', 'src/core/store/store.c')
        self.assertEqual(out.split(), ['perf_star', '--', '-', '--', 'perf_table', '--', '*', '--'])


if __name__ == '__main__':
    unittest.main()
