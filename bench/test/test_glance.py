"""bench/steps/glance.py: the README benchmark section from a fake results directory."""
import os
import re
import sys
import tempfile
import unittest
import xml.dom.minidom

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'steps'))
import glance  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../steps'))
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)

TABLE = [('page', 'p_big', 'native', 'no'), ('page', 'p_small', 'btree', 'no'), ('count', 'c1', 'native', 'yes')]


def write(path, rows, results):
    with open(path, 'w') as f:
        f.write('# orders: %d\n# postgresql: 18.6\n# pg_sift: 3.8\nversion\tcase\tarm\tmetric\tvalue\tmin\tmax\tn\tload\tnote\n' % rows)
        for case, arm, metric, value in results:
            f.write('3.8\t%s\t%s\t%s\t%s\t\t\t3\t\t\n' % (case, arm, metric, value))


class Glance(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(__import__('shutil').rmtree, self.tmp)
        for n, scale in ((100000, 1), (300000, 3), (1000000, 10)):
            write(os.path.join(self.tmp, 'cases-%d.tsv' % n), n, [
                ('p_big', 'native', 'ms', 50 * scale), ('p_big', 'sift', 'ms', 5 * scale),
                ('p_small', 'btree', 'ms', 0.1 * scale), ('p_small', 'sift', 'ms', 0.3 * scale),                    # the b-tree is ahead by 0.2 ms
                ('c1', 'native', 'ms', 20 * scale), ('c1', 'sift', 'ms', 2 * scale), ('c1', 'pg_duckdb', 'ms', 4 * scale)])

    def test_the_representative_is_the_largest_absolute_gap_either_way(self):
        data, _ = glance.load(self.tmp)
        self.assertEqual(glance.representatives(data, TABLE), {'page': ('p_big', 'native'), 'count': ('c1', 'native')})
        data['p_small'][1000000]['btree']['ms'] = 900   # the gap |900 - 3| is larger than p_big's, whichever side is ahead
        self.assertEqual(glance.representatives(data, TABLE)['page'], ('p_small', 'btree'))

    def test_the_page_has_absolute_figures_the_factor_and_the_slower_rows(self):
        data, versions = glance.load(self.tmp)
        reps = glance.representatives(data, TABLE)
        text = glance.page(data, versions, TABLE, reps, {'c1': 9.0})
        self.assertIn('| page | [`p_big`](#p-big) | 1,000,000 | PostgreSQL plan | 500 → 50 | 10× |', text)
        self.assertIn('| `p_small` | page | 1,000,000 | b-tree | 1 → 3 | **0.33× (slower)** |', text)   # in the limitations, though it is not the representative
        self.assertIn('| count | `c1` | 1,000,000 | 200 | 20 | 40 | 9 |', text)                                # PostgreSQL, pg_sift, pg_duckdb, DuckDB
        self.assertIn('PostgreSQL 18.6; the same table', text)

    def test_the_charts_are_self_contained_svg_with_a_title_and_no_date(self):
        data, versions = glance.load(self.tmp)
        reps = glance.representatives(data, TABLE)
        for svg in (glance.bars([('page', 500, 50), ('count', 200, 20)], 'Time', 'Bars.', 'milliseconds', 'baseline'), glance.slope(reps, data, None)):
            doc = xml.dom.minidom.parseString(svg)
            self.assertTrue(doc.getElementsByTagName('title')[0].firstChild.data)
            self.assertTrue(doc.getElementsByTagName('desc')[0].firstChild.data)
            self.assertIn('prefers-color-scheme:dark', svg)
            self.assertNotRegex(svg, r'20\d\d-\d\d-\d\d')
            self.assertNotIn('href=', svg)


if __name__ == '__main__':
    unittest.main()
