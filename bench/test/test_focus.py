#!/usr/bin/env python3
"""bench/focus: the pure parts of the focused write and read benches (class of a WAL record, the cells, the expectation check, the flags of compare.py,
the phase-balanced statistics, the rotated key windows). No server: the benches themselves are run by hand or by bench/focus/run.sh.

    python3 -I bench/test/test_focus.py
"""
import io
import os
import sys
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'focus'))
import compare  # noqa: E402
import lab  # noqa: E402
import reads  # noqa: E402
import wamp  # noqa: E402


def result(idx_by_batch, n=100, size=1e6):
    return {'case': 'std', 'ck': 24, 'n': n, 'batches': [{'b': b + 1, 'idx': i, 'all': 10 * n, 'size': size, 'ops': {}} for b, i in enumerate(idx_by_batch)]}


class Classes(unittest.TestCase):
    def test_group(self):
        for op, kind, full, want in [('promotion', 'insert', False, 'promotion'), ('promotion-outer', 'read', False, 'promotion'),
                                     ('merge', 'insert', False, 'merge'), ('merge', 'insert', True, 'rewrite'), ('rewrote', 'vacuum', True, 'rewrite'),
                                     ('relocation', 'update', False, 'move'), ('shift', 'update', False, 'move'), ('shrink', 'update', False, 'move'),
                                     ('', 'insert', False, 'wbuf'), ('', 'delete', True, 'wbuf'), ('', 'vacuum', False, 'other'), ('sidecar', 'read', False, 'other'),
                                     ('worker', 'insert', False, 'other')]:
            self.assertEqual(wamp.group(op, kind, full), want, (op, kind, full))

    def test_opname(self):
        for what, want in [('promoted a sealed column segment of', 'promotion'), ('promoted (write buffer rows) the write buffer of', 'promotion-outer'),
                           ('merged 3 column segments into 1 of', 'merge'), ('shrunk', 'shrink'), ('rewrote (dirty units) the segments of', 'rewrote'),
                           ('refreshed net pre-aggregates of', 'sidecar'), ('column rewrite worker finished', 'worker')]:
            self.assertEqual(wamp.opname(what), want, what)


class Cells(unittest.TestCase):
    def test_cumulative_per_row(self):
        res = result([{'wbuf': 1000, 'merge': 500}] * 48)
        c = wamp.cells(res)
        self.assertEqual(sorted(c), [6, 12, 24, 48])
        self.assertEqual(c[6]['idx'], 15.0)
        self.assertEqual(c[48]['wbuf'], 10.0)
        self.assertEqual(c[48]['all'], 10.0)

    def test_short_run_has_no_cell(self):
        self.assertEqual(sorted(wamp.cells(result([{'wbuf': 1}] * 13))), [6, 12])

    def test_check(self):
        res = result([{'wbuf': 1000}] * 6)
        path = os.path.join(os.environ.get('TMPDIR', '/dev/shm'), f'focus-expect-{os.getpid()}.tsv')
        try:
            for value, bad in [('10.0', 0), ('10.9', 0), ('13.5', 1), ('6.0', 1)]:
                with open(path, 'w') as f:
                    f.write(f'std-ck24\t6\tidx\t{value}\t3.0\n')
                with redirect_stdout(io.StringIO()):
                    self.assertEqual(wamp.check(path, [res]), bad, value)
        finally:
            os.path.exists(path) and os.remove(path)


class Reads(unittest.TestCase):
    def test_rotate_moves_every_key(self):
        self.assertEqual(lab.delta(7), 7 * 53 % 400)
        self.assertEqual(lab.rotate(lab.Q['key'], 10), 'select count(*), sum(points) from facts where tenant_id = 133')
        self.assertIn('between 110 and 140', lab.rotate(lab.Q['range'], 10))
        self.assertIn('generate_series(110, 209)', lab.rotate(lab.Q['latest'], 10))
        self.assertIn('in (110,111', lab.rotate(lab.Q['top_sub'], 10))
        self.assertEqual(lab.rotate(lab.Q['point'], 10), lab.Q['point'])

    def test_phase_balanced_median(self):
        # phase 0 (the VACUUM batches) is a phase of its own: one slow phase moves the balanced median by one rank, not the mean
        s = [{'q': 'key', 'mode': 'warm', 'b': b, 'instr': 100 if b % 6 else 1000, 'hit': 4, 'read': 0, 'plan': 'sift'} for b in range(1, 25)]
        st = reads.stats(s, 'key', 'warm')
        self.assertEqual(st['pbm'], 100)
        self.assertEqual(st['mean'], 100 * 20 / 24 + 1000 * 4 / 24)
        self.assertEqual(st['p90'], 1000)
        self.assertIsNone(reads.stats(s, 'key', 'cold'))

    def test_spikes_do_not_move_the_trimmed_mean(self):
        s = [{'q': 'key', 'mode': 'warm', 'b': b, 'instr': 100 + b % 3, 'hit': 4, 'read': 0, 'plan': 'sift'} for b in range(1, 49)]
        s[5]['instr'] = 9000
        st = reads.stats(s, 'key', 'warm')
        self.assertEqual(st['spikes'], 1)
        self.assertLess(st['tmean'], 102)
        self.assertGreater(st['mean'], 280)

    def test_flags(self):
        def arm(x, hi=None):
            return [{'q': 'key', 'mode': 'warm', 'b': b, 'instr': hi if hi and b > 9 else x, 'hit': 1, 'read': 0, 'plan': 'sift'} for b in range(1, 13)]
        # a build 12 percent dearer in every sample flags nothing from one run a side (the p90 bar is 25 percent there); with three runs a side the p90 and the mean without spikes flag
        for new, runs, want in [(100, 3, []), (104, 3, []), (106, 3, ['mean without spikes']), (106, 1, []), (112, 1, []), (112, 3, ['p90', 'mean without spikes']), (130, 1, ['p90'])]:
            (row,) = compare.rows(arm(100), arm(new), ['key'], runs)
            self.assertEqual([f.rsplit(' +', 1)[0] for f in row[4]], want, (new, runs))
        # a tail only (a quarter of the samples 30 percent dearer): the p90 flags, the phase medians of the six phases do not
        (row,) = compare.rows(arm(100), arm(100, 130), ['key'], 1)
        self.assertEqual([f.rsplit(' +', 1)[0] for f in row[4]], ['p90'])


if __name__ == '__main__':
    unittest.main()
