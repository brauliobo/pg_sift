#!/usr/bin/env python3
"""bench/steps/iolib.py: the pure parts (the pg_stat_io query of PostgreSQL 17 and 18, the rows of a results file, the eviction list), then a live test on a tiny table in a
throwaway cluster under /var/tmp (about 10 s; skipped without initdb or pg_buffercache). It checks the cold run reads, the warm run after it does not, that EXPLAIN's blocks and pg_stat_io's
bytes agree, and that a script EXPLAIN cannot wrap still reports its WAL and size.

    python3 -I bench/test/test_iolib.py
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'steps'))
import iolib  # noqa: E402
from duckdb import Session  # noqa: E402


class Fake:
    """a session that answers the facts query and the pg_stat_io column probe like the server named by `columns`"""

    def __init__(self, has_stat_io=True, read_bytes=True):
        self.has, self.rb = has_stat_io, read_bytes
        self.seen = []

    def run(self, sql):
        self.seen.append(sql)
        if 'pg_backend_pid' in sql:
            return 0, ['4242|8192|%s' % ('t' if self.has else 'f')], '', ''
        if 'pg_attribute' in sql:
            return 0, ['1' if self.rb else '0'], '', ''
        return 0, [], '', ''


class Pure(unittest.TestCase):
    def test_stat_query_uses_the_byte_columns_where_they_exist(self):
        s18, s17 = Fake(read_bytes=True), Fake(read_bytes=False)
        sql18, sql17 = iolib.setup(s18)[2], iolib.setup(s17)[2]
        self.assertIn('sum(read_bytes)', sql18)
        self.assertNotIn('op_bytes', sql18)
        self.assertIn('sum(reads * op_bytes)', sql17)
        self.assertIn("object <> 'wal'", sql17)

    def test_no_pg_stat_io_degrades_to_the_plan(self):
        self.assertIsNone(iolib.setup(Fake(has_stat_io=False))[2])

    def test_delta_keeps_none_where_a_side_has_none(self):
        a = dict(stat=None, wal=100, proc=None)
        b = dict(stat=None, wal=250, proc=7)
        d = iolib.delta(a, b)
        self.assertEqual((d['wal_bytes'], d['stat_read_bytes'], d['proc_read_bytes']), (150, None, None))

    def test_explainable(self):
        for sql, want in [('SELECT 1', True), (' with x as (select 1) select * from x;', True), ('INSERT INTO t VALUES (1)', True), ('CREATE INDEX i ON t (a)', False),
                          ('VACUUM t', False), ('SELECT 1; SELECT 2', False), ('COPY t FROM STDIN', False)]:
            self.assertEqual(iolib.explainable(sql), want, sql)

    def test_metrics_of_a_cold_and_a_warm_run(self):
        cold = dict(cache='cold', read=277, hit_pct=0.0, read_bytes=2 * iolib.MB, proc_read_bytes=None, read_ms=1.5)
        warm = dict(cache='warm', read=0, hit_pct=100.0, read_bytes=0, proc_read_bytes=0, read_ms=None)
        self.assertEqual(iolib.metrics(cold), {'io_cold_read_blocks': '277', 'io_cold_hit_pct': '0.00', 'io_cold_read_mb': '2.00', 'io_cold_read_ms': '1.50'})
        self.assertEqual(iolib.metrics(warm), {'io_warm_read_blocks': '0', 'io_warm_hit_pct': '100.00'})
        timed = dict(warm, exec_ms=0.4567, cpu_ms=0.5)
        self.assertEqual(iolib.metrics(timed), {'io_warm_read_blocks': '0', 'io_warm_hit_pct': '100.00', 'io_warm_ms': '0.457', 'io_warm_cpu_ms': '0.500'})   # the time and the CPU of the run

    def test_load_metrics(self):
        self.assertEqual(iolib.load_metrics(dict(stat_extend_bytes=3 * iolib.MB, stat_write_bytes=None)), {'io_extend_mb': 3.0})

    def test_relations_quotes_the_names(self):
        sql = iolib.relations(["s.t'x"])
        self.assertIn("'s.t''x'::regclass", sql)
        self.assertIn('reltoastrelid', sql)

    def test_fs_type_takes_the_longest_mount_point(self):
        mounts = '/dev/a / ext4 rw 0 0\ntmpfs /dev/shm tmpfs rw 0 0\ntmpfs /dev/shm/deeper btrfs rw 0 0\n'
        with mock.patch('builtins.open', mock.mock_open(read_data=mounts)):
            self.assertEqual(iolib.fs_type('/dev/shm/x/data'), 'tmpfs')
            self.assertEqual(iolib.fs_type('/dev/shm/deeper/data'), 'btrfs')
            self.assertEqual(iolib.fs_type('/var/tmp/x'), 'ext4')
            self.assertEqual(iolib.fs_type('/dev/shmx'), 'ext4')


def which(name):
    return shutil.which(name) or shutil.which(name, path=subprocess.run(['pg_config', '--bindir'], capture_output=True, text=True).stdout.strip() or None)


@unittest.skipUnless(which('initdb') and which('pg_ctl'), 'needs initdb and pg_ctl')
class Live(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix='sift-io-selftest.', dir='/var/tmp')   # a disk: the page cache is there to be emptied
        with socket.socket() as sk:
            sk.bind(('127.0.0.1', 0))
            cls.port = sk.getsockname()[1]
        data = os.path.join(cls.dir, 'data')
        subprocess.run([which('initdb'), '-D', data, '-U', 'postgres', '-A', 'trust', '--locale=C'], check=True, capture_output=True)
        subprocess.run([which('pg_ctl'), '-D', data, '-w', '-l', os.path.join(cls.dir, 'log'), '-o', "-p %d -k %s -c listen_addresses='' -c shared_buffers=32MB -c fsync=off" % (cls.port, cls.dir), 'start'],
                       check=True, capture_output=True)
        cls.s = Session(cls.port, 60, None, db='postgres', host=cls.dir)
        if cls.s.run("SELECT count(*) FROM pg_available_extensions WHERE name = 'pg_buffercache'")[1] == ['0']:   # a server without its contrib package (the lint runner's) cannot evict
            cls.tearDownClass()
            raise unittest.SkipTest('needs the pg_buffercache extension')
        for sql in ('CREATE TABLE t AS SELECT i, md5(i::text) AS s FROM generate_series(1, 50000) i', 'CREATE INDEX ON t (i)', 'VACUUM (ANALYZE, FREEZE) t', 'SET max_parallel_workers_per_gather = 0'):
            assert not cls.s.run(sql)[2], sql
        cls.major = int(iolib.one(cls.s, "SELECT current_setting('server_version_num')::int / 10000")[0])

    @classmethod
    def tearDownClass(cls):
        cls.s.close()
        subprocess.run([which('pg_ctl'), '-D', os.path.join(cls.dir, 'data'), '-m', 'immediate', 'stop'], capture_output=True)
        shutil.rmtree(cls.dir, ignore_errors=True)

    def test_cold_then_warm(self):
        sql = 'SELECT count(*) FROM t WHERE i < 30000'
        cold = iolib.measure(self.s, sql, ['public.t'])
        warm = iolib.measure(self.s, sql)
        self.assertEqual((cold['cache'], warm['cache']), ('cold', 'warm'), cold['how'])
        self.assertGreater(cold['read'], 10)
        self.assertEqual((warm['read'], warm['hit_pct']), (0, 100.0))
        # the plan's blocks and the counters of pg_stat_io are one measure: a few blocks of the helper's own catalog queries are the difference
        self.assertAlmostEqual(cold['stat_read_bytes'] / 8192, cold['read'], delta=30)
        self.assertEqual(cold['read_bytes'], cold['read'] * 8192)   # the MB of a row are the plan's blocks
        self.assertEqual(warm['read_bytes'], 0)
        if cold['proc_read_bytes'] is not None and iolib.fs_type(self.dir) != 'tmpfs':
            self.assertGreater(cold['proc_read_bytes'], 0)   # physical bytes: a compressing file system reads fewer than the blocks say
        self.assertTrue(cold['how'].startswith('evict'), cold['how'])

    def test_cold_finds_pg_buffercache_whatever_the_search_path(self):
        # a lane's search_path is its case's schema (dropped after the case): the extension must not live there, and a later lane with another path must find it
        for path in ('lane_one', 'lane_two'):
            self.s.run('CREATE SCHEMA IF NOT EXISTS %s' % path)
            self.s.run('SET search_path = %s' % path)
            how = iolib.cool(self.s, ['public.t'])
            self.assertTrue(how.startswith('evict'), '%s: %s' % (path, how))
        self.s.run('RESET search_path')

    def test_workers_reads_are_counted(self):
        # a parallel scan: the workers read in other backends, which pg_stat_io names `background worker`; the blocks of the plan and its bytes are one measure
        self.s.run('SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0')
        try:
            io = iolib.measure(self.s, "SELECT count(*) FROM t WHERE s > 'x'", ['public.t'])
        finally:
            self.s.run('RESET ALL')
            self.s.run('SET max_parallel_workers_per_gather = 0')
        if io['cache'] == 'cold':
            self.assertAlmostEqual(io['stat_read_bytes'] / 8192, io['read'], delta=30)

    def test_a_write_dirties_blocks_and_writes_wal(self):
        io = iolib.measure(self.s, 'INSERT INTO t SELECT i, md5(i::text) FROM generate_series(1, 20000) i')
        self.assertGreater(io['dirtied'], 0)
        self.assertGreater(io['wal_bytes'], 0)
        self.assertGreater(io['cpu_ms'], 0)   # the backend's CPU of the run; a statement EXPLAIN can wrap also has its execution time
        self.assertGreater(io['exec_ms'], 0)

    def test_a_script_reports_its_wal_and_size(self):
        io = iolib.measure(self.s, 'CREATE INDEX t_s ON t (s); DROP INDEX t_s', None, ['public.t'])
        self.assertGreater(io['wal_bytes'], 0)
        self.assertEqual(io['size_bytes'], 0)   # dropped again
        io = iolib.measure(self.s, 'CREATE INDEX t_s ON t (s)', None, ['public.t'])
        self.assertGreater(io['size_bytes'], 0)
        self.assertIsNotNone(io['hit_pct'])
        self.assertIn('io_warm_read_blocks', iolib.metrics(io))   # a statement without a plan has the same rows

    def test_a_failing_statement_raises(self):
        with self.assertRaises(RuntimeError):
            iolib.measure(self.s, 'SELECT * FROM missing_table')


if __name__ == '__main__':
    unittest.main()
