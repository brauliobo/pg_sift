"""Filesystem-only launcher contracts; no PostgreSQL or socket bind."""
import contextlib
import json
import os
from pathlib import Path
import stat
import shutil
import tempfile
import unittest
from unittest.mock import MagicMock, patch
from test_perf_gate import gate


class Launcher(unittest.TestCase):
    def test_actual_socket_byte_boundaries_suffix_port_and_platform_contract(self):
        with patch.object(gate.sys,'platform','linux'):
            suffix='/.s.PGSQL.56260'
            for length in (106,107,108):
                directory=Path('/'+'x'*(length-len(os.fsencode(suffix))-1))
                if length>107:
                    with self.assertRaisesRegex(ValueError,'108.*107.*shorter'):
                        gate.validate_socket_path(directory,56260)
                else:
                    result=gate.validate_socket_path(directory,56260)
                    self.assertEqual(result['path_bytes'],length)
                    self.assertEqual(result['pathname_limit_bytes'],107)
                    self.assertTrue(result['path'].endswith('.s.PGSQL.56260'))
            directory=Path('/'+'x'*89+'é')
            self.assertEqual(len(str(directory/'.s.PGSQL.56260')),106)
            self.assertEqual(gate.validate_socket_path(directory,56260)['path_bytes'],107)
            with self.assertRaises(ValueError): gate.validate_socket_path(Path(str(directory)+'x'),56260)
            for port in (1,65535):
                self.assertTrue(gate.validate_socket_path(Path('/tmp/s'),port)['path'].endswith(str(port)))
            for port in (0,65536):
                with self.assertRaises(ValueError): gate.validate_socket_path(Path('/tmp/s'),port)
        with patch.object(gate.sys,'platform','darwin'):
            self.assertEqual(gate.validate_socket_path(Path('/tmp/s'),56260)['pathname_limit_bytes'],103)
        with patch.object(gate.sys,'platform','unsupported'):
            with self.assertRaisesRegex(ValueError,'platform'): gate.validate_socket_path(Path('/tmp/s'),56260)

    def revisions(self, root, output):
        def archive(argv,**kwargs):
            self.assertEqual(argv[:3],['git','archive','--format=tar'])
            Path(argv[argv.index('-o')+1]).write_bytes(b'')
            return ''
        with patch.object(gate,'command',side_effect=archive), patch.object(gate.tarfile,'open',return_value=MagicMock()):
            return [gate.Revision(name,'a'*40,None,root,output,'/bin/true',port)
                    for name,port in (('baseline',56260),('candidate',56261))]

    def test_two_clusters_use_unique_private_short_sockets_independent_of_long_roots_and_tmpdir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/('verbose-build-root-'*8)
            root.mkdir()
            output=Path(tmp)/'output'; output.mkdir()
            with patch.dict(os.environ,{'TMPDIR':str(root)}):
                revisions=self.revisions(root,output)
            try:
                self.assertNotEqual(revisions[0].socket,revisions[1].socket)
                for rev in revisions:
                    self.assertEqual(rev.socket.parent,Path('/tmp'))
                    self.assertEqual(stat.S_IMODE(rev.socket.stat().st_mode),0o700)
                    self.assertFalse(rev.socket.is_relative_to(root))
                    self.assertTrue(rev.data.is_relative_to(root))
                    self.assertTrue(rev.tree.is_relative_to(root))
                    self.assertTrue(rev.stage.is_relative_to(root))
                    self.assertEqual(rev.socket_path['path'],str(rev.socket/f'.s.PGSQL.{rev.port}'))
                    self.assertLessEqual(rev.socket_path['path_bytes'],107)
            finally:
                for rev in revisions: rev.stop(output)
            self.assertTrue(all(not rev.socket.exists() for rev in revisions))

    def test_socket_directory_is_0700_even_under_owner_masking_umask(self):
        old=os.umask(0o777)
        directory=None
        try:
            directory,_=gate.allocate_socket_directory(56260)
            self.assertEqual(stat.S_IMODE(directory.stat().st_mode),0o700)
        finally:
            os.umask(old)
            if directory is not None:
                directory.chmod(0o700)
                directory.rmdir()

    def bare_revision(self, root, *, log=True, pid=False):
        rev=gate.Revision.__new__(gate.Revision)
        rev.name,rev.base,rev.data='baseline',root/'baseline',root/'baseline'/'cluster'
        rev.base.mkdir(); rev.data.mkdir()
        rev.socket=Path(tempfile.mkdtemp(prefix='pgs-test-',dir='/tmp'))
        self.addCleanup(shutil.rmtree,rev.socket,ignore_errors=True)
        rev.socket_owned=True
        rev.port,rev.conn,rev.started,rev.bindir,rev.env=56260,None,False,Path('/bin'),{}
        rev.log=root/'setup.log'
        if log: (rev.base/'server.log').write_text('synthetic startup diagnostic\n')
        if pid: (rev.data/'postmaster.pid').write_text('123\n')
        return rev

    def test_failed_pg_ctl_no_pid_preserves_server_log_before_socket_and_scratch_removal(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); output=root/'out'; output.mkdir()
            rev=self.bare_revision(root)
            with patch.object(gate,'command',side_effect=RuntimeError('original pg_ctl startup error')):
                with self.assertRaisesRegex(RuntimeError,'original pg_ctl'):
                    rev.start()
            rev.stop(output)
            self.assertFalse(rev.started)
            self.assertFalse(rev.socket.exists())
            self.assertEqual((output/'baseline-server.log').read_text(),'synthetic startup diagnostic\n')
            self.assertEqual(rev.server_log_evidence['state'],'copied')

    def test_missing_startup_log_is_explicit_and_successful_stop_keeps_log(self):
        for log,pid in ((False,False),(True,True)):
            with self.subTest(log=log,pid=pid), tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp); output=root/'out'; output.mkdir()
                rev=self.bare_revision(root,log=log,pid=pid)
                rev.started=pid
                def stop(*args,**kwargs): (rev.data/'postmaster.pid').unlink()
                with patch.object(gate,'command',side_effect=stop) as command:
                    rev.stop(output)
                self.assertEqual(command.call_count,int(pid))
                self.assertFalse(rev.socket.exists())
                self.assertEqual(rev.server_log_evidence['state'],'copied' if log else 'absent')
                self.assertEqual((output/'baseline-server.log').exists(),log)

    def test_failed_stop_retains_live_cluster_socket_and_preserves_primary_error_if_log_copy_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); output=root/'out'; output.mkdir()
            rev=self.bare_revision(root,pid=True); rev.started=True
            try:
                with patch.object(gate,'command',side_effect=RuntimeError('primary stop failure')), patch.object(gate.shutil,'copyfile',side_effect=OSError('log copy failure')):
                    with self.assertRaisesRegex(RuntimeError,'primary stop failure') as caught: rev.stop(output)
                self.assertTrue(rev.started)
                self.assertTrue(rev.socket.exists())
                self.assertEqual(rev.server_log_evidence['state'],'copy_failed')
                self.assertIn('log copy failure',' '.join(caught.exception.__notes__))
            finally:
                (rev.data/'postmaster.pid').unlink()
                with patch.object(gate,'command'): rev.stop(output)

    def test_stop_then_restart_exclusively_recreates_private_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); output=root/'out'; output.mkdir()
            rev=self.bare_revision(root)
            rev.stop(output)
            self.assertFalse(rev.socket_owned)
            class Conn:
                def __init__(self,*args): pass
                def exec(self,sql): pass
                def script(self,sql): pass
                def rows(self,sql): return [['8192']] if sql=='SHOW block_size' else [['PG18','3']]
            with patch.object(gate,'command'), patch.object(gate.cell,'libpq',return_value=MagicMock()), patch.object(gate,'SQL',Conn):
                rev.start()
            self.assertTrue(rev.socket_owned)
            self.assertEqual(stat.S_IMODE(rev.socket.stat().st_mode),0o700)
            rev.conn=None
            rev.stop(output)
            rev.socket.mkdir(mode=0o700)  # A different owner occupies the released name.
            with patch.object(gate,'command') as command:
                with self.assertRaises(FileExistsError): rev.start()
            command.assert_not_called()
            rev.stop(output)
            self.assertTrue(rev.socket.exists(),'never remove a directory not reacquired by this owner')

    def test_pid_remains_authoritative_even_if_started_flag_was_false(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); output=root/'out'; output.mkdir()
            rev=self.bare_revision(root,pid=True)
            with patch.object(gate,'command',side_effect=RuntimeError('live cluster cannot stop')):
                with self.assertRaisesRegex(RuntimeError,'live cluster'): rev.stop(output)
            self.assertTrue(rev.started)
            self.assertTrue(rev.socket.exists())
            (rev.data/'postmaster.pid').unlink()
            rev.stop(output)

    def test_actual_main_preserves_failed_start_log_or_absence_and_original_error_before_cleanup(self):
        for log,copy_failure in ((True,False),(False,False),(True,True)):
            with self.subTest(log=log,copy_failure=copy_failure), tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp); output=root/'out'; scratch=root/('long-build-root-'*8)
                made=[]
                def command(argv,**kwargs):
                    argv=list(map(str,argv))
                    if argv[:2]==['git','archive']:
                        Path(argv[argv.index('-o')+1]).write_bytes(b'')
                        return ''
                    if argv[0]=='nice': raise RuntimeError('original pg_ctl startup failure')
                    return 'PostgreSQL 18.6'
                def setup(rev,rows):
                    made.append(rev)
                    # Both actual unique byte-validated endpoints exist BEFORE any build/start.
                    snapshot=json.loads((output/'results.json').read_text())
                    self.assertEqual(set(snapshot['unix_socket_paths']),{'baseline','candidate'})
                    self.assertTrue(all(v['path_bytes']<=v['pathname_limit_bytes'] for v in snapshot['unix_socket_paths'].values()))
                    rev.data.mkdir()
                    rev.env,rev.bindir={},Path('/bin')
                    if log: (rev.base/'server.log').write_text('synthetic failed-start evidence\n')
                    rev.start()
                args=['--baseline-ref','a'*40,'--candidate-ref','a'*40,'--output',str(output),
                      '--scratch-root',str(scratch),'--pg-config','/bin/true']
                copy_context=patch.object(gate.shutil,'copyfile',side_effect=OSError('cannot preserve log')) if copy_failure else contextlib.nullcontext()
                with patch.object(gate,'command',side_effect=command), patch.object(gate,'exact_sha'), patch.object(gate.tarfile,'open',return_value=MagicMock()), patch.object(gate.Revision,'setup',autospec=True,side_effect=setup), patch.object(gate.os,'getloadavg',return_value=(0,0,0)), copy_context, contextlib.redirect_stdout(__import__('io').StringIO()):
                    code=gate.main(args)
                gate.DEADLINE=__import__('math').inf
                report=json.loads((output/'results.json').read_text())
                self.assertEqual(code,3)
                self.assertIn('original pg_ctl startup failure',report['error'])
                self.assertEqual(report['cases'],[])
                self.assertEqual(len(made),1)
                expected='copy_failed' if copy_failure else 'copied' if log else 'absent'
                self.assertEqual(report['server_logs']['baseline']['state'],expected)
                self.assertEqual(report['server_logs']['candidate']['state'],'absent')
                self.assertFalse(made[0].socket.exists())
                if copy_failure:
                    self.assertTrue(made[0].base.exists(),'failed evidence copy must retain original scratch')
                    self.assertIn('cannot preserve log',report['cleanup_errors'][0])
                else:
                    self.assertFalse(made[0].base.exists())
                    self.assertEqual((output/'baseline-server.log').exists(),log)

    def test_oversized_generated_socket_is_rejected_before_any_build_or_start_and_owned_dir_cleaned(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); output=root/'out'; output.mkdir()
            long_socket=root/('x'*100); long_socket.mkdir()
            with patch.object(gate.tempfile,'mkdtemp',return_value=str(long_socket)), patch.object(gate,'command') as command:
                with self.assertRaisesRegex(ValueError,'socket path'):
                    gate.allocate_socket_directory(56260)
            command.assert_not_called()
            self.assertFalse(long_socket.exists())


if __name__=='__main__': unittest.main()
