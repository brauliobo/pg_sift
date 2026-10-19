#!/usr/bin/env python3
"""Tests of the pure parts of steps/cell.py: the script reader and the variable expansion of the persistent driver. Run: python3 -I bench/test/test_cell.py"""
import importlib.util, os, sys, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'steps'))
spec = importlib.util.spec_from_file_location('cell', os.path.join(HERE, '..', 'steps', 'cell.py'))
cell = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cell)

SCRIPT = """\\set i random(1, 300)
SELECT quote_literal(q) AS q, quote_literal(sq) AS sq FROM (SELECT q, q || '*' AS sq FROM pq WHERE id = 5 + :i) x \\gset
SELECT id FROM places WHERE name @@ :q::tsquery
 ORDER BY id LIMIT 10;
"""


class Script(unittest.TestCase):
    def test_steps(self):
        steps = cell.parse_script(SCRIPT)
        self.assertEqual(steps[0], ('set', 'i', 1, 300))
        self.assertEqual([s[0] for s in steps], ['set', 'sql', 'sql'])
        self.assertTrue(steps[1][2] and not steps[2][2])
        self.assertFalse(steps[2][1].endswith(';'))
        self.assertIn('LIMIT 10', steps[2][1])

    def test_unterminated(self):
        with self.assertRaises(ValueError):
            cell.parse_script('SELECT 1')

    def test_expand(self):
        self.assertEqual(cell.expand("x = :q::text AND y = :missing AND z = :i", {'q': "'a'", 'i': '7'}), "x = 'a'::text AND y = :missing AND z = 7")
        self.assertEqual(cell.expand("a::int :: text", {'int': 'no'}), "a::int :: text")


class Noise(unittest.TestCase):
    def test_window(self):
        self.assertFalse(cell.noisy((0, 0, 0.0), (50_000, 0, 1.0), 10))     # 5% of one second
        self.assertTrue(cell.noisy((0, 0, 0.0), (0, 200_000, 1.0), 10))     # 20% IO stall


if __name__ == '__main__':
    unittest.main(warnings='ignore')
