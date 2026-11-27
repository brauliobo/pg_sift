"""The harness of the load gate without a server (scripts/load/run.py --selftest runs it): fakes of the control, sampler and worker sessions, and the shell
library of the TAP scripts with stubs of the node functions. One test a finding of Spec 116 (S116-L1..L7)."""
from __future__ import annotations

import math
import os
import subprocess
import sys
import tempfile
import threading
import types
import unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), 'bench', 'steps'))
import collect  # noqa: E402
import run  # noqa: E402
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)
from cases import Ceilings, LoadCase  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))


class FakeDb:
    """a session that answers 0, one sift relation (oid 42) and N problems of sift.check; ERROR makes the sampler statements fail"""

    def __init__(self, problems: int = 0, error: str = '', dead: bool = False):
        self.problems, self.error, self.dead = problems, error, dead

    def run(self, sql: str) -> list:
        if self.error:
            raise RuntimeError(self.error)
        return ['42'] if 'pg_am' in sql else []

    def val(self, sql: str) -> str:
        return str(self.problems) if 'sift.check' in sql else self.row(sql)[0]

    def row(self, sql: str) -> list:
        if self.error:
            raise RuntimeError(self.error)
        return ['0', '0', '0', ''] if sql == collect.WAITS else ['0']

    def q(self, sql: str):
        return ['0'], None, 0.0

    def close(self):
        pass


def ctx_of(db=None, obs_db=None) -> run.Ctx:
    ctx = run.Ctx.__new__(run.Ctx)
    ctx.a, ctx.db, ctx.obs_db, ctx.obs, ctx.open_sessions = types.SimpleNamespace(scale=1.0), db or FakeDb(), obs_db or FakeDb(), None, []
    ctx.fmt, ctx.testing = 2, True
    return ctx


def worker(seed: int = 1) -> types.SimpleNamespace:
    return types.SimpleNamespace(seed=seed, lat=[], ok=0, expected=0, bad=[])


class Harness(unittest.TestCase):
    def test_l1_every_log_is_scanned_and_a_log_made_anew_is_read_from_its_start(self):
        a = run.parse(['--log', 'primary.log', '--log', 'standby.log'])
        self.assertEqual(a.log, ['primary.log', 'standby.log'])
        with tempfile.TemporaryDirectory() as d:
            p, s = os.path.join(d, 'p.log'), os.path.join(d, 's.log')
            for f in (p, s):
                with open(f, 'w') as out:
                    out.write('LOG:  before the case\nTRAP: failed Assert("old")\n')
            log = collect.LogScan([p, s])
            log.mark()
            os.unlink(s)
            with open(s, 'w') as out:
                out.write('TRAP: failed Assert("clone")\n')
            with open(p, 'a') as out:
                out.write('LOG:  server process (PID 9) was terminated by signal 11: Segmentation fault\n')
            self.assertEqual(len(log.findings()), 2, log.findings())

    def test_l1_restandby_checks_the_standby_before_the_clone_erases_it(self):
        out = subprocess.run(['bash', '-c', 'f=$(sed -n "/^standby() /p; /^restandby() {/,/^}/p" test/t/core/load_repl.sh); node_leakcheck() { echo leakcheck $1; }; '
                              'node_clone() { echo clone $1 $2; }; node_start() { :; }; node_stop() { :; }; sql() { echo 0; }; '
                              'LS_LEAKS=/dev/stdout; eval "$f"; restandby'], cwd=REPO, capture_output=True, text=True)
        self.assertEqual([l.split()[0] for l in out.stdout.split('\n') if l.startswith(('leakcheck', 'clone'))], ['leakcheck', 'clone'], out.stdout + out.stderr)

    def test_l2_an_exception_of_a_worker_thread_is_an_unexpected_error(self):
        def boom(w, i):
            raise RuntimeError('XX000 the worker broke')

        out = ctx_of().phase({'w': ([worker()], boom)}, 0.05)
        self.assertEqual(len(out['w'].bad), 1)
        self.assertIn('the worker broke', out['w'].bad[0])

    def test_l3_the_hygiene_of_the_runner_adds_to_the_counts_of_a_case(self):
        ctx = ctx_of(FakeDb(problems=1))
        case = LoadCase('conc_x', 'conc', lambda c: {'verify_errors': 2, 'maint_failures': 1}, Ceilings())
        m, _ = run.measure(ctx, case, collect.LogScan([]))
        self.assertEqual((m['verify_errors'], m['maint_failures']), (3, 1))

    def test_l4_every_rule_of_the_leak_gate_is_a_rule_of_the_load_gate(self):
        rules = collect.leak_rules()
        self.assertGreaterEqual(len(rules), 21)
        for regex, sample in rules:
            self.assertTrue(collect.LEAKS.search(sample), f'{regex}: {sample}')

    def test_l5_the_sampler_stops_before_the_workers_end(self):
        ctx, w = ctx_of(), worker()

        def last(w, i):
            w.seen = ctx.obs.done.wait(2)
            return False

        ctx.phase({'w': ([w], last)}, 0.05)
        self.assertTrue(w.seen, 'the sampler was still running while the workers finished')

    def test_l6_an_error_of_the_sampler_fails_the_phase_and_a_lost_server_does_not(self):
        with self.assertRaisesRegex(RuntimeError, 'the sampler: 42P01'):
            ctx_of(obs_db=FakeDb(error='42P01 relation does not exist')).phase({'w': ([worker()], lambda w, i: False)}, 0.05)
        ctx_of(obs_db=FakeDb(error='08006 no server', dead=True)).phase({'w': ([worker()], lambda w, i: False)}, 0.05)

    def test_l7_a_lost_session_forgets_its_backend(self):
        p = run.Psql.__new__(run.Psql)
        p.n, p.dead, p._pid = 0, False, 1234

        class Gone:
            def write(self, s):
                raise BrokenPipeError

        p.p = types.SimpleNamespace(stdin=Gone(), wait=lambda: 0)
        self.assertEqual(p.q('SELECT 1')[1][0], '08006')
        self.assertIsNone(p._pid)

    def test_l7_no_latency_under_load_fails_the_ratio(self):
        self.assertTrue(math.isinf(collect.p99_ratio([1.0], [])))
        self.assertTrue(run.violations(Ceilings(p99_ratio=3.0), {'p99_ratio': collect.p99_ratio([1.0], [])}))

    def test_l7_every_suite_gets_the_crash_and_restart_hooks(self):
        out = subprocess.run(['bash', '-c', 'TAP_SCRIPTS=/s; . test/t/lib/load.bash; T=$(mktemp -d); trap "rm -rf $T" EXIT; mkdir $T/lw; dir() { echo $T/$1; }; port() { echo 1; }; crash() { :; }; real_node() { :; }; '
                              'diag() { echo "$@"; }; check() { :; }; python3() { echo "crash=$LOAD_CRASH restart=$LOAD_RESTART"; }; load_suite lw --suite workloads'],
                             cwd=REPO, capture_output=True, text=True, env=dict(os.environ, SIFT_REPO=REPO))
        self.assertIn('crash=crash lw', out.stdout, out.stdout + out.stderr)
        self.assertRegex(out.stdout, r"restart=.*-D '/[^']*/lw/data' -l '/[^']*/lw/postgres\.log'.*leakcheck\.sh' baseline", out.stdout + out.stderr)


def selftest():
    result = unittest.TextTestRunner(stream=sys.stderr, verbosity=0).run(unittest.defaultTestLoader.loadTestsFromTestCase(Harness))
    if not result.wasSuccessful():
        raise AssertionError(f'{len(result.failures) + len(result.errors)} harness test(s) failed')


if __name__ == '__main__':
    unittest.main()
