"""The harness of the load gate without a server (scripts/load/run.py --selftest runs it): fakes of the control, sampler and worker sessions, and the shell
library of the TAP scripts with stubs of the node functions. One test a finding of Spec 116 to 119 (S116-L1..L7, S117-L1..L5, S118-L1..L6, S119-L1..L4)."""
from __future__ import annotations

import ast
import inspect
import io
import math
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
import tokenize
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

    def __init__(self, problems: int = 0, error: str = '', dead: bool = False, left: str = '0'):
        self.problems, self.error, self.dead, self.left, self.sqls = problems, error, dead, left, []

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
        self.sqls.append(sql)
        return [self.left if sql == run.GONE else '0'], None, 0.0

    def close(self):
        pass


def ctx_of(db=None, obs_db=None) -> run.Ctx:
    ctx = run.Ctx.__new__(run.Ctx)
    ctx.a, ctx.db, ctx.obs_db, ctx.obs, ctx.open_sessions = types.SimpleNamespace(scale=1.0), db or FakeDb(), obs_db or FakeDb(), None, []
    ctx.fmt, ctx.testing, ctx.assert_build, ctx.workers, ctx.wedged, ctx.dirs = 2, True, False, [], 0, []
    return ctx


def worker(seed: int = 1, kill=None) -> types.SimpleNamespace:
    return types.SimpleNamespace(seed=seed, lat=[], ok=0, expected=0, bad=[], conn={}, p=types.SimpleNamespace(kill=kill), close=lambda: None)


def files() -> list:
    names = [os.path.join(HERE, f) for f in os.listdir(HERE)] + [os.path.join(HERE, 'suites', f) for f in os.listdir(os.path.join(HERE, 'suites'))]
    return sorted(n for n in names if n.endswith('.py'))


def sources() -> str:
    out = ''
    for n in files():
        if not n.endswith('test_harness.py'):
            with open(n) as f:
                out += f.read()
    return out


def quote(lit: str) -> str:
    body = lit.lstrip('rRfFbBuUtT')
    return body[:3] if body[:3] in ('"""', "'''") else body[:1]


def newer_fstrings(text: str) -> list:
    """what of TEXT Python 3.11 cannot read: a syntax error with feature_version 3.11, and the strings in the replacement fields of an f-string with its quote or a
    backslash (PEP 701: feature_version does not see them; a parser of 3.12 or newer tokenizes them)"""
    try:
        ast.parse(text, feature_version=(3, 11))
    except SyntaxError as e:
        return [f'line {e.lineno}: {e.msg}']
    out, outer = [], []
    for t in tokenize.generate_tokens(io.StringIO(text).readline):
        name = tokenize.tok_name[t.type]
        inner = name in ('STRING', 'FSTRING_START') and outer and (quote(t.string) == outer[-1] or '\\' in t.string)
        out += [f'line {t.start[0]}: {t.string[:40]}'] if inner else []
        outer += [quote(t.string)] if name == 'FSTRING_START' else []
        outer = outer[:-1] if name == 'FSTRING_END' else outer
    return out


def bash(script: str) -> subprocess.CompletedProcess:
    return subprocess.run(['bash', '-c', script], cwd=REPO, capture_output=True, text=True, env=dict(os.environ, SIFT_REPO=REPO))


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
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, 'postgres.log'), 'w') as f:
                f.write('LOG:  fine\n2026 [7] [XX000] ERROR:  cache lookup failed for relation 1\n')
            out = bash(f'f=$(sed -n "/^standby() /p; /^restandby() {{/,/^}}/p" test/t/core/load_repl.sh); node_leakcheck() {{ echo leakcheck $1; }}; '
                       f'node_clone() {{ echo clone $1 $2; }}; node_start() {{ :; }}; node_stop() {{ :; }}; sql() {{ echo 0; }}; dir() {{ echo {d}; }}; '
                       'LS_LEAKS=/dev/stdout; eval "$f"; restandby')
        lines = out.stdout.split('\n')
        self.assertEqual([l.split()[0] for l in lines if l.startswith(('leakcheck', 'clone'))], ['leakcheck', 'clone'], out.stdout + out.stderr)
        self.assertTrue(any('cache lookup failed' in l for l in lines), 'the lines of the standby log beyond the leak rules are kept before the clone')

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
        with self.assertRaisesRegex(RuntimeError, 'the sampler: RuntimeError: 42P01'):
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

    def test_s117_l1_a_wedged_worker_ends_its_phase_bounded_and_counts(self):
        ctx, gate = ctx_of(), threading.Event()
        ctx.a.scale = 0.01
        w = worker(kill=gate.set)

        def stuck(w, i):
            gate.wait(10)
            return False

        t0 = time.monotonic()
        out = ctx.phase({'w': ([w], stuck)}, 0.05)
        self.assertLess(time.monotonic() - t0, 5)
        self.assertEqual(ctx.wedged, 1)
        self.assertTrue(out['w'].bad and out['w'].bad[0].startswith('wedged: '), out['w'].bad)
        self.assertIn(run.STUCK, ctx.db.sqls)

    def test_s117_l1_restandby_and_restart_waits_are_bounded(self):
        with open(os.path.join(REPO, 'test/t/core/load_repl.sh')) as f:
            self.assertNotRegex(f.read(), r'until \[\[ \$\(sql lp')
        out = bash('. test/t/lib/load.bash; declare -f load_suite')
        self.assertIn('timeout 120 bash -c', out.stdout, out.stderr)

    def test_s117_l2_every_error_of_a_batch_is_judged(self):
        self.assertEqual(run.unexpected([('57014', 'cancel'), ('XX000', 'boom')], ('57014',)), ('XX000', 'boom'))
        self.assertIsNone(run.unexpected([('57014', 'cancel'), ('25P02', 'aborted')], ('57014',)))
        self.assertEqual(run.unexpected([('25P02', 'aborted')], ('57014',)), ('25P02', 'aborted'))
        self.assertIsNone(run.unexpected([('57P01', 'gone')], ('08006',)))
        self.assertIsNone(run.unexpected([('25P02', 'aborted')], ('57014',), cascade=True), 'the batch after a cancelled COMMIT')
        self.assertEqual(run.unexpected([('25P02', 'aborted'), ('XX000', 'boom')], ('57014',), cascade=True), ('XX000', 'boom'))
        self.assertEqual(re.findall(r"(?<!LOCAL )statement_timeout = '\d+ms'|RESET statement_timeout", sources()), [])

    def test_s117_l3_the_log_carries_the_sqlstate_and_an_internal_error_is_a_finding(self):
        self.assertTrue(collect.LEAKS.search('2026-11-27 [42] [XX000] ERROR:  unexpected chunk number'))
        self.assertFalse(collect.LEAKS.search('2026-11-27 [42] [57014] ERROR:  canceling statement due to user request'))
        conf = bash('. test/t/lib/load.bash; echo "$LOAD_CONF"').stdout
        self.assertIn("log_line_prefix = '%m [%p] [%e] '", conf)
        for t in ('load_gate', 'load_workloads', 'load_repl', 'load_stress'):
            with open(os.path.join(REPO, f'test/t/core/{t}.sh')) as f:
                body = f.read()
            self.assertIn('$LOAD_CONF"', body, t)
            self.assertIn('load_suite ', body, t)

    def test_s117_l4_a_case_without_progress_fails(self):
        c0 = LoadCase('conc_x', 'conc', lambda ctx: {}, Ceilings())
        for k in ('ops', 'tiered', 'standby_reads', 'shifts'):
            self.assertEqual(run.judge(c0, {k: 0}, [])[0], 'FAIL', k)
            self.assertEqual(run.judge(c0, {k: 1}, [])[0], 'PASS', k)

    def test_s117_l4_any_exception_of_a_body_drops_its_schema_and_reads_the_log(self):
        db = FakeDb()

        def body(ctx):
            raise ValueError('a harness bug')

        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, 'p.log')
            open(log, 'w').close()
            ctx = ctx_of(db)
            run.reconnect = lambda ctx: ctx.db
            run.Psql = lambda **kw: FakeDb()
            try:
                scan = collect.LogScan([log])
                case = LoadCase('conc_x', 'conc', lambda c: (open(log, 'a').write('TRAP: failed Assert("x")\n'), body(c)), Ceilings())
                m, bad = run.measure(ctx, case, scan)
            finally:
                run.reconnect, run.Psql = REAL
        self.assertIsNone(m)
        self.assertTrue(bad[0].startswith('ValueError: a harness bug') and any('TRAP' in b for b in bad), bad)
        self.assertTrue(any(s.startswith('DROP SCHEMA IF EXISTS load_case') for s in db.sqls), db.sqls)

    def test_s117_l5_hygiene_skips_damaged_relations_and_sees_tier_chunks_and_every_database(self):
        seen = []

        class Db(FakeDb):
            def val(self, sql):
                seen.append(sql)
                return super().val(sql)

        collect.hygiene(Db(), ('42',))
        self.assertTrue(any('maint_failures' in s and 'NOT IN (42)' in s for s in seen), seen)
        self.assertIn(collect.TIER_TMP, seen)
        self.assertNotIn('current_database', run.DEADLOCKS)

    def test_s117_l5_the_options_of_the_caller_are_kept(self):
        p = run.Psql.__new__(run.Psql)
        p.app, p.dbname, p.conn = 'load_ctl', 'postgres', {}
        old = {k: os.environ.get(k) for k in ('PGOPTIONS', 'LOAD_PGOPTIONS')}
        os.environ.update(PGOPTIONS='-c work_mem=64MB', LOAD_PGOPTIONS='-c sift.test_create_format1=on')
        try:
            self.assertEqual(p.env()['PGOPTIONS'], '-c work_mem=64MB -c sift.test_create_format1=on -c client_min_messages=error')
        finally:
            for k, v in old.items():
                os.environ.pop(k) if v is None else os.environ.update({k: v})

    def test_s117_l5_lint_covers_the_load_harness(self):
        with open(os.path.join(REPO, 'scripts/check-test-hygiene.py')) as f:
            self.assertIn("'scripts/load'", f.read())
        with open(os.path.join(REPO, 'scripts/lint.sh')) as f:
            self.assertIn('test/t/lib/*.bash', f.read())

    def test_s118_l1_the_errors_of_every_phase_count(self):
        ctx, w = ctx_of(), worker()
        ctx.workers = [w]

        def warm(w, i):
            w.bad.append('XX000 in the warm-up')
            return False

        first = ctx.phase({'w': ([w], warm)}, 0.05)
        busy = ctx.phase({'w': ([w], lambda w, i: False)}, 0.05)
        self.assertEqual((len(first['w'].bad), busy['w'].bad), (1, []))
        self.assertEqual(run.worker_errors(ctx)['unexpected_errors'], 1)

    def test_s118_l2_a_failover_that_fails_still_clones_a_new_standby(self):
        from suites import repl
        ctx, hooks = ctx_of(), []
        ctx.hook = hooks.append
        env = {k: os.environ.pop(k) for k in ('LOAD_STANDBY_HOST', 'LOAD_STANDBY_PORT') if k in os.environ}
        try:
            with self.assertRaises(RuntimeError):
                repl.repl_standby_failover(ctx)
        finally:
            os.environ.update(env)
        self.assertEqual(hooks, ['LOAD_RESTANDBY'])

    def test_s118_l3_the_dsm_check_waits_for_a_leaving_session_and_close_counts_wedged_backends(self):
        out = bash('. scripts/leakcheck.sh; T=$(mktemp); trap "rm -f $T" EXIT; leak_launchers() { echo 0; }; '
                   'leak_psql() { case "$*" in *busy*) n=$(($(cat $T 2>/dev/null || echo 0) + 1)); echo $n >$T; ((n < 3)) && echo busy || echo primary ;; '
                   '*format*) echo "shared table t is mapped by 1 backend(s) with no session left" ;; esac; }; LEAK_DSM_FN=x; leak_shared_tables /x')
        self.assertIn('shared table t', out.stdout, out.stdout + out.stderr)
        self.assertEqual(run.gone(FakeDb(left='2'), 0.05), 2)
        ctx = ctx_of(FakeDb(left='3'))
        ctx.open_sessions = [worker()]
        run.gone, gone = (lambda db, cap=0.05: REAL_GONE(db, cap)), run.gone
        try:
            ctx.close_sessions()
        finally:
            run.gone = gone
        self.assertEqual(ctx.wedged, 3)

    def test_s118_l4_a_snapshot_pair_splits_at_its_marker(self):
        self.assertEqual(collect.halves_differ(['__SIFT_SEP__', '1|1', '1|1']), 1)
        self.assertEqual(collect.halves_differ(['1|1', '__SIFT_SEP__', '1|1']), 0)
        self.assertEqual(collect.halves_differ(None), 0)
        with self.assertRaises(ValueError):
            collect.halves_differ(['1|1', '1|1'])
        self.assertIn(collect.SEP, collect.snap('SELECT 1', 'SELECT 2'))
        self.assertNotIn('len(out) // 2', sources(), 'a split without its marker')

    def test_s118_l5_a_session_that_fails_to_open_leaks_none_and_the_sampler_catches_all(self):
        made = []

        class W:
            def __init__(self, *a, **kw):
                if len(made) == 2:
                    raise RuntimeError('42P01 in the start of a session')
                made.append(self)

        ctx, real = ctx_of(), run.Worker
        run.Worker = W
        try:
            with self.assertRaises(RuntimeError):
                ctx.sessions(4)
        finally:
            run.Worker = real
        self.assertEqual((ctx.open_sessions, ctx.workers), (made, made))

        class Bad(FakeDb):
            def row(self, sql):
                raise ValueError('a bad sample')

        o = collect.Observer(Bad())
        o.loop()
        self.assertIn('ValueError: a bad sample', o.error)

    def test_s118_l6_counts_after_the_sessions_end(self):
        self.assertEqual(re.findall(r"m\['tmp_files_left'\] =|hygiene\(ctx\.db\)\.items\(\)", sources()), [])

    def test_s119_l1_a_crash_resets_the_deadlocks_the_case_counts_from(self):
        class Db(FakeDb):
            seen = iter(['5', '2'])

            def val(self, sql):
                return next(self.seen) if sql == run.DEADLOCKS else super().val(sql)

        ctx = ctx_of(Db())
        ctx.hook = lambda var: None
        m, _ = run.measure(ctx, LoadCase('fault_x', 'fault', lambda c: (c.restart('LOAD_CRASH', 'LOAD_RESTART'), {})[1], Ceilings()), collect.LogScan([]))
        self.assertEqual((m['deadlocks'], ctx.restarted), (2, True))

    def test_s119_l2_the_wave_three_checks_see_nan_quantiles_a_crashed_column_index_and_share_one_cap(self):
        from suites import workloads_prod3 as w3
        for out, bad in ((['1|{1,2,3,4}|2'], 0), (['NaN|{1,2,3,4}|2'], 1), (['1|{-1,2,3,4}|2'], 1), (['1|{1,2,3,NaN}|2'], 1), (['1|{1,2,3,1000}|2'], 1)):
            self.assertEqual(w3.kll_wrong(out), bad, out)
        crash = inspect.getsource(w3.fault_unlogged_kill9_reset)
        self.assertIn("AGG.format(t='e_col')", crash)
        self.assertIn('not_restarted=', crash)
        self.assertNotIn('end + CAP', inspect.getsource(w3.repl_logical_rif_pk_ddl))

    def test_s119_l3_materialized_views_are_checked(self):
        self.assertIn("c.relkind IN ('r', 'i', 'm')", collect.SIFT_RELS)

    def test_s119_l4_every_file_reads_on_python_3_11(self):
        self.assertTrue(newer_fstrings("x = f'{d['k']}'") and newer_fstrings("x = f'{chr(10) + \"\\\\n\"}'") and newer_fstrings('def f[T](x): pass'))
        self.assertEqual(newer_fstrings("x = f'{d[\"k\"]}' + f\"{d['k']}\" + f\"\"\"{d['k']}\"\"\""), [])
        for n in files():
            with open(n) as f:
                self.assertEqual(newer_fstrings(f.read()), [], n)


REAL = (run.reconnect, run.Psql)
REAL_GONE = run.gone


def selftest():
    result = unittest.TextTestRunner(stream=sys.stderr, verbosity=0).run(unittest.defaultTestLoader.loadTestsFromTestCase(Harness))
    if not result.wasSuccessful():
        raise AssertionError(f'{len(result.failures) + len(result.errors)} harness test(s) failed')


if __name__ == '__main__':
    unittest.main()
