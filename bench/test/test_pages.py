"""The generators write the tracked pages and results only under docs/ (docs/results, docs/charts, docs/BENCHMARKS.md, README.md): no source names the retired bench/results or bench/out/img paths."""
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(HERE, '..', 'steps'))
import resultfile  # noqa: E402
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)

RETIRED = re.compile(r'bench/results|bench/out/img|bench/out/BENCHMARKS|bench/out/README-blocks|\'bench\', \'results\'')


class Pages(unittest.TestCase):
    def test_a_block_is_appended_to_a_page_that_does_not_exist_then_replaced_in_place(self):
        with tempfile.TemporaryDirectory() as d:
            page = os.path.join(d, 'out', 'perf', 'AGGREGATES.md')
            resultfile.replace_block(page, 'one', ['a'])
            resultfile.replace_block(page, 'two', ['b'])
            resultfile.replace_block(page, 'one', ['c', 'd'])
            with open(page) as f:
                text = f.read()
        self.assertEqual(re.findall(r'<!-- bench:(\w+):begin', text), ['one', 'two'])
        self.assertIn('begin -->\nc\nd\n<!-- bench:one:end -->', text)
        self.assertNotIn('\na\n', text)

    def test_no_source_names_the_retired_result_and_chart_paths(self):
        tracked = subprocess.run(['git', '-C', ROOT, 'ls-files', 'bench', 'scripts'], capture_output=True, text=True, check=True).stdout.split()
        hits = []
        for path in tracked:
            full = os.path.join(ROOT, path)
            if path == 'bench/test/test_pages.py' or not os.path.isfile(full):
                continue
            with open(full, errors='ignore') as f:
                hits += [path for line in f if RETIRED.search(line)]
        self.assertEqual(sorted(set(hits)), [])


if __name__ == '__main__':
    unittest.main()
