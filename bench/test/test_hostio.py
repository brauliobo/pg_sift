"""bench/steps/hostio.py on a fake /proc and /sys: the device, process and cgroup counters, their differences, the order of the sources of the average read size, and what is absent
where the host has no device (tmpfs) or no cgroup. No server."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'steps'))
import engine_io  # noqa: E402
import hostio  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../steps'))
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)

DISKSTATS = '259 2 nvme0n1p2 {r} 5 {s} 100 {w} 3 {ws} 50 0 10 20 0 0 0 0 {f} 7\n'


class Fake:
    def __init__(self, tmp):
        self.root = tmp

    def put(self, path, text):
        p = os.path.join(self.root, path.lstrip('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'w').write(text)

    def disk(self, r, s, w=0, ws=0, f=0):
        self.put('/proc/diskstats', DISKSTATS.format(r=r, s=s, w=w, ws=ws, f=f))

    def proc(self, pid, parent, syscr, rb, wb=0, cg='0::/system.slice/db.scope'):
        self.put('/proc/%d/stat' % pid, '%d (db worker) S %d 1 1 0\n' % (pid, parent))
        self.put('/proc/%d/io' % pid, 'rchar: %d\nwchar: 1\nsyscr: %d\nsyscw: 1\nread_bytes: %d\nwrite_bytes: %d\n' % (rb * 2, syscr, rb, wb))
        self.put('/proc/%d/cgroup' % pid, cg + '\n')

    def cgroup(self, rios, rbytes, wios=0, wbytes=0):
        self.put('/sys/fs/cgroup/system.slice/db.scope/io.stat', '259:0 rbytes=%d wbytes=%d rios=%d wios=%d dbytes=0 dios=0\n8:0 rbytes=1 wbytes=0 rios=1 wios=0\n' % (rbytes, wbytes, rios, wios))


class Sources(unittest.TestCase):
    def setUp(self):
        self._t = tempfile.TemporaryDirectory()
        self.addCleanup(self._t.cleanup)
        self.f = Fake(self._t.name)

    def test_the_device_line_is_read_in_bytes_and_the_flush_count_is_optional(self):
        self.f.disk(10, 200, w=4, ws=8, f=2)
        self.assertEqual(hostio.disk_counters(('disk', 'nvme0n1p2'), self.f.root), dict(reads=10, merged=5, read_bytes=200 * 512, writes=4, write_bytes=8 * 512, flushes=2))
        self.f.put('/proc/diskstats', '259 2 nvme0n1p2 10 5 200 100 4 3 8 50 0 10 20\n')   # a kernel before discard and flush counters
        self.assertIsNone(hostio.disk_counters(('disk', 'nvme0n1p2'), self.f.root)['flushes'])
        self.assertIsNone(hostio.disk_counters(('disk', 'sda'), self.f.root))

    def test_zfs_reads_the_pool_kstat(self):
        self.f.put('/proc/spl/kstat/zfs/tank/io', '13 1 0x01 12 3264 1 2\nnread nwritten reads writes wtime\n4096 8192 7 3 99\n')
        self.assertEqual(hostio.disk_counters(('zfs', 'tank'), self.f.root), dict(reads=7, merged=None, read_bytes=4096, writes=3, write_bytes=8192, flushes=None))

    def test_the_tree_sums_the_children_and_skips_what_is_not_readable(self):
        self.f.proc(100, 1, 5, 4096)
        self.f.proc(101, 100, 7, 8192)
        self.f.proc(200, 1, 1000, 1 << 30)   # another program
        self.assertEqual(sorted(hostio.tree(100, self.f.root)), [100, 101])
        self.assertEqual(hostio.proc_counters([100, 101, 999], self.f.root), dict(rchar=2 * (4096 + 8192), syscr=12, read_bytes=12288, write_bytes=0))
        self.assertIsNone(hostio.proc_counters([999], self.f.root))

    def test_a_host_without_proc_has_no_process_counters_and_no_error(self):
        nowhere = os.path.join(self.f.root, 'nowhere')
        self.assertEqual(hostio.tree(100, nowhere), [100])
        self.assertIsNone(hostio.snapshot([100], None, nowhere)['proc'])

    def test_the_cgroup_adds_every_device(self):
        self.f.proc(100, 1, 1, 1)
        self.f.cgroup(rios=9, rbytes=1000)
        self.assertEqual(hostio.cgroup_counters(100, self.f.root), dict(rios=10, rbytes=1001, wios=0, wbytes=0))
        self.f.put('/proc/100/cgroup', '12:cpu:/x\n')   # cgroup v1: no unified line
        self.assertIsNone(hostio.cgroup_counters(100, self.f.root))


class Figures(unittest.TestCase):
    def setUp(self):
        self._t = tempfile.TemporaryDirectory()
        self.addCleanup(self._t.cleanup)
        self.f = Fake(self._t.name)
        self.f.proc(100, 1, 10, 0)

    def snap(self):
        return hostio.snapshot([100], None, self.f.root)

    def test_a_run_reports_the_difference_and_the_device_gives_the_average(self):
        self.f.disk(10, 200)
        self.f.cgroup(5, 0)
        a = dict(disk=hostio.disk_counters(('disk', 'nvme0n1p2'), self.f.root), proc=hostio.proc_counters([100], self.f.root), cgroup=hostio.cgroup_counters(100, self.f.root))
        self.f.disk(110, 1000200, w=3, ws=16, f=1)
        self.f.proc(100, 1, 30, 2 << 20)
        self.f.cgroup(105, 100 << 20)
        b = dict(disk=hostio.disk_counters(('disk', 'nvme0n1p2'), self.f.root), proc=hostio.proc_counters([100], self.f.root), cgroup=hostio.cgroup_counters(100, self.f.root))
        got = hostio.figures(a, b)
        self.assertEqual((got['dev_read_iops'], got['dev_write_iops'], got['dev_flushes'], got['proc_read_ops'], got['cg_read_ios']), (100, 3, 1, 20, 100))
        self.assertAlmostEqual(got['dev_read_mb'], 1000000 * 512 / hostio.MB)
        self.assertAlmostEqual(got['avg_kb_per_read'], 1000000 * 512 / 1024 / 100)

    def test_without_a_device_the_columns_are_absent_and_the_average_comes_from_the_next_source(self):
        a = self.snap()
        self.f.proc(100, 1, 110, 100 << 20)
        got = hostio.figures(a, self.snap())
        self.assertIsNone(got['dev_read_iops'])
        self.assertIsNone(got['cg_read_ios'])
        self.assertEqual(got['proc_read_ops'], 100)
        self.assertAlmostEqual(got['avg_kb_per_read'], 100 * 1024 / 100)
        self.assertEqual(hostio.figures(dict(disk=None, proc=None, cgroup=None), dict(disk=None, proc=None, cgroup=None))['avg_kb_per_read'], None)

    def test_reads_that_asked_the_device_for_nothing_have_no_average(self):
        a = self.snap()
        self.f.proc(100, 1, 110, 0)
        got = hostio.figures(a, self.snap())
        self.assertEqual(got['proc_read_ops'], 100)
        self.assertIsNone(got['avg_kb_per_read'])

    def test_a_path_is_mapped_to_its_block_device_and_a_file_system_with_no_device_to_none(self):
        os.makedirs(os.path.join(self.f.root, 'sys/devices/pci/nvme0n1'))
        os.makedirs(os.path.join(self.f.root, 'sys/dev/block'))
        os.symlink('../../devices/pci/nvme0n1', os.path.join(self.f.root, 'sys/dev/block/8:0'))
        with mock.patch.object(hostio.os, 'stat', return_value=SimpleNamespace(st_dev=os.makedev(8, 0))):
            self.assertEqual(hostio.device_of('/data', self.f.root), ('disk', 'nvme0n1'))
        self.f.put('/proc/mounts', 'tank/db /srv/db zfs rw 0 0\ntmpfs /dev/shm tmpfs rw 0 0\n')
        with mock.patch.object(hostio.os, 'stat', return_value=SimpleNamespace(st_dev=os.makedev(0, 55))):
            self.assertEqual(hostio.device_of('/srv/db/pgdata', self.f.root), ('zfs', 'tank'))
            self.assertIsNone(hostio.device_of('/dev/shm/x', self.f.root))

    def test_the_command_prints_a_snapshot_and_its_delta(self):
        out = lambda *a: subprocess.run([sys.executable, '-I', os.path.join(HERE, '..', 'steps', 'hostio.py'), *a], capture_output=True, text=True, check=True).stdout  # noqa: E731
        snap = out('snap', '--pid', str(os.getpid()))
        self.assertIn('proc', json.loads(snap))
        path = os.path.join(self._t.name, 'a.json')
        open(path, 'w').write(snap)
        lines = dict(l.split('\t') for l in out('delta', path, '--pid', str(os.getpid())).splitlines())
        self.assertIn('proc_read_ops', lines)
        self.assertEqual(lines['dev_read_iops'], '-')


class Engines(unittest.TestCase):
    class Fake:
        def __init__(self):
            self.n = 0

        def pids(self):
            return [os.getpid()]

        def datadir(self):
            return '/dev/shm'   # tmpfs: no device

        def native_io(self):
            self.n += 7
            return {'es_read_operations': self.n}

    def test_each_call_is_measured_with_the_engines_counters_and_the_median_of_the_timed_calls_is_kept(self):
        eng, sink = self.Fake(), []
        go = engine_io.metered(eng, lambda: (1.0, [['x']]), sink)
        self.assertEqual([go() for _ in range(3)], [(1.0, [['x']])] * 3)
        self.assertEqual(len(sink), 3)
        self.assertEqual(sink[0]['es_read_operations'], 7)
        self.assertIsNone(sink[0]['dev_read_iops'])
        sink[:] = [{'a': 1, 'b': 1}, {'a': 5, 'b': None}, {'a': 3, 'b': 2}]
        self.assertEqual(engine_io.medians(sink, 2), {'a': 4.0})   # the first call is the untimed one; b is absent where a timed call lacks it
        self.assertEqual(engine_io.medians([], 3), {})

    @unittest.skipUnless(os.path.exists('/proc/self/io'), 'the process counters are those of the /proc of Linux')
    def test_the_process_counters_of_the_engine_are_in_each_call(self):
        sink = []
        engine_io.metered(self.Fake(), lambda: (1.0, []), sink)()
        self.assertIsNotNone(sink[0]['proc_read_ops'])


if __name__ == '__main__':
    unittest.main()
