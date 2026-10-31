"""The coverage check of bench/steps/cases/coverage.py: every claim of a release (a CHANGELOG line under Unreleased, a landed commit that gives a measured speed) has a case with rows of
the current version, rows of its peers, and no `n/a: not measured` is left in the pages. Each test builds a small git repository: no server, no measurement."""
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
from cases import coverage, registry  # noqa: E402

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
        self.write('pg_sift.control', "default_version = '3.8'\n")
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

    def rows(self, cid, arms, version='3.8', file='cases', value='4.0'):
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

    def test_a_waiver_with_a_reason_covers_it_and_an_unused_waiver_is_an_error(self):
        self.t.land(SPEED)
        sha = self.t.git('rev-parse', '--short', 'HEAD').strip()
        self.t.append('scripts/bench-cases.allow', 'commit %s\tmeasured by the plan gate, not a benchmark\n' % sha)
        self.assertEqual(self.messages(), [])
        self.t.append('scripts/bench-cases.allow', 'commit 0000000\tno such commit\n')
        self.assertIn('matches no claim', ' '.join(self.messages()))

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


if __name__ == '__main__':
    unittest.main()
