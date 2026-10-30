#!/usr/bin/env python3
"""Tests of steps/meter.py and of the CPU and buffers per transaction that steps/cell.py prints: the process tree on a shell with a busy child, and a throwaway
Postgres cluster under /var/tmp (about 5 s; skipped without initdb). Run: python3 -I bench/test/test_meter.py"""
import os, shutil, socket, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
STEPS = os.path.join(HERE, '..', 'steps')
sys.path.insert(0, STEPS)
import meter  # noqa: E402


def which(name):
    return shutil.which(name) or shutil.which(name, path=subprocess.run(['pg_config', '--bindir'], capture_output=True, text=True).stdout.strip() or None)


class Tree(unittest.TestCase):
    def test_stat_survives_a_name_with_spaces_and_parentheses(self):
        self.assertEqual(meter.stat(os.getpid())[0], os.getppid())

    def test_cpu_of_a_child_that_ended_is_in_its_parent(self):
        sh = subprocess.Popen(['sh', '-c', f'{sys.executable} -c "sum(range(4 * 10**6))"; sleep 3'])
        try:
            while not meter.stat(sh.pid)[2]:   # the child is waited for: its time moves to the parent's
                pass
            self.assertGreater(meter.cpu_ms(sh.pid), 20)
        finally:
            sh.kill()
            sh.wait()

    def test_per_query(self):
        m = meter.Meter(os.getpid())
        m.cpu, m.blocks = 40.0, 256
        self.assertEqual(m.per(4), (10.0, 64 * meter.BLOCK_MB))
        self.assertEqual(m.per(0), (None, None))
        m.blocks = None
        self.assertEqual(m.per(2), (20.0, None))


@unittest.skipUnless(which('initdb') and which('pg_ctl') and which('pgbench'), 'needs initdb, pg_ctl and pgbench')
class Live(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix='sift-meter-selftest.', dir='/var/tmp')
        with socket.socket() as sk:
            sk.bind(('127.0.0.1', 0))
            cls.port = sk.getsockname()[1]
        cls.data = os.path.join(cls.dir, 'data')
        subprocess.run([which('initdb'), '-D', cls.data, '-U', 'postgres', '-A', 'trust', '--locale=C'], check=True, capture_output=True)
        subprocess.run([which('pg_ctl'), '-D', cls.data, '-w', '-l', os.path.join(cls.dir, 'log'), '-o', f"-p {cls.port} -k {cls.dir} -c listen_addresses='' -c fsync=off", 'start'], check=True, capture_output=True)
        cls.env = {**os.environ, 'PGPORT': str(cls.port), 'PGHOST': cls.dir, 'PGUSER': 'postgres'}
        cls.psql('-c', 'CREATE TABLE t AS SELECT i, md5(i::text) AS s FROM generate_series(1, 20000) i', '-c', 'VACUUM t')
        cls.pages = int(cls.psql('-c', "SELECT pg_relation_size('t') / 8192")[0])
        with open(os.path.join(cls.dir, 'q.pgb'), 'w') as f:
            f.write('SELECT count(*) FROM t;\n')

    @classmethod
    def psql(cls, *args):
        return subprocess.run(['psql', '-X', '-qAt', '-d', 'postgres', *args], env=cls.env, capture_output=True, text=True, check=True).stdout.split()

    @classmethod
    def tearDownClass(cls):
        subprocess.run([which('pg_ctl'), '-D', cls.data, '-m', 'immediate', 'stop'], capture_output=True)
        shutil.rmtree(cls.dir, ignore_errors=True)

    def check(self, fields):
        cpu, mb = float(fields[-2]), float(fields[-1])
        self.assertGreater(cpu, 0)
        self.assertGreater(mb, self.pages * 8 / 1024 * 0.9)   # every query scans the table: at least its pages
        self.assertLess(mb, self.pages * 8 / 1024 * 3)

    @unittest.skipUnless(os.path.exists('/proc/self/schedstat'), 'the CPU of a cell is read from /proc (Linux)')
    def test_pgbench_cell(self):
        out = subprocess.run([sys.executable, '-I', os.path.join(STEPS, 'cell.py'), 'qps', '--max-time', '1', '--tmp', self.dir, '--server', self.data, '--db', 'postgres', '--',
                              '-n', '-c', '2', '-f', os.path.join(self.dir, 'q.pgb'), 'postgres'], env=self.env, capture_output=True, text=True).stdout.split('\t')
        self.assertEqual(len(out), 6, out)
        self.check(out)

    @unittest.skipUnless(os.path.exists('/proc/self/schedstat'), 'the CPU of a cell is read from /proc (Linux)')
    def test_batch_cell(self):
        plan, res = os.path.join(self.dir, 'plan'), os.path.join(self.dir, 'out')
        with open(plan, 'w') as f:
            f.write(f'a\t{self.port}\tpostgres\t{os.path.join(self.dir, "q.pgb")}\n')
        env = {**self.env, 'PGHOST': self.dir}
        run = subprocess.run([sys.executable, '-I', os.path.join(STEPS, 'cell.py'), 'batch', plan, res, '--max-time', '1'], env=env, capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        with open(res) as f:
            row = f.read().rstrip('\n').split('\t')
        self.assertEqual(len(row), 9, row)
        self.check(row[:-1])


if __name__ == '__main__':
    unittest.main()
