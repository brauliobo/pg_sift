#!/usr/bin/env python3
"""Tests of the pure parts of the PR gate (scripts/perf/gate.py): the plan verifier, the perf parser, the verdicts, the catalog. No PostgreSQL, under a second.

  python3 -I scripts/perf/selftest.py
"""
import contextlib
import io
import os
import re
import subprocess
import sys
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRATCH = '/dev/shm' if os.path.isdir('/dev/shm') else tempfile.gettempdir()
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))
import canaries  # noqa: E402
import gate  # noqa: E402


def node(kind='Seq Scan', plans=(), **kw):
    return {'Node Type': kind, 'Shared Hit Blocks': 0, 'Shared Read Blocks': 0, 'Plans': list(plans), **kw}


def scan(provider='SiftAggV6', **kw):
    return node('Custom Scan', **{'Custom Plan Provider': provider, **kw})


def gather(planned, launched, loops, avg_rows, worker_rows, **kw):
    workers = [{'Worker Number': i, 'Actual Rows': r, 'Actual Loops': 1} for i, r in enumerate(worker_rows)]
    leaf = node('Parallel Seq Scan', **{'Parallel Aware': True, 'Actual Rows': avg_rows, 'Actual Loops': loops, 'Workers': workers})
    return node('Gather', [leaf], **{'Workers Planned': planned, 'Workers Launched': launched, **kw})


def doc(root):
    return [{'Plan': root}]


AGG = canaries.Canary('t', 'star', '1T', 'select 1')
PARALLEL = replace(AGG, mode='4T', workers=2, balance=True, plan=('none',))


class Plan(unittest.TestCase):
    def test_verdicts(self):
        cases = [
            ('a sift aggregate passes', AGG, scan(**{'Shared Hit Blocks': 10}), []),
            ('a family not in the plan', AGG, node('Seq Scan'), ['no agg']),
            ('blocks add hit and read', replace(AGG, blocks=100), scan(**{'Shared Hit Blocks': 60, 'Shared Read Blocks': 41}), ['101 shared blocks']),
            ('temp blocks of any node', AGG, scan(plans=[node(**{'Temp Written Blocks': 3})]), ['3 temp blocks']),
            ('a seq scan where the index belongs', replace(AGG, no_seq=True), scan(plans=[node()]), ['Seq Scan']),
            ('heap vets on a frozen table', replace(AGG, heap_zero=True), scan(**{'Heap Vet Rows': 4}), ['Heap Vet Rows = 4']),
            ('a heap fetch ceiling', replace(AGG, extra=(('Heap Fetches', 5),)), scan(**{'Heap Fetches': 9}), ['Heap Fetches = 9']),
            ('a lookup taken by sift instead of the B-tree', replace(AGG, plan=('idx:t_pkey',)), scan(), ['no idx:t_pkey', 'took a lookup']),
            ('the B-tree lookup', replace(AGG, plan=('idx:t_pkey',)), node('Index Scan', **{'Index Name': 't_pkey'}), []),
        ]
        for name, cn, root, want in cases:
            with self.subTest(name):
                got = gate.plan_failures(cn, doc(root))
                self.assertEqual(len(got), len(want), got)
                for text, g in zip(want, got):
                    self.assertIn(text, g)

    def test_workers(self):
        cases = [
            ('balanced', gather(3, 3, 4, 250, [250, 250, 250]), []),
            ('fewer launched than planned', gather(4, 2, 3, 333, [450, 450]), ['launched 2 of 4']),
            ('planned below the minimum', gather(1, 1, 2, 500, [500]), ['plans 1 workers']),
            ('the leader scans the table', gather(2, 2, 3, 333, [50, 50]), ['at least 50%']),
            ('one worker does all', gather(2, 2, 3, 333, [800, 100]), ['at most 60%']),
            ('no per-worker rows without VERBOSE', gather(2, 2, 3, 333, []), []),
        ]
        for name, root, want in cases:
            with self.subTest(name):
                got = gate.plan_failures(PARALLEL, doc(node('Sort', [root])))
                self.assertEqual(len(got), len(want), got)
                for text, g in zip(want, got):
                    self.assertIn(text, g)

    def test_summary_of_the_counters_read(self):
        root = gather(3, 3, 4, 250, [250, 250, 250], **{'Shared Hit Blocks': 9})
        self.assertEqual(gate.plan_summary(replace(PARALLEL, blocks=20), doc(root)), 'plan=Gather,Parallel Seq Scan blocks=9/20 temp=0/0 heap=0 workers=3/3')

    def test_serial_plan_under_4t(self):
        self.assertEqual(gate.plan_failures(replace(PARALLEL, workers=0), doc(scan())), [])
        self.assertIn('no parallel node', gate.plan_failures(PARALLEL, doc(scan()))[0])

    def test_custom_scan_workers_without_gather(self):
        self.assertEqual(gate.plan_failures(replace(PARALLEL, balance=False), doc(scan(**{'Workers Planned': 2, 'Workers Launched': 2}))), [])


class Numbers(unittest.TestCase):
    def test_perf_output(self):
        cases = [('151041,,instructions:u,1156181,100.00,,\n', 151041), ('<not counted>,,instructions:u,0,0.00,,\n', None), ('<not supported>,,instructions:u,0,0,,\n', None),
                 ('', None), ('12,,cycles:u,1,1,,\n77,,instructions:u,1,1,,\n', 77)]
        for text, want in cases:
            with self.subTest(text):
                self.assertEqual(gate.parse_perf(text), want)

    def test_dual_floor(self):
        cases = [(1_200_000, 500_000, 1.5, True), (400_000, 200_000, 1.5, False), (1_000_000, 900_000, 1.5, False), (2_000_000, 1_000_000, 2.0, False),
                 (900_000, 1_000_000, 0.4, True), (500_000, 1_000_000, 0.4, False), (300_000, 500_000, 0.45, False)]
        for sift, base, ceiling, want in cases:
            with self.subTest((sift, base, ceiling)):
                self.assertEqual(gate.ratio_failure(sift, base, ceiling), want)

    def test_rows(self):
        self.assertTrue(gate.rows_equal([['a', '1.0000000001']], [['a', '1.0']]))
        self.assertFalse(gate.rows_equal([['a', '1.001']], [['a', '1.0']]))
        self.assertFalse(gate.rows_equal([['a']], [['b']]))
        self.assertFalse(gate.rows_equal([['a']], [['a'], ['a']]))

    def test_limits(self):
        cn = replace(AGG, ratio=0.5, per_row=100, total=1_000_000)
        side = lambda instr: {'instr': instr, 'rows': [['1']]}
        self.assertEqual(gate.number_failures(cn, side(300_000), side(900_000), 64000), [])
        self.assertEqual(len(gate.number_failures(cn, side(3_000_000), side(4_000_000), 64000)), 2)
        self.assertEqual(len(gate.number_failures(cn, side(7_000_000), side(30_000_000), 64000)), 2)
        self.assertEqual(len(gate.number_failures(cn, side(450_000), side(1_000_000), 64000)), 0)

    def test_no_pmu_keeps_the_rows(self):
        cn = replace(AGG, ratio=0.1, per_row=1, total=1)
        none = {'instr': None, 'rows': [['1']]}
        self.assertEqual(gate.number_failures(cn, none, {'instr': None, 'rows': [['1']]}, 64000), [])
        self.assertEqual(gate.number_failures(cn, none, {'instr': None, 'rows': [['2']]}, 64000), ['the rows differ from the native plan'])

    def test_write_limits(self):
        cn = replace(AGG, extra=(('bytes_per_row', 120), ('wal_per_row', 180), ('wal_ratio', 1.45)))
        self.assertEqual(gate.write_failures(cn, {'rel': 64000 * 100, 'wal': 64000 * 100}, {'wal': 64000 * 100}, 64000), [])
        self.assertEqual(len(gate.write_failures(cn, {'rel': 64000 * 130, 'wal': 64000 * 200}, {'wal': 64000 * 100}, 64000)), 3)

    def test_gpu_arbitration(self):
        cases = [('not used (build)', False), ('shared worker', True), ("backend's own device", True)]
        for note, want in cases:
            with self.subTest(note):
                self.assertEqual(gate.gpu_device_used(doc(scan(GPU=note)))[0], want)
        self.assertEqual(gate.gpu_device_used(doc(scan()))[0], False)


class Settings(unittest.TestCase):
    def test_stock_planner(self):
        forced = re.compile(r'enable_\w+ = off|cpu_tuple_cost = 0\.[2-9]|parallel_setup_cost|parallel_tuple_cost|min_parallel_\w+|seq_page_cost = 100')
        for mode, workers in (('1T', 0), ('4T', 4)):
            s = gate.settings(mode)
            self.assertIn(f'max_parallel_workers_per_gather = {workers}', s)
            self.assertIsNone(forced.search(s.replace('SET sift.enable_agg = on', '')), s)

    def test_session_window(self):
        with_pmu, without = (gate.session_script(Path('/w'), 'SET x = 1', '', 'SELECT 1', gate.EXPLAIN, window) for window in (True, False))
        self.assertEqual(with_pmu.count('GATE_CTL'), 2)
        self.assertEqual(without.count('GATE_CTL'), 0)
        self.assertLess(with_pmu.index('enable'), with_pmu.index('SELECT 1;\n\\! echo disable'))
        self.assertIn('EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, FORMAT JSON) SELECT 1;', with_pmu)
        self.assertIn('FORMAT JSON, VERBOSE) SELECT 1;', gate.session_script(Path('/w'), 'SET x = 1', '', 'SELECT 1', gate.EXPLAIN_VERBOSE, True))


class Catalog(unittest.TestCase):
    def test_shape(self):
        by_area = {a: [c.id for c in canaries.CANARIES if c.area == a] for a in canaries.AREAS}
        self.assertEqual({a: len(v) for a, v in by_area.items()}, {'star': 3, 'clickbench': 5, 'coexist': 3, 'jsonb': 4, 'table': 4, 'bm25': 2, 'gpu': 1})
        self.assertEqual(len(canaries.CANARIES), 22)
        self.assertEqual(len({c.id for c in canaries.CANARIES}), 22)
        for a in canaries.AREAS:
            self.assertEqual(len([c for c in canaries.CANARIES if c.area == a and c.smoke]), 1, a)

    def test_every_canary_asserts_something(self):
        for c in canaries.CANARIES:
            with self.subTest(c.id):
                self.assertIn(c.mode, gate.WORKERS)
                self.assertTrue(c.blocks is not None or c.ratio or c.total or c.extra, 'no limit')

    def test_selection(self):
        self.assertEqual(len(canaries.select()), 22)
        self.assertEqual([c.id for c in canaries.select(['bm25'])], ['bm25_topk_common_1t', 'bm25_hybrid_facet_4t'])
        self.assertEqual(len(canaries.select(smoke=True)), 7)
        self.assertEqual([c.id for c in canaries.select(['jsonb'], smoke=True)], ['jsonb_shred_agg_1t'])
        with self.assertRaises(ValueError):
            canaries.select(['nope'])

    def test_black_box(self):
        forbidden = re.compile(r'sift\._test_\w+\(|sift\.test_\w+ *=|#include|callgrind|SiftParPool|SiftMorselDriver|SiftCompact|Format [12]|enable_seqscan = off|cpu_tuple_cost = 0\.[2-9]|random\(\)')
        for f in ('gate.py', 'canaries.py', 'fixture.sql'):
            for n, line in enumerate((HERE / f).read_text().splitlines(), 1):
                self.assertIsNone(forbidden.search(line), f'{f}:{n}: {line}')

    def test_cli_arguments(self):
        a = gate.parse_args(['--areas', 'star,smoke', '--area', 'jsonb', '--no-perf', '--port', '56274'])
        self.assertEqual((a.names, a.smoke, a.no_perf, a.port), (['jsonb', 'star'], True, True, 56274))
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            gate.parse_args([])
        self.assertEqual(gate.parse_args(['--all', '--inject', 'SET sift.enable_agg = off']).inject, 'SET sift.enable_agg = off')


class Lock(unittest.TestCase):
    def test_dead_holder_is_taken_over(self):
        dead = subprocess.Popen(['true'])
        dead.wait()
        with tempfile.TemporaryDirectory(dir=SCRATCH) as d:
            lock = Path(d) / 'tpl.lock'
            lock.mkdir()
            (lock / 'pid').write_text(str(dead.pid))
            self.assertTrue(gate.acquire(lock, lambda: False))
            self.assertEqual((lock / 'pid').read_text(), str(os.getpid()))

    def test_clean_stale(self):
        dead = subprocess.Popen(['true'])
        dead.wait()
        with tempfile.TemporaryDirectory(dir=SCRATCH) as d:
            cache = Path(d)
            for name in (f'run-{dead.pid}', f'run-{os.getpid()}', 'tpl-old', 'tpl-new', 'tpl-x.tmp'):
                (cache / name).mkdir()
            for name in ('tpl-old', 'tpl-x.tmp'):
                os.utime(cache / name, (0, 0))
            gate.clean_stale(cache, 3600)
            self.assertEqual(sorted(p.name for p in cache.iterdir()), [f'run-{os.getpid()}', 'tpl-new', 'tpl-old', 'tpl-x.tmp'])
            (cache / f'run-{os.getpid()}').rename(cache / f'run-{dead.pid}')
            (cache / 'tpl-new' / 'data').mkdir()
            (cache / 'tpl-new' / 'data' / 'postmaster.pid').write_text(f'{os.getpid()}\n')
            gate.clean_stale(cache, 3600)
            self.assertEqual(sorted(p.name for p in cache.iterdir()), ['tpl-new', 'tpl-old', 'tpl-x.tmp'])
            (cache / 'tpl-new' / 'data' / 'postmaster.pid').write_text(f'{dead.pid}\n')
            gate.clean_stale(cache, 3600)
            self.assertEqual(sorted(p.name for p in cache.iterdir()), ['tpl-new'])

    def test_template_built_by_the_holder(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as d:
            lock = Path(d) / 'tpl.lock'
            lock.mkdir()
            (lock / 'pid').write_text(str(os.getppid()))
            ready = iter([False, True])
            self.assertFalse(gate.acquire(lock, lambda: next(ready)))


if __name__ == '__main__':
    unittest.main(verbosity=1)
