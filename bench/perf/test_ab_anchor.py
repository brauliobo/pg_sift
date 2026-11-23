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


if __name__=='__main__':
    unittest.main()
