"""Layer 3 tests: the quiet-host guard, instruction counters and the pinned release anchor. python3 -m unittest discover -s bench/perf -v."""
import contextlib
import io
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../steps'))
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)
from test_perf_gate import gate  # noqa: E402


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


SHA = 'f' * 40
META = {'anchor_ref':'v5.1.0','anchor_sha':SHA,'postgresql':'PostgreSQL 18.7','rows':'8192','tier':'pr','recorded':'2026-11-23'}
ROWS = {('agg_group','1T'):dict(instr_per_row=100.0,ms_ratio_vs_native=0.5,blocks=40.0),
        ('agg_group','4T'):dict(instr_per_row=120.0,ms_ratio_vs_native=0.6,blocks=40.0),
        ('bm25_top10_common','1T'):dict(instr_per_row=None,ms_ratio_vs_native=None,blocks=12.0)}


def manifest_text(meta=META, rows=ROWS):
    return gate.format_anchor_manifest(meta,rows)


class AnchorManifestFile(unittest.TestCase):
    def test_a_manifest_round_trips_with_its_metadata(self):
        manifest = gate.parse_anchor_manifest(manifest_text())
        self.assertEqual(manifest.meta['rows'],8192)
        self.assertEqual(manifest.meta['anchor_ref'],'v5.1.0')
        self.assertEqual(manifest.rows[('agg_group','4T')],ROWS[('agg_group','4T')])
        self.assertIsNone(manifest.rows[('bm25_top10_common','1T')]['ms_ratio_vs_native'])
        self.assertEqual(manifest_text().splitlines()[0],'# schema: perf-anchor-manifest v1')

    def test_every_malformed_manifest_names_its_line_and_is_refused(self):
        good = manifest_text().splitlines()
        header = good.index('\t'.join(gate.ANCHOR_COLUMNS))
        def with_row(row): return '\n'.join(good[:header+1]+[row])+'\n'
        row = '\t'.join
        cases = {
            'no schema': '\n'.join(good[1:])+'\n',
            'other schema': '\n'.join(['# schema: perf-anchor-manifest v0']+good[1:])+'\n',
            'missing metadata': '\n'.join(good[:3]+good[header:])+'\n',
            'unknown metadata': '\n'.join(good[:header]+['# colour: red']+good[header:])+'\n',
            'repeated metadata': '\n'.join(good[:header]+['# rows: 8192']+good[header:])+'\n',
            'short sha': manifest_text(dict(META,anchor_sha='abc')),
            'bad tier': manifest_text(dict(META,tier='weekly')),
            'bad rows': manifest_text(dict(META,rows='0')),
            'wrong header': '\n'.join(good[:header]+['case_id\tmode']+good[header+1:])+'\n',
            'no case': '\n'.join(good[:header+1])+'\n',
            'field count': with_row('agg_group\t1T\tv5.1.0\t1'),
            'unknown case': with_row(row(['nope','1T','v5.1.0','1','1','1'])),
            'unknown mode': with_row(row(['agg_group','8T','v5.1.0','1','1','1'])),
            'case without that mode': with_row(row(['bm25_top10_common','4T','v5.1.0','1','1','1'])),
            'mutation case': with_row(row(['storage_update','1T','v5.1.0','1','1','1'])),
            'other ref': with_row(row(['agg_group','1T','v6.0.0','1','1','1'])),
            'not a number': with_row(row(['agg_group','1T','v5.1.0','many','1','1'])),
            'zero': with_row(row(['agg_group','1T','v5.1.0','0','1','1'])),
            'infinite': with_row(row(['agg_group','1T','v5.1.0','inf','1','1'])),
            'nothing recorded': with_row(row(['agg_group','1T','v5.1.0','-','-','-'])),
            'duplicate': '\n'.join(good+[good[header+1]])+'\n',
        }
        for label,text in cases.items():
            with self.subTest(label), self.assertRaisesRegex(ValueError,'anchor manifest'):
                gate.parse_anchor_manifest(text)

    def test_a_manifest_that_cannot_be_read_names_the_flag_and_the_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError,'--anchor-tsv .*absent'):
                gate.load_anchor_manifest(Path(tmp)/'absent')


class WarmBlocks(unittest.TestCase):
    def test_blocks_come_from_an_execution_after_the_first_one_of_the_session(self):
        events = []
        class Conn:
            def rows(self, sql):
                events.append(sql.split(' ',1)[0])
                return [[json.dumps([{'Plan':{'Shared Hit Blocks':6,'Shared Read Blocks':2}}])]]
        case = gate.CASES[0]
        with patch.object(gate,'prepare',side_effect=lambda *a: events.append('prepare')), \
             patch.object(gate,'elapsed',side_effect=lambda *a: events.append('warm') or 1.0):
            self.assertEqual(gate.warm_blocks(Conn(),case,math.inf),8)
        self.assertEqual(events,['prepare','warm','EXPLAIN'])


class CheckedInManifest(unittest.TestCase):
    def test_the_seeded_manifest_covers_every_read_case_in_the_modes_it_runs(self):
        path = Path(gate.__file__).resolve().parents[1]/'baselines/anchor-manifest.tsv'
        manifest = gate.load_anchor_manifest(path)
        expected = {(c.name.split('@')[0],c.mode) for c in gate.expand([c for c in gate.CASES if not c.nightly]) if c.lifecycle=='read'}
        self.assertEqual(set(manifest.rows),expected)
        self.assertTrue(all(row['instr_per_row'] and row['blocks'] for row in manifest.rows.values()))
        self.assertEqual(manifest.meta['rows'],8192)


class AnchorDrift(unittest.TestCase):
    def setUp(self):
        self.manifest = gate.parse_anchor_manifest(manifest_text())
        self.case = next(c for c in gate.CASES if c.name=='agg_group')

    def drift(self, instr=100.0, wall=0.5, blocks=40.0, rows=8192, case=None):
        profile = dict(instr_per_row=instr,ms_ratio_vs_native=wall,blocks=blocks)
        return gate.anchor_drift(self.manifest,case or self.case,profile,rows)

    def test_inside_every_limit_passes(self):
        got = self.drift(109.9,0.5*1.149,40*1.10+2)
        self.assertEqual(got['status'],'pass')
        self.assertEqual({c['status'] for c in got['checks'].values()},{'pass'})

    def test_each_signal_fails_alone_past_its_limit(self):
        for label,kwargs,failed in (('instructions',dict(instr=110.5),'instructions'),('wall',dict(wall=0.5*1.16),'wall_vs_native'),
                                    ('blocks',dict(blocks=47.0),'blocks')):
            with self.subTest(label):
                got = self.drift(**kwargs)
                self.assertEqual(got['status'],'regression')
                self.assertEqual([k for k,c in got['checks'].items() if c['status']=='regression'],[failed])

    def test_modes_are_recorded_apart(self):
        self.assertEqual(self.drift(125,case=self.case.at('4T'))['status'],'pass')
        self.assertEqual(self.drift(125)['status'],'regression')

    def test_blocks_are_compared_only_at_the_recorded_row_count(self):
        got = self.drift(blocks=4000,rows=32768)
        self.assertEqual(got['status'],'pass')
        self.assertEqual(got['checks']['blocks']['status'],'rows_differ')

    def test_what_cannot_be_measured_or_was_not_recorded_does_not_fail(self):
        self.assertEqual(self.drift(instr=None,wall=None,blocks=None)['status'],'unmeasured')
        self.assertEqual(self.drift(case=next(c for c in gate.CASES if c.name=='agg_distinct'))['status'],'unrecorded')
        bm25 = next(c for c in gate.CASES if c.name=='bm25_top10_common')
        got = gate.anchor_drift(self.manifest,bm25,dict(instr_per_row=5.0,ms_ratio_vs_native=None,blocks=12.0),8192)
        self.assertEqual((got['status'],got['checks']['instructions']['status']),('pass','unrecorded'))

    def test_ten_commits_of_two_percent_pass_every_pairing_and_fail_the_anchor(self):
        parent = 100.0
        for step in range(10):
            candidate = parent*1.02
            self.assertEqual(gate.verdict(gate.interval([(parent,candidate)]*6)),'pass')
            parent = candidate
        self.assertEqual(self.drift(instr=parent)['checks']['instructions']['status'],'regression')

    def test_the_drift_decides_the_verdict_of_a_gating_case_only(self):
        def report(name,drift):
            return dict(config={'verify_control':False},status='pass',complete=True,size_complete=True,required_size_states=[],
                        size_checks=[dict(state='settled_build',status='pass')],
                        cases=[dict(name=name,timing_status='pass',vs_anchor_manifest=dict(status=drift))])
        for name,drift,expected in (('agg_group','regression','regression'),('agg_group@4T','regression','regression'),
                                    ('agg_group','pass','pass'),('agg_group','unmeasured','pass'),('agg_group','unrecorded','pass'),
                                    ('storage_point_lookup','regression','pass')):
            with self.subTest(name=name,drift=drift):
                value = report(name,drift)
                gate.reduce_report(value)
                self.assertEqual(value['status'],expected)

    def test_the_native_ratio_is_the_median_of_paired_batches(self):
        times = {self.case.sql:[4.0,2.0,3.0,9.0,3.0],self.case.oracle:[1.0]*5}
        def timed(conn, case, operations, deadline):
            return times[case.sql].pop(0) if operations>1 and case.sql==self.case.sql else (1.0 if case.sql==self.case.oracle and operations>1 else 0.1)
        with patch.object(gate,'prepare'), patch.object(gate,'elapsed',side_effect=timed):
            self.assertEqual(gate.native_wall_ratio(object(),self.case,5,math.inf),3.0)


class AnchorCommandLine(unittest.TestCase):
    def run_main(self, *argv):
        out,err = io.StringIO(),io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = gate.main(list(argv))
        return code,out.getvalue(),err.getvalue()

    def test_check_anchor_tsv_validates_without_any_other_argument(self):
        with tempfile.TemporaryDirectory() as tmp:
            good,bad = Path(tmp)/'good.tsv',Path(tmp)/'bad.tsv'
            good.write_text(manifest_text())
            bad.write_text(manifest_text().replace('v5.1.0\t100','v9\t100',1))
            code,out,_ = self.run_main('--check-anchor-tsv',str(good))
            self.assertEqual(code,0)
            self.assertIn('3 cases of v5.1.0',out)
            code,_,err = self.run_main('--check-anchor-tsv',str(bad))
            self.assertEqual(code,3)
            self.assertIn('anchor_ref',err)

    def test_a_bad_manifest_stops_a_run_before_any_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp)/'bad.tsv'
            bad.write_text('nothing\n')
            with patch.object(gate,'exact_sha',side_effect=AssertionError('setup began')):
                code,_,err = self.run_main('--baseline-ref','a'*40,'--candidate-ref','b'*40,'--output',tmp,'--scratch-root',tmp,
                                           '--anchor-tsv',str(bad))
        self.assertEqual(code,3)
        self.assertIn('--anchor-tsv',err)

    def test_the_drift_reaches_the_case_result_of_a_run_with_a_manifest(self):
        class Rev:
            def __init__(self,name,*args): self.name,self.conn,self.timings,self.versions,self.started = name,self,{},[],False
        case = next(c for c in gate.CASES if c.name=='agg_group')
        timed = dict(gate.interval([(1,1.0)]*6),samples=[(1,1.0)]*6)
        manifest = gate.parse_anchor_manifest(manifest_text())
        with patch.object(gate,'PROFILE',True), patch.object(gate,'PERF_AVAILABLE',True), patch.object(gate,'ANCHOR_MANIFEST',manifest), \
             patch.object(gate,'validate_case',return_value={'size_checks':None}), \
             patch.object(gate,'calibrate',return_value={'operations':3}), patch.object(gate,'block',return_value=timed), \
             patch.object(gate,'fixture_rows',return_value=8192), patch.object(gate,'case_instructions',side_effect=[800000,900000]), \
             patch.object(gate,'native_wall_ratio',return_value=0.5), patch.object(gate,'warm_blocks',return_value=50):
            result = gate.measure([Rev('a'),Rev('b')],case,math.inf)
        self.assertEqual(result['profile'],dict(instr_per_row=900000/8192,ms_ratio_vs_native=0.5,blocks=50))
        self.assertEqual(result['vs_anchor_manifest']['checks']['blocks']['status'],'regression')
        self.assertEqual(result['vs_anchor_manifest']['status'],'regression')


class AnchorStage(unittest.TestCase):
    SHA = 'c' * 40

    def fake_command(self, argv, **kwargs):
        return self.SHA if argv[0]=='git' else 'PostgreSQL 18.7 on x86_64'

    def stage(self, cache, build, ref='v5.1.0'):
        with patch.object(gate,'command',side_effect=self.fake_command), patch.object(gate,'archive_tree',side_effect=lambda sha,dest,*a: dest.mkdir()), \
             patch.object(gate,'build_release',side_effect=build):
            return gate.stage_anchor(ref,cache,'/usr/bin/pg_config',1,None)

    def library(self, tree, stage, *args):
        (stage/'usr/lib/postgresql/18/lib').mkdir(parents=True)
        (stage/'usr/lib/postgresql/18/lib/pg_sift.so').write_text('lib')

    def test_the_anchor_is_built_once_per_commit_and_major_and_then_reused(self):
        built = []
        def build(tree, stage, *args):
            built.append(stage)
            self.library(tree,stage)
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp)/'anchor'
            first = self.stage(cache,build)
            second = self.stage(cache,build)
            self.assertEqual(first,(self.SHA,cache/f'{self.SHA[:12]}-pg18',False))
            self.assertEqual(second,(self.SHA,first[1],True))
            self.assertEqual(len(built),1)
            self.assertEqual((first[1]/'.fixture').read_text(),f'{self.SHA} pg18\n')
            self.assertEqual(sorted(p.name for p in cache.iterdir()),[first[1].name])

    def test_a_stage_that_scripts_build_v510_fixture_wrote_is_reused(self):
        def build(*args): raise AssertionError('rebuilt a stamped stage')
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)/f'{self.SHA[:12]}-pg18'
            stage.mkdir()
            (stage/'.fixture').write_text(f'{self.SHA} pg18\n')
            self.assertEqual(self.stage(Path(tmp),build),(self.SHA,stage,True))

    def test_a_stale_stage_is_rebuilt_and_a_build_without_a_library_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)/f'{self.SHA[:12]}-pg18'
            stage.mkdir()
            (stage/'.fixture').write_text(f'{"d"*40} pg18\n')
            self.assertEqual(self.stage(Path(tmp),self.library)[2],False)
            self.assertEqual((stage/'.fixture').read_text(),f'{self.SHA} pg18\n')
        with tempfile.TemporaryDirectory() as tmp, self.assertRaisesRegex(RuntimeError,'no pg_sift library'):
            self.stage(Path(tmp),lambda *a: a[1].mkdir())
        self.assertFalse(list(Path(tmp).glob('*pg18')))

    def test_a_prebuilt_revision_neither_archives_nor_builds(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(gate,'archive_tree',side_effect=AssertionError('archived')), \
             patch.object(gate,'allocate_socket_directory',return_value=(Path(tmp)/'sock',{})):
            rev = gate.Revision('anchor',self.SHA,None,Path(tmp),Path(tmp),'/usr/bin/pg_config',56284,1,prebuilt=Path(tmp)/'stage',anchor=True)
            self.assertEqual((rev.stage,rev.anchor),(Path(tmp)/'stage',True))

    def test_an_unknown_sift_setting_of_an_older_release_is_skipped_only_when_the_release_is_listed(self):
        sent = []
        for gucs,expected in ((None,['SET sift.agg_preagg=off','SET sift.enable_agg=on','SET jit=off']),
                              ({'sift.enable_agg'},['SET sift.enable_agg=on','SET jit=off'])):
            conn = object.__new__(gate.SQL)
            conn.gucs = gucs
            conn.exec = sent.append
            sent.clear()
            conn.script('SET sift.agg_preagg=off; set sift.enable_agg=on; SET jit=off')
            self.assertEqual([s.strip().replace('set','SET') for s in sent],expected)


class AnchorPairs(unittest.TestCase):
    class Rev:
        def __init__(self, name, capabilities=()):
            self.name,self.conn,self.capabilities = name,self,set(capabilities)

    def report(self):
        return dict(cases=[],vs_anchor=[],anchor_profile={},config={'verify_control':False},status='pass',complete=True,size_complete=True,
                    required_size_states=[],size_checks=[dict(state='settled_build',status='pass')])

    def pair(self, case, ratio, record=False, anchor=None, **patches):
        report = self.report()
        timed = dict(gate.interval([(1,ratio)]*6),samples=[(1,ratio)]*6)
        with patch.object(gate,'validate_case',return_value={'size_checks':None}), patch.object(gate,'calibrate',return_value={'operations':3}), \
             patch.object(gate,'block',return_value=timed), patch.object(gate,'PERF_AVAILABLE',True), patch.object(gate,'fixture_rows',return_value=100), \
             patch.object(gate,'case_instructions',side_effect=[2000,2600]), patch.object(gate,'native_wall_ratio',return_value=0.25), \
             patch.object(gate,'warm_blocks',return_value=10):
            gate.measure_against_anchor(anchor or self.Rev('anchor',('table',)),self.Rev('candidate'),case,math.inf,report,None,record)
        return report

    def case(self, name='agg_group'):
        return next(c for c in gate.CASES if c.name==name)

    def test_a_candidate_slower_than_the_anchor_is_a_regression_the_parent_comparison_cannot_see(self):
        report = self.pair(self.case(),1.25)
        self.assertEqual(report['vs_anchor'][0]['status'],'regression')
        self.assertEqual(report['vs_anchor'][0]['regression_kind'],'instructions')
        gate.reduce_report(report)
        self.assertEqual(report['status'],'regression')
        passing = self.pair(self.case(),1.0)
        gate.reduce_report(passing)
        self.assertEqual(passing['status'],'pass')

    def test_a_candidate_on_par_with_the_anchor_passes_and_the_ratio_is_reported(self):
        report = self.pair(self.case(),1.0)
        self.assertEqual(report['vs_anchor'][0]['status'],'pass')
        self.assertEqual(report['vs_anchor'][0]['instructions']['ratio'],1.3)
        self.assertEqual(report['anchor_profile'],{})

    def test_a_case_the_anchor_cannot_run_is_reported_not_timed(self):
        report = self.pair(self.case('agg_table_group'),1.25,anchor=self.Rev('anchor'))
        entry = report['vs_anchor'][0]
        self.assertEqual((entry['status'],entry['timing_status']),('skipped','skipped'))
        self.assertIn('no table access method',entry['reason'])
        gate.reduce_report(report)
        self.assertEqual(report['status'],'pass')

    def test_an_anchor_that_answers_differently_is_skipped_not_an_error(self):
        report = self.report()
        with patch.object(gate,'validate_case',side_effect=gate.CorrectnessMismatch('agg_group baseline/candidate: correctness differs')):
            gate.measure_against_anchor(self.Rev('anchor',('table',)),self.Rev('candidate'),self.case(),math.inf,report,None,False)
        self.assertEqual(report['vs_anchor'][0]['status'],'skipped')
        self.assertIn('correctness differs',report['vs_anchor'][0]['reason'])

    def test_recording_profiles_the_anchor_side_of_the_pair(self):
        report = self.pair(self.case('agg_group').at('4T'),1.0,record=True)
        self.assertEqual(report['anchor_profile'],{'agg_group@4T':dict(instr_per_row=20.0,ms_ratio_vs_native=0.25,blocks=10)})

    def test_the_recorded_manifest_is_the_one_a_later_run_reads(self):
        report = self.pair(self.case(),1.0,record=True)
        report.update(anchor=dict(sha=SHA,rows=100),postgresql='PostgreSQL 18.7',date='2026-11-23T10:00:00+00:00',quiet_host=dict(enabled=True,blocks=1,noisy_blocks=0))
        report['anchor_profile'].update(self.pair(self.case('agg_group').at('4T'),1.0,record=True)['anchor_profile'])
        with tempfile.TemporaryDirectory() as tmp:
            args = gate.parser().parse_args(['--baseline-ref','a','--candidate-ref','b','--output','o','--scratch-root','s','--anchor-ref','v5.1.0',
                                             '--anchor-record',str(Path(tmp)/'m.tsv')])
            gate.anchor_record(report,args)
            manifest = gate.load_anchor_manifest(Path(tmp)/'m.tsv')
        self.assertEqual(manifest.meta['anchor_ref'],'v5.1.0')
        self.assertEqual(set(manifest.rows),{('agg_group','1T'),('agg_group','4T')})
        self.assertEqual(manifest.rows[('agg_group','1T')]['blocks'],10)

    def test_a_wall_ratio_is_recorded_only_from_a_quiet_host(self):
        profile = {'agg_group':dict(instr_per_row=20.0,ms_ratio_vs_native=0.25,blocks=10)}
        for host,recorded in ((dict(enabled=True,blocks=12,noisy_blocks=0),0.25),(dict(enabled=True,blocks=12,noisy_blocks=1),None),
                              (dict(enabled=False,blocks=0,noisy_blocks=0),None),(dict(enabled=True,blocks=0,noisy_blocks=0),None)):
            with self.subTest(host=host), tempfile.TemporaryDirectory() as tmp:
                report = dict(anchor_profile=profile,quiet_host=host,anchor=dict(sha=SHA,rows=100),postgresql='PostgreSQL 18.7',date='2026-11-23T10:00:00+00:00')
                args = gate.parser().parse_args(['--baseline-ref','a','--candidate-ref','b','--output','o','--scratch-root','s','--anchor-ref','v5.1.0',
                                                 '--anchor-record',str(Path(tmp)/'m.tsv')])
                gate.anchor_record(report,args)
                row = gate.load_anchor_manifest(Path(tmp)/'m.tsv').rows[('agg_group','1T')]
                self.assertEqual((row['ms_ratio_vs_native'],row['instr_per_row'],row['blocks']),(recorded,20.0,10))

    def test_the_anchor_fixture_must_hold_the_candidate_data(self):
        class Conn:
            def __init__(self,digest): self.conn,self.digest = self,digest
            def rows(self,sql): return [self.digest]
        self.assertEqual(gate.check_anchor_fixture(Conn(['8192','x']),Conn(['8192','x'])),8192)
        with self.assertRaisesRegex(gate.CorrectnessMismatch,'logical fixture'):
            gate.check_anchor_fixture(Conn(['8192','x']),Conn(['8192','y']))

    def test_the_anchor_flags(self):
        base = ['--baseline-ref','a','--candidate-ref','b','--output','o','--scratch-root','s']
        parsed = gate.parser().parse_args(base)
        self.assertEqual((parsed.anchor_ref,parsed.anchor_tsv,parsed.anchor_record,parsed.build_jobs),(None,None,None,2))
        parsed = gate.parser().parse_args(base+['--anchor-ref','v5.1.0','--anchor-tsv','m.tsv','--build-jobs','1'])
        self.assertEqual((parsed.anchor_ref,str(parsed.anchor_tsv),parsed.build_jobs),('v5.1.0','m.tsv',1))
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            gate.main(base+['--anchor-record',str(Path(tmp)/'m.tsv')])


class DryRun(unittest.TestCase):
    def plan(self, *extra, manifest=None):
        argv = ['--baseline-ref','a'*40,'--candidate-ref','b'*40,'--output','o','--scratch-root','s','--dry-run',*extra]
        out = io.StringIO()
        with patch.object(gate,'perf_available',return_value=True), \
             patch.object(gate,'exact_sha',side_effect=AssertionError('a dry run touches no revision')), contextlib.redirect_stdout(out):
            self.assertEqual(gate.main(argv),0)
        return json.loads(out.getvalue())

    def test_the_schedule_names_every_run_its_profile_and_the_alternating_order(self):
        plan = self.plan('--groups','agg','--allow-loaded')
        runs = {r['name']:r for r in plan['runs']}
        self.assertEqual(runs['agg_group@4T']['settings'],gate.SETTINGS_4T)
        self.assertEqual(runs['agg_group']['settings'],gate.SETTINGS_1T)
        self.assertNotIn('storage_update@4T',runs)
        self.assertEqual(plan['pair_order'],['AB','BA']*3)
        self.assertEqual(plan['quiet_host']['enabled'],False)
        self.assertTrue(plan['perf_instructions'])
        self.assertTrue(all(not r['paired_with_anchor'] for r in plan['runs']))

    def test_a_real_gate_plans_the_quiet_host_guard_and_a_pinned_anchor_pairs_the_read_cases(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'m.tsv'
            path.write_text(manifest_text())
            plan = self.plan('--groups','agg,storage','--anchor-ref','HEAD','--anchor-tsv',str(path),'--anchor-cache',tmp)
        self.assertTrue(plan['quiet_host']['enabled'])
        self.assertEqual(plan['anchor']['commit'],subprocess.run(['git','rev-parse','HEAD'],cwd=gate.ROOT,capture_output=True,text=True).stdout.strip())
        self.assertEqual(plan['anchor_manifest']['cases'],3)
        runs = {r['name']:r for r in plan['runs']}
        self.assertTrue(runs['agg_group@4T']['paired_with_anchor'])
        self.assertFalse(runs['storage_update']['paired_with_anchor'])
        self.assertTrue(runs['agg_group']['drift_checked'])
        self.assertFalse(runs['agg_distinct']['drift_checked'])
        self.assertEqual(runs['agg_table_group']['needs'],'table')

    def test_a_dry_run_leaves_no_state_for_the_next_run(self):
        self.plan('--anchor-ref','HEAD')
        self.assertFalse(gate.QUIET.enabled)
        self.assertIsNone(gate.ANCHOR_MANIFEST)
        self.assertFalse(gate.PERF_AVAILABLE)

    def test_the_rounds_alternate_who_goes_first(self):
        self.assertEqual([gate.pair_order(n) for n in range(4)],[[0,1],[1,0],[0,1],[1,0]])


if __name__=='__main__':
    unittest.main()
