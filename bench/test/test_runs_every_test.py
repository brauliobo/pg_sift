"""Every test file of the harness runs all of its tests when it is run as a script (scripts/lint.sh runs `python3 -I FILE`): the `__main__` block is the last statement, so no class after it is
skipped (20 tests of test_cases.py were not run for a while), and no class or test has the name of another (the later one replaces the earlier: SearchFig of test_triple.py was there twice)."""
import ast
import collections
import glob
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


FILES = sorted(glob.glob(os.path.join(HERE, 'test_*.py')) + glob.glob(os.path.join(HERE, '..', 'scaling', 'test_*.py')))


class EveryTestRuns(unittest.TestCase):
    def test_no_class_or_test_has_the_name_of_another(self):
        self.assertGreater(len(FILES), 10)
        for f in FILES:
            body = ast.parse(open(f).read()).body
            names = collections.Counter(n.name for n in body if isinstance(n, ast.ClassDef))
            for c in (n for n in body if isinstance(n, ast.ClassDef)):
                names.update('%s.%s' % (c.name, m.name) for m in c.body if isinstance(m, ast.FunctionDef))
            self.assertEqual([k for k, v in names.items() if v > 1], [], os.path.basename(f))

    def test_no_test_file_defines_anything_after_its_main_block(self):
        for f in FILES:
            body = ast.parse(open(f).read()).body
            main = [i for i, n in enumerate(body) if isinstance(n, ast.If) and '__main__' in ast.dump(n.test)]
            self.assertTrue(all(i == len(body) - 1 for i in main), '%s: statements follow its `if __name__ == "__main__"` block, so the tests after it never run' % os.path.basename(f))


if __name__ == '__main__':
    unittest.main()
