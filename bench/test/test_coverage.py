"""The coverage check of bench/steps/cases/coverage.py: every claim of a release (a CHANGELOG line under Unreleased, a landed commit that gives a measured speed) has a case with rows of
the current version, rows of its peers, and no `n/a: not measured` is left in the pages. Each test builds a small git repository: no server, no measurement."""
import os
import re
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
from cases import coverage, registry  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../steps'))
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)

HEAD = ('id', 'changelog', 'dataset', 'index', 'plan', 'baselines', 'variants', 'metric', 'block', 'headline', 'peers')
RESULT_HEAD = 'version\tcase\tarm\tmetric\tvalue\tmin\tmax\tn\tload\tnote\n'
CLAIM_LINE = '- Changed: pg_sift: a top-N over a text key reads a tenth of the blocks, 30 ms against 4 ms.'
SPEED = 'agg: topn: a text key reads a tenth of the blocks, 30 ms against 4 ms\n\nthe order array answers the page.'


class Tree:
    """a repository with a tag, a registry, results and pages; `land` adds a commit"""

    def __init__(self, tmp):
        self.root = tmp
        self.git('init', '-q', '-b', 'main')
        self.git('config', 'user.email', 't@example.org')
        self.git('config', 'user.name', 't')
        self.write('pg_sift.control', "default_version = '4.0'\n")
        self.write('CHANGELOG.md', '# Changelog\n\n## Unreleased\n\n## 3.7.0\n')
        self.write('bench/cases.tsv', '# registry\n')
        self.write('bench/results/cases.tsv', '# results\n' + RESULT_HEAD)
        self.write('bench/out/README-blocks.md', '# readme\n<!-- bench:faster33:begin -->\nx\n<!-- bench:faster33:end -->\n')
        self.write('scripts/bench-cases.allow', '# waivers\n')
        self.commit('release: 3.7.0')
        self.git('tag', 'v3.7.0')

    def git(self, *a):
        return subprocess.run(['git', '-C', self.root, *a], check=True, capture_output=True, text=True).stdout

    def write(self, path, text):
        p = os.path.join(self.root, path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'w').write(text)

    def append(self, path, text):
        open(os.path.join(self.root, path), 'a').write(text)

    def commit(self, message):
        self.git('add', '-A')
        self.git('commit', '-q', '-m', message)

    def land(self, message, path='src/core/host.h'):
        self.append(path, '/* %s */\n' % message.split('\n')[0]) if os.path.exists(os.path.join(self.root, path)) else self.write(path, '/* x */\n')
        self.commit(message)

    def case(self, cid, phrase, peers='-', block='faster33'):
        self.append('bench/cases.tsv', '\t'.join([cid, phrase, 'orders', 'default', 'Top-N', 'native', '-', 'ms', block, 'no', peers]) + '\n')
        self.write('bench/cases/%s.sql' % cid, '-- ' + cid + '\n--@ pg\nSELECT 1;\n')

    def rows(self, cid, arms, version='4.0', file='cases', value='4.0'):
        for arm in arms:
            self.append('bench/results/%s.tsv' % file, '\t'.join([version, cid, arm, 'ms', value, '', '', '3', '', '']) + '\n')

    def check(self, **kw):
        registry._REGISTRY.clear()   # the registry is read once per root
        return coverage.check(self.root, **kw)


class Fixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.t = Tree(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def messages(self, **kw):
        return [m for m, _ in self.t.check(**kw)[0]]


class KnownGaps(Fixture):
    def flag(self):
        self.t.case('topn_x', '-')
        self.t.write('bench/cases/topn_x.sql', '-- topn_x\n-- gap: sift lab 0013 (filtered ordered page)\n--@ pg\nSELECT 1;\n')

    def test_a_flagged_case_is_counted_in_the_notes_and_fails_only_the_release_gate(self):
        self.flag()
        errors, notes = self.t.check()
        self.assertEqual(errors, [])
        self.assertTrue(any('topn_x' in n and 'known gap on the sift lane' in n and 'lab 0013' in n for n in notes))
        errors, _ = self.t.check(strict=True)
        self.assertTrue(any('topn_x' in m and 'known gap' in m for m, _ in errors))

    def test_a_case_without_the_flag_adds_none(self):
        self.t.case('topn_y', '-')
        self.assertFalse(any('known gap' in n for n in self.t.check()[1]))


class Landed(Fixture):
    def test_a_landed_optimization_without_a_case_or_a_waiver_is_listed_with_its_commit(self):
        self.t.land(SPEED)
        found = self.messages()
        self.assertEqual(len(found), 1)
        self.assertIn('a text key reads a tenth of the blocks', found[0])
        self.assertIn(self.t.git('rev-parse', '--short', 'HEAD').strip(), found[0])

    def test_a_commit_that_does_not_give_a_speed_or_does_not_touch_the_code_is_not_a_claim(self):
        self.t.land('agg: topn: rename a helper')
        self.t.land('docs: topn: 30 ms against 4 ms in the text', path='docs/USAGE.md')
        self.assertEqual(self.messages(), [])

    def test_a_range_of_releases_is_not_a_ratio(self):
        self.t.write('CHANGELOG.md', '# Changelog\n\n## Unreleased\n\n- Fixed: pg_sift: wrong rows from 3.4.0 to 3.7.1: a join took a parameter as 0.\n\n## 3.7.0\n')
        self.assertEqual(self.messages(), [])
        self.t.land('agg: topn: wrong rows from 3.4.0 to 3.7.1 are fixed')
        self.assertEqual(self.messages(), [])

    def test_a_waiver_with_a_reason_covers_it_and_an_unused_waiver_is_an_error(self):
        self.t.land(SPEED)
        sha = self.t.git('rev-parse', '--short', 'HEAD').strip()
        self.t.append('scripts/bench-cases.allow', 'commit %s\tmeasured by the plan gate, not a benchmark\n' % sha)
        self.assertEqual(self.messages(), [])
        self.t.append('scripts/bench-cases.allow', 'commit 0000000\tno such commit\n')
        self.assertIn('matches no claim', ' '.join(self.messages()))

    def test_a_waiver_is_the_start_of_the_hash_whatever_its_length(self):
        self.t.land(SPEED)
        full = self.t.git('rev-parse', 'HEAD').strip()
        for n in (7, 8, 9, 10, 40):
            with self.subTest(length=n):
                self.t.write('scripts/bench-cases.allow', 'commit %s\tmeasured by the plan gate\n' % full[:n])
                self.assertEqual(self.messages(), [])
        self.t.write('scripts/bench-cases.allow', 'commit %s\tmeasured by the plan gate\n' % full[:6])
        self.assertIn('matches no claim', ' '.join(self.messages()))

    def test_a_waiver_that_starts_two_hashes_is_ambiguous(self):
        commits = [('abcdef0123' + '0' * 30, 'abcdef0', 'one', False), ('abcdef0999' + '0' * 30, 'abcdef0', 'two', False), ('1234567890' + '0' * 30, '1234567', 'three', False)]
        self.assertEqual([c[2] for c in coverage.waived_commits('commit abcdef0', commits)], ['one', 'two'])
        self.assertEqual([c[2] for c in coverage.waived_commits('commit abcdef01', commits)], ['one'])
        self.assertEqual(coverage.waived_commits('commit abcdef', commits), [])

    def test_a_commit_that_adds_its_changelog_line_is_judged_by_that_line(self):
        self.t.write('CHANGELOG.md', '# Changelog\n\n## Unreleased\n\n%s\n\n## 3.7.0\n' % CLAIM_LINE)
        self.t.land(SPEED)
        found = self.messages()
        self.assertEqual(len(found), 1)
        self.assertIn('CHANGELOG', found[0])
        self.t.case('topn_text_key', 'a top-N over a text key reads a tenth of the blocks')
        self.t.commit('bench: case')
        self.t.rows('topn_text_key', ['sift', 'native'])
        self.t.commit('bench: rows')
        self.assertEqual(self.messages(), [])

    def test_a_case_without_a_changelog_phrase_claims_no_line(self):
        self.t.write('CHANGELOG.md', '# Changelog\n\n## Unreleased\n\n%s\n\n## 3.7.0\n' % CLAIM_LINE)
        self.t.case('search_and_count', '-')
        self.t.commit('bench: case')
        found = self.messages()
        self.assertEqual(len(found), 1)
        self.assertIn('no case measures', found[0])

    def test_since_is_the_last_tag_that_is_behind_the_head_unless_given(self):
        self.t.land(SPEED)
        self.t.git('tag', 'v3.7.1')
        self.t.land('agg: topn: ordinary')
        self.assertEqual(self.messages(), [])
        self.assertEqual(len(self.messages(since='v3.7.0')), 1)


class Rows(Fixture):
    def claim(self, peers='-'):
        self.t.write('CHANGELOG.md', '# Changelog\n\n## Unreleased\n\n%s\n\n## 3.7.0\n' % CLAIM_LINE)
        self.t.case('topn_text_key', 'a top-N over a text key reads a tenth of the blocks', peers)

    def test_rows_of_an_older_version_are_stale(self):
        self.claim()
        self.t.rows('topn_text_key', ['sift', 'native'], version='3.7.0')
        self.assertIn('stale', ' '.join(self.messages()))

    def test_a_missing_lane_or_an_NA_value_is_a_gap(self):
        self.claim()
        self.t.rows('topn_text_key', ['sift'])
        self.t.rows('topn_text_key', ['native'], value='NA')
        self.assertIn('native', ' '.join(self.messages()))

    def test_a_peer_needs_a_row_of_its_own_in_its_file(self):
        self.claim('generic:clickhouse')
        self.t.rows('topn_text_key', ['sift', 'native'])
        self.assertIn('generic:clickhouse', ' '.join(self.messages()))
        self.t.write('bench/results/generic.tsv', '# generic\n' + RESULT_HEAD)
        self.t.rows('topn_text_key', ['clickhouse'], file='generic')
        self.assertEqual(self.messages(), [])

    def test_a_case_whose_block_is_not_in_a_page_is_a_gap(self):
        self.claim()
        self.t.rows('topn_text_key', ['sift', 'native'])
        self.assertEqual(self.messages(), [])
        self.t.write('bench/out/README-blocks.md', '# readme\n<!-- bench:other:begin -->\nx\n<!-- bench:other:end -->\n')
        self.assertIn('faster33', ' '.join(self.messages()))

    def test_pages_without_any_generated_block_are_one_note_in_warn_mode_and_one_error_when_strict(self):
        self.claim()
        self.t.rows('topn_text_key', ['sift', 'native'])
        self.t.write('bench/out/README-blocks.md', '# readme\n')
        errors, notes = self.t.check()
        self.assertEqual(errors, [])
        self.assertTrue(any('no generated block in bench/out/README-blocks.md' in n for n in notes))
        self.assertEqual(len([m for m, _ in self.t.check(strict=True)[0] if 'no generated block' in m]), 1)

    def test_without_results_the_row_checks_are_one_note_in_warn_mode_and_an_error_when_strict(self):
        self.claim()
        os.remove(os.path.join(self.t.root, 'bench/results/cases.tsv'))
        errors, notes = self.t.check()
        self.assertEqual(errors, [])
        self.assertTrue(any('no bench/results/cases.tsv' in n for n in notes))
        self.assertTrue(any('no bench/results/cases.tsv' in m for m, _ in self.t.check(strict=True)[0]))

    def test_strict_wants_rows_measured_after_the_last_change_of_the_code(self):
        self.claim()
        build = self.t.git('rev-parse', 'HEAD').strip()
        self.t.write('bench/results/cases.tsv', '# results\n# build: %s\n%s' % (build, RESULT_HEAD))
        self.t.rows('topn_text_key', ['sift', 'native'])
        self.t.commit('bench: rows')
        self.assertEqual(self.messages(strict=True), [])
        self.t.land('agg: topn: ordinary')
        self.assertEqual(self.messages(), [])
        self.assertIn('before the last change of src/ or sql/', ' '.join(self.messages(strict=True)))


class Figures(Fixture):
    """the figures of the CHANGELOG against bench/claims.tsv: each line that states one has a row or a reasoned waiver; with records the rows of their size are checked"""
    ROW = 'a top-N over a text key\tcase_a\t300000\tsift\tdefault\texec_ms\t1\t\n'

    def figures(self, **kw):
        registry._REGISTRY.clear()
        return coverage.claim_figures(self.t.root, **kw)

    def line(self):
        self.t.append('CHANGELOG.md', '')
        self.t.write('CHANGELOG.md', '# Changelog\n\n## Unreleased\n\n' + CLAIM_LINE + '\n\n## 3.7.0\n')

    def test_a_line_that_states_a_figure_needs_a_row_or_a_waiver(self):
        self.line()
        self.assertEqual(len(self.figures()), 1)
        self.t.write('bench/claims.tsv', 'phrase\tstatement\trows\tarm\toption\tmetric\tvalue\n' + self.ROW)
        self.assertEqual(self.figures(), [])

    def test_a_waiver_covers_a_line_and_is_refused_once_the_line_has_a_row_or_matches_no_line(self):
        self.line()
        self.t.write('scripts/bench-cases.allow', 'figure a top-N over a text key\tthe row waits for a run\n')
        self.assertEqual(self.figures(), [])
        self.t.write('bench/claims.tsv', 'phrase\tstatement\trows\tarm\toption\tmetric\tvalue\n' + self.ROW)
        self.assertIn('remove the waiver', self.figures()[0])
        self.t.write('scripts/bench-cases.allow', 'figure a sentence nobody wrote\treason\n')
        self.assertIn('matches no line', self.figures()[0])

    def test_a_row_is_checked_when_the_records_have_its_statement_and_size_and_waits_otherwise(self):
        self.line()
        self.t.write('bench/claims.tsv', 'phrase\tstatement\trows\tarm\toption\tmetric\tvalue\n' + self.ROW)
        recs = os.path.join(self.t.root, 'recs.tsv')
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps', 'records'))
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import schema
        import test_records
        rs = [{k: v for k, v in r.items() if not k.startswith('_')} | dict(statement='case_a', rows=300000) for r in test_records.clean()]
        schema.write(recs, rs)
        self.assertEqual(self.figures(records=[recs]), [])
        self.t.write('bench/claims.tsv', 'phrase\tstatement\trows\tarm\toption\tmetric\tvalue\n' + self.ROW.replace('\t1\t', '\t9\t'))
        self.assertIn('CLAIM NOT MET', self.figures(records=[recs])[0])
        schema.write(recs, [dict(r, rows=1000) for r in rs])
        self.assertEqual(self.figures(records=[recs]), [])   # the run was at another size: the row waits
        self.assertIn('NO RECORD', self.figures(records=[recs], strict=True)[0])


class Pages(Fixture):
    def test_strict_fails_on_every_not_measured_footer_and_warn_mode_only_counts_them(self):
        self.t.write('bench/out/BENCHMARKS.md', 'a\n`n/a`: not measured under 3.7 yet.\nb\n`n/a`: not measured under 3.7 yet.\n')
        errors, notes = self.t.check()
        self.assertEqual(errors, [])
        self.assertTrue(any('2 table footers say "not measured"' in n for n in notes))
        self.assertEqual(len([m for m, _ in self.t.check(strict=True)[0] if 'bench/out/BENCHMARKS.md' in m]), 2)


class HostPlan(Fixture):
    def test_a_case_without_current_rows_becomes_the_command_that_measures_it(self):
        self.t.write('CHANGELOG.md', '# Changelog\n\n## Unreleased\n\n%s\n\n## 3.7.0\n' % CLAIM_LINE)
        self.t.case('topn_text_key', 'a top-N over a text key reads a tenth of the blocks')
        self.t.case('other_case', 'another phrase')
        self.t.rows('topn_text_key', ['sift', 'native'], version='3.7.0')
        self.t.rows('other_case', ['sift', 'native'])
        plan = coverage.host_plan(self.t.root)
        self.assertIn('bench/bench cases --cases topn_text_key --rows 1000000 --docs', plan)
        self.assertNotIn('other_case', ' '.join(plan))

    def test_without_results_every_case_of_a_claim_is_to_be_measured(self):
        self.t.write('CHANGELOG.md', '# Changelog\n\n## Unreleased\n\n%s\n\n## 3.7.0\n' % CLAIM_LINE)
        self.t.case('topn_text_key', 'a top-N over a text key reads a tenth of the blocks')
        os.remove(os.path.join(self.t.root, 'bench/results/cases.tsv'))
        self.assertEqual(coverage.host_plan(self.t.root), ['bench/bench cases --cases topn_text_key --rows 1000000 --docs'])

    def test_the_footers_become_the_release_command_with_the_cells_of_the_competitors_measured_again(self):
        self.t.write('bench/out/README-blocks.md', '`n/a`: not measured under 3.7 yet.\n')
        self.assertIn('bench/bench release --remeasure-all --budget 160', coverage.host_plan(self.t.root))


class ScriptsCallRealSubcommands(unittest.TestCase):
    """a script or lint entry that calls cases.py with a subcommand that does not exist rots unseen: lint runs no such call"""

    def test_every_call_of_cases_py_names_a_subcommand_of_main(self):
        repo = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
        with open(os.path.join(repo, 'bench', 'steps', 'cases', 'main.py')) as f:
            known = set(re.findall(r"'(\w+)'", re.search(r"add_argument\('cmd', choices=\[([^\]]*)\]", f.read()).group(1)))
        calls = []
        for folder, _, names in os.walk(repo):
            if '.git' in folder.split(os.sep) or os.sep + 'out' in folder:
                continue
            for name in names:
                if name.endswith('.sh') or name == 'bench':
                    path = os.path.join(folder, name)
                    with open(path, errors='replace') as f:
                        calls += [(os.path.relpath(path, repo), sub) for sub in re.findall(r'python3 (?:-\w+ )*(?:\S*cases\.py|"\$CASES")"? (\w+)', f.read())]
        self.assertTrue(calls)
        self.assertEqual([c for c in calls if c[1] not in known], [])


if __name__ == '__main__':
    unittest.main()
