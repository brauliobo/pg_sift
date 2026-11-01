"""The pages the generators write (BENCHMARKS.md, BENCH-GAP.md, README-blocks.md, perf/, img/) are in bench/out, untracked: replace_block makes a page from nothing and no source names the old paths in docs/."""
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(HERE, '..', 'steps'))
import readme  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../steps'))
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)

# The scripts that render or check the pages of a RELEASE name docs/BENCHMARKS.md, docs/perf and docs/img on purpose: the release commit ships the scrubbed pages generated into bench/out
# (docs/BENCHMARKS.md, docs/img and the curated results). The bench generators (bench/ and every other script) must not name them: they write bench/out.
RELEASE_PAGE_SCRIPTS = ('scripts/gen-docs.py', 'scripts/check-bench-fresh.sh', 'scripts/check-readme-tables.py')
MOVED = re.compile(r'docs/(BENCHMARKS|BENCH-GAP|BASELINE|perf/|img)|\'docs\', \'(BENCHMARKS|BENCH-GAP|BASELINE|perf|img)')


class Pages(unittest.TestCase):
    def test_a_block_is_appended_to_a_page_that_does_not_exist_then_replaced_in_place(self):
        with tempfile.TemporaryDirectory() as d:
            page = os.path.join(d, 'out', 'perf', 'AGGREGATES.md')
            readme.replace_block(page, 'one', ['a'])
            readme.replace_block(page, 'two', ['b'])
            readme.replace_block(page, 'one', ['c', 'd'])
            with open(page) as f:
                text = f.read()
        self.assertEqual(re.findall(r'<!-- bench:(\w+):begin', text), ['one', 'two'])
        self.assertIn('begin -->\nc\nd\n<!-- bench:one:end -->', text)
        self.assertNotIn('\na\n', text)

    def test_no_source_writes_into_the_tracked_docs(self):
        tracked = subprocess.run(['git', '-C', ROOT, 'ls-files', 'bench', 'scripts'], capture_output=True, text=True, check=True).stdout.split()
        hits = []
        for path in tracked:
            full = os.path.join(ROOT, path)
            if path == os.path.join('bench', 'test', 'test_pages.py') or path in RELEASE_PAGE_SCRIPTS or not os.path.isfile(full) or path.startswith('bench/results/'):
                continue
            with open(full, errors='ignore') as f:
                hits += [path for line in f if MOVED.search(line)]
        self.assertEqual(sorted(set(hits)), [])


if __name__ == '__main__':
    unittest.main()
