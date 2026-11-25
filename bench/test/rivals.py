#!/usr/bin/env python3
"""bench/steps/rivals.py without a server: the rival engines of a search run are planned, saved and assembled from the cache by the key of their adapter and context.

    python3 -I bench/test/rivals.py
"""
import argparse
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'steps'))
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)
import rivals  # noqa: E402

CELLS = 'round\tload\tclass\tengine\tmetric\tvalue\tse\tn\tsecs\n'
SIFT_ROW, RIVAL_ROW = '1\tx\tterm\tsift\tp50\t1.0\t0.1\t9\t1\n', '1\tx\tterm\tgin\tp50\t8.0\t0.2\t9\t1\n'


def args(cache, dest='', context='host-a tier slim', engines='sift gin', kind='articles'):
    return argparse.Namespace(cache=cache, kind=kind, context=context, engines=engines, dest=dest)


def result_dir(root, name, cells, builds):
    d = os.path.join(root, name)
    os.makedirs(d)
    open(os.path.join(d, 'cells.tsv'), 'w').write(CELLS + cells)
    open(os.path.join(d, 'builds.tsv'), 'w').write(builds)
    return d


class RivalCache(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cache = os.path.join(self.tmp.name, 'cache')
        self.addCleanup(self.tmp.cleanup)

    def plan(self, **kw):
        import io
        import contextlib
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rivals.cmd_plan(args(self.cache, **kw))
        fresh, cached = out.getvalue().split('\n')[:2]
        return fresh.split(), cached.split()

    def saved(self):
        first = result_dir(self.tmp.name, 'first', SIFT_ROW + RIVAL_ROW, 'slim\tsift\t5\t1\n' + 'slim\tgin\t5\t2\n')
        rivals.cmd_save(args(self.cache, first))
        return first

    def test_nothing_cached_measures_everything(self):
        self.assertEqual(self.plan(), (['sift', 'gin'], []))

    def test_a_saved_rival_is_cached_and_sift_never(self):
        self.saved()
        self.assertEqual(self.plan(), (['sift'], ['gin']))

    def test_another_context_does_not_use_the_entry(self):
        self.saved()
        self.assertEqual(self.plan(context='host-b tier slim'), (['sift', 'gin'], []))

    def test_only_cached_rivals_are_all_measured(self):
        self.saved()
        self.assertEqual(self.plan(engines='gin'), (['gin'], []))

    def test_assemble_restores_the_rows_once(self):
        self.saved()
        second = result_dir(self.tmp.name, 'second', SIFT_ROW, 'slim\tsift\t5\t1\n')
        for _ in range(2):
            rivals.cmd_assemble(args(self.cache, second, engines='gin'))
        self.assertEqual(open(os.path.join(second, 'cells.tsv')).read(), CELLS + SIFT_ROW + RIVAL_ROW)
        self.assertEqual(open(os.path.join(second, 'builds.tsv')).read(), 'slim\tsift\t5\t1\nslim\tgin\t5\t2\n')

    def test_places_keys_the_engine_by_its_own_columns(self):
        d = os.path.join(self.tmp.name, 'p')
        os.makedirs(d)
        open(os.path.join(d, 'cells.tsv'), 'w').write('round\tload\tworkload\tclass\tengine\tp50\n1\tx\ttop\tp\tgin\t3\n')
        open(os.path.join(d, 'builds.tsv'), 'w').write('gin\t1\t2\n')
        rivals.cmd_save(args(self.cache, d, engines='gin', kind='places'))
        self.assertEqual(self.plan(engines='gin sift', kind='places'), (['sift'], ['gin']))


if __name__ == '__main__':
    unittest.main()
