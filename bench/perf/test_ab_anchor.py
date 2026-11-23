"""Layer 3 tests: the quiet-host guard, instruction counters and the pinned release anchor. python3 -m unittest discover -s bench/perf -v."""
import contextlib
import io
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from test_perf_gate import gate


def pressure_dir(root, cpu, io_pressure):
    for name,value in (('cpu',cpu),('io',io_pressure)):
        (Path(root)/name).write_text(f'some avg10={value:.2f} avg60={value:.2f} avg300={value:.2f} total=0\n')
    return root


class Script:
    def __init__(self, *codes):
        self.codes,self.calls = list(codes),[]

    def __call__(self, *args):
        self.calls.append([str(a) for a in args])
        return subprocess.CompletedProcess(args,self.codes.pop(0) if self.codes else 0,'',f'pressure {len(self.calls)}\n')


class QuietHost(unittest.TestCase):
    def test_a_disabled_guard_never_asks_the_host(self):
        script = Script()
        guard = gate.QuietGuard(False,script=script)
        self.assertTrue(guard.settle(math.inf))
        self.assertEqual(script.calls,[])
        self.assertEqual(guard.summary()['blocks'],0)

    def test_a_quiet_host_times_the_block_and_a_noisy_one_marks_it(self):
        for code,quiet in ((0,True),(1,False),(3,False)):
            with self.subTest(code=code):
                guard = gate.QuietGuard(True,10,90,Script(code))
                self.assertEqual(guard.settle(math.inf),quiet)
                self.assertEqual((guard.blocks,guard.noisy_blocks),(1,0 if quiet else 1))

    def test_the_first_wait_is_long_a_wait_after_a_noisy_block_is_short_and_the_budget_bounds_both(self):
        script = Script(1,1,0,0)
        guard = gate.QuietGuard(True,7,90,script)
        now = gate.time.monotonic()
        for deadline in (now+1000,now+1000,now+1000,now+3.5):
            guard.settle(deadline)
        waits = [call[call.index('--timeout')+1] for call in script.calls]
        self.assertEqual(waits[:2],['90',str(gate.NOISY_RECHECK_SECONDS)])
        self.assertEqual(waits[2],str(gate.NOISY_RECHECK_SECONDS))
        self.assertEqual(waits[3],'3')
        self.assertEqual({call[call.index('--max')+1] for call in script.calls},{'7'})
        self.assertEqual(guard.summary()['noisy_blocks'],2)

    def test_a_noisy_block_is_marked_and_cannot_decide_a_verdict(self):
        class Rev:
            def __init__(self,side): self.conn = side
        for code,noisy,expected in ((0,False,'pass'),(1,True,'inconclusive')):
            with self.subTest(noisy=noisy):
                with patch.object(gate,'QUIET',gate.QuietGuard(True,script=Script(code))), \
                     patch.object(gate,'elapsed',return_value=1.0), patch.object(gate,'prepare'):
                    saved = gate.block([Rev('A'),Rev('B')],gate.CASES[0],3,math.inf)
                self.assertEqual(saved['noisy'],noisy)
                self.assertEqual(gate.verdict(saved),expected)
                self.assertEqual(gate.verdict(saved,True),expected)

    def test_the_real_script_reports_a_busy_host_and_a_host_without_pressure_information(self):
        with tempfile.TemporaryDirectory() as tmp:
            for label,proc,code in (('busy',pressure_dir(tmp,90,2),gate.QUIET_TIMEOUT),
                                    ('no psi',os.path.join(tmp,'absent'),gate.QUIET_NO_PSI)):
                with self.subTest(label), patch.dict(os.environ,SIFT_QUIET_PROC=proc):
                    guard = gate.QuietGuard(True,10,0)
                    self.assertFalse(guard.settle(math.inf))
                    self.assertEqual(gate.quiet_script('wait','--max',10,'--timeout',0).returncode,code)

    def test_a_real_gate_waits_for_a_quiet_host_and_the_loaded_flag_is_for_smoke_runs(self):
        parsed = gate.parser().parse_args(['--baseline-ref','a','--candidate-ref','b','--output','o','--scratch-root','s'])
        self.assertFalse(parsed.allow_loaded)
        self.assertEqual((parsed.quiet_max,parsed.quiet_timeout),(gate.QUIET_MAX_PERCENT,gate.QUIET_TIMEOUT_SECONDS))
        for flag,guarded in (([],True),(['--allow-loaded'],False)):
            with self.subTest(flag=flag), tempfile.TemporaryDirectory() as tmp, \
                 patch.object(gate,'exact_sha',side_effect=gate.BudgetExpired('stop')), contextlib.redirect_stdout(io.StringIO()):
                gate.main(['--baseline-ref','a'*40,'--candidate-ref','b'*40,'--output',tmp,'--scratch-root',tmp,*flag])
                report = json.loads((Path(tmp)/'results.json').read_text())
                self.assertEqual(report['config']['quiet_host_guard'],guarded)
                self.assertEqual(report['quiet_host']['enabled'],guarded)
                self.assertIn('Quiet host:',(Path(tmp)/'summary.md').read_text())
        gate.DEADLINE = math.inf


class Instructions(unittest.TestCase):
    def test_perf_stat_output_is_parsed_exactly(self):
        self.assertEqual(gate.parse_instructions('# started on Mon\n\n151041,,instructions:u,2146321,100.00,,\n'),151041)
        for text in ('','<not counted>,,instructions:u,0,100.00,,\n','<not supported>,,instructions:u,0,0.00,,\n','12,,cycles:u,1,100.00,,\n'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                gate.parse_instructions(text)

    def test_the_probe_needs_the_counter_in_the_output(self):
        def fake(code, stderr):
            return lambda *a, **k: subprocess.CompletedProcess(a,code,'',stderr)
        self.assertTrue(gate.perf_available(fake(0,'1,,instructions:u,2,100.00,,\n')))
        self.assertFalse(gate.perf_available(fake(0,'<not supported>,,cycles,0,0,,\n')))
        self.assertFalse(gate.perf_available(fake(1,'')))

    @unittest.skipUnless(gate.perf_available(),'perf stat instructions:u is unavailable on this host')
    def test_a_real_process_is_counted_only_inside_the_window(self):
        worker = subprocess.Popen(['python3','-c','import sys\nfor n in sys.stdin:\n    sum(range(int(n)))\n    print("done",flush=True)'],
                                  stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
        def run(n):
            worker.stdin.write(f'{n}\n')
            worker.stdin.flush()
            return worker.stdout.readline().strip()
        try:
            small,_ = gate.count_instructions([worker.pid],lambda: run(1000))
            large,result = gate.count_instructions([worker.pid],lambda: run(3_000_000))
        finally:
            worker.kill()
            worker.wait()
            worker.stdin.close()
            worker.stdout.close()
        self.assertEqual(result,'done')
        self.assertGreater(large,10*small)
        self.assertGreater(large,3_000_000)

    def test_a_regression_is_an_instruction_regression_or_an_ipc_and_scaling_one(self):
        for status,instructions,kind in (('pass',{'ratio':1.5},None),('regression',{'ratio':1.12},'instructions'),
                                         ('regression',{'ratio':1.02},'ipc_or_scaling'),('regression',None,'instructions_unmeasured')):
            self.assertEqual(gate.regression_kind(status,instructions),kind)

    def test_a_4t_run_counts_the_postmaster_that_forks_the_workers(self):
        class Rev:
            def __init__(self,data):
                self.data,self.conn = data,self
            def rows(self,sql): return [['4242']]
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp)/'postmaster.pid').write_text('777\n/data\n')
            rev = Rev(Path(tmp))
            case = gate.CASES[0]
            self.assertEqual(gate.counted_pids(rev,case),[4242])
            self.assertEqual(gate.counted_pids(rev,case.at('4T')),[4242,777])

    def test_each_revision_is_counted_once_per_row_after_the_timed_blocks(self):
        with patch.object(gate,'case_instructions',side_effect=[1000,1300]):
            got = gate.measure_instructions([object(),object()],gate.CASES[0],100,math.inf)
        self.assertEqual((got['baseline_per_row'],got['candidate_per_row']),(10,13))
        self.assertAlmostEqual(got['ratio'],1.3)

    def test_the_counters_reach_the_case_result_and_a_missing_counter_is_reported_not_failed(self):
        class Rev:
            def __init__(self,name,*args): self.name,self.conn,self.timings,self.versions,self.started = name,self,{},[],False
        case = gate.CASES[0]
        timed = dict(gate.interval([(1,1.3)]*6),samples=[(1,1.3)]*6)
        for available in (True,False):
            with self.subTest(available=available), patch.object(gate,'PERF_AVAILABLE',available), \
                 patch.object(gate,'validate_case',return_value={'size_checks':None}), patch.object(gate,'calibrate',return_value={'operations':3}), \
                 patch.object(gate,'block',return_value=timed), patch.object(gate,'fixture_rows',return_value=50), \
                 patch.object(gate,'case_instructions',side_effect=[500,500]):
                result = gate.measure([Rev('a'),Rev('b')],case,math.inf)
                self.assertEqual(result['status'],'regression')
                self.assertEqual('instructions' in result,available)
                self.assertEqual(result.get('regression_kind'),'ipc_or_scaling' if available else 'instructions_unmeasured')


if __name__=='__main__':
    unittest.main()
