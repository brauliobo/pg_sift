# A revision under test: its build, cluster, start, stop and logs, and the fixtures it loads.
# Both revisions are built from exact commits and run side by side.

class Revision:
    def __init__(self, name, sha, tree, scratch, output, pg_config, port):
        self.name, self.sha = name, sha
        self.base = scratch / name
        self.base.mkdir()
        self.tree = self.base / 'source'
        self.data = self.base / 'cluster'
        self.stage = self.base / 'stage'
        self.port = port
        self.conn = None
        self.started = False
        self.pg_config = pg_config
        self.log = output / f'{name}-setup.log'
        source = Path(tree).resolve() if tree else ROOT
        if tree:
            require_equal(command(['git','rev-parse','HEAD'],cwd=source), sha, name+' tree HEAD')
            if command(['git','status','--porcelain','--untracked-files=no'],cwd=source):
                raise ValueError(f'{name} tree has tracked modifications')
        # git archive ignores untracked build products and avoids inherited object files.
        archive = self.base / 'source.tar'
        command(['git','archive','--format=tar','-o',archive,sha],cwd=source,log=self.log)
        self.tree.mkdir()
        with tarfile.open(archive) as tar:
            tar.extractall(self.tree, filter='data')
        archive.unlink()
        self.socket,self.socket_path = allocate_socket_directory(port)
        self.socket_owned = True
        self.server_log_evidence = None

    def setup(self, rows=8192):
        self.timings = {}
        phase = time.monotonic()
        env = os.environ.copy()
        for key in ('MAKEFLAGS','MFLAGS','CFLAGS','CPPFLAGS','LDFLAGS','PGOPTIONS','SIFT_INITDB_TEMPLATE',
                    'SANITIZE','SIFT_HARDEN','SIFT_LTO','VULKAN','SIFT_TESTING','LD_PRELOAD'):
            env.pop(key, None)
        env['PATH'] = str(Path(self.pg_config).parent)+os.pathsep+env['PATH']
        flags = ['USE_PGXS=1','SIFT_TESTING=','SIFT_LTO=','SANITIZE=','VULKAN=',f'PG_CONFIG={self.pg_config}']
        command(['nice','-n','19','make','-j2',*flags],cwd=self.tree,env=env,log=self.log)
        command(['nice','-n','19','make','-j2',*flags,f'DESTDIR={self.stage}','install'],cwd=self.tree,env=env,log=self.log)
        self.timings['build_install'] = time.monotonic()-phase
        phase = time.monotonic()
        bindir = Path(command([self.pg_config,'--bindir']))
        sharedir = self.stage / command([self.pg_config,'--sharedir']).lstrip('/')
        pkglib = self.stage / command([self.pg_config,'--pkglibdir']).lstrip('/')
        self.bindir = bindir
        command([bindir/'initdb','--no-sync','--wal-segsize=1','-D',self.data,'-A','trust','-U','postgres','--no-locale','-E','UTF8'],env=env,log=self.log)
        # Independent logical load; no physical index is copied between revisions.
        config = f"""
port={self.port}
listen_addresses=''
unix_socket_directories='{self.socket}'
extension_control_path='{sharedir}:$system'
dynamic_library_path='{pkglib}:$libdir'
shared_buffers='64MB'
max_wal_size='48MB'
min_wal_size='32MB'
fsync=off
full_page_writes=off
autovacuum=off
max_parallel_workers_per_gather=0
max_parallel_maintenance_workers=0
jit=off
"""
        with (self.data/'postgresql.conf').open('a',encoding=sys.getfilesystemencoding(),errors='surrogateescape') as f:
            f.write(config)
        self.env = env
        self.timings['cluster_init_start'] = time.monotonic()-phase
        self.start(fixture_rows=rows)
        return config

    def start(self, *, fixture_rows=None):
        # The restart path launches the existing data directory without loading fixtures.
        phase=time.monotonic()
        if not self.socket_owned:
            # Exclusive recreation after the cost probe; never adopt an existing path.
            self.socket.mkdir(mode=0o700)
            self.socket_owned = True
            self.socket.chmod(0o700)
        self.started = True
        command(['nice','-n','19',self.bindir/'pg_ctl','-D',self.data,'-l',self.base/'server.log','-w','start'],env=self.env,log=self.log)
        if fixture_rows is not None:
            self.timings['cluster_init_start']+=time.monotonic()-phase
            phase=time.monotonic()
            command([self.bindir/'psql','-X','-v','ON_ERROR_STOP=1','-h',self.socket,'-p',self.port,'-U','postgres','-d','postgres',
                     '-v',f'rows={fixture_rows}','-f',ROOT/'bench/perf/fixture.sql'],env=self.env,log=self.log)
            self.timings['fixture_load']=time.monotonic()-phase
        lib = cell.libpq()
        lib.PQgetisnull.restype = ctypes.c_int
        lib.PQgetisnull.argtypes = [ctypes.c_void_p,ctypes.c_int,ctypes.c_int]
        self.conn = SQL(lib, f'host={self.socket} port={self.port} user=postgres dbname=postgres application_name=perf-gate')
        self.conn.exec("LOAD 'pg_sift'")
        self.conn.script(SETTINGS)
        require_equal(self.conn.rows('SHOW block_size'),[['8192']],'PostgreSQL page size')
        self.versions = self.conn.rows("SELECT version(),extversion FROM pg_extension WHERE extname='pg_sift'")

    def stop(self, output):
        try:
            if self.conn:
                self.conn.close()
                self.conn = None
            pid=self.data/'postmaster.pid'
            self.started = pid.exists()
            if self.started:
                command([self.bindir/'pg_ctl','-D',self.data,'-m','fast','-w','stop'],log=self.log,bounded=False)
                if pid.exists():
                    raise RuntimeError('postmaster.pid remains after stop; preserving cluster and socket directory')
                self.started = False
        finally:
            primary=sys.exc_info()[1]
            source,destination=self.base/'server.log',output/f'{self.name}-server.log'
            self.server_log_evidence=dict(source=str(source),artifact=str(destination),state='absent')
            try:
                # Failed startup can produce a log without ever leaving a PID.
                try:
                    source.stat()
                except FileNotFoundError:
                    pass
                else:
                    shutil.copyfile(source,destination)
                    self.server_log_evidence['state']='copied'
            except Exception as exc:
                self.server_log_evidence.update(state='copy_failed',error=f'{type(exc).__name__}: {exc}')
                if primary is None:
                    raise
                primary.add_note('server log preservation failed: '+str(exc))
            finally:
                if self.socket_owned and not self.started and not (self.data/'postmaster.pid').exists():
                    shutil.rmtree(self.socket)
                    self.socket_owned = False



def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def finalize_fixtures(revisions, deadline, sink):
    """Drain setup checkpoints on both clusters before any fixture/read probes."""
    start = time.monotonic()
    sink.update(started_at=utc_now(),revisions={})
    try:
        for rev in revisions:
            if time.monotonic()>=deadline:
                raise BudgetExpired('global runtime budget exhausted before fixture checkpoint')
            phase = time.monotonic()
            barrier = dict(started_at=utc_now(),complete=False)
            sink['revisions'][rev.name] = barrier
            try:
                rev.conn.exec('CHECKPOINT')
                if time.monotonic()>=deadline:
                    raise BudgetExpired('global runtime budget exhausted after fixture checkpoint')
                barrier['complete'] = True
            finally:
                barrier.update(finished_at=utc_now(),runtime_seconds=time.monotonic()-phase)
    finally:
        sink.update(finished_at=utc_now(),runtime_seconds=time.monotonic()-start)


def budget_context(total, workflow_start, reserve, *, now=None):
    if not math.isfinite(total) or total<=0 or not math.isfinite(reserve) or reserve<0:
        raise ValueError('budget must be positive and reserve nonnegative, both finite')
    if workflow_start is not None and (not math.isfinite(workflow_start) or workflow_start<=0):
        raise ValueError('workflow start must be a positive finite Unix timestamp')
    now = time.time() if now is None else now
    if workflow_start is not None and workflow_start>now:
        raise ValueError('workflow start is in the future')
    prior = now-workflow_start if workflow_start is not None else 0
    return dict(total_budget_seconds=total,workflow_start_seconds=workflow_start,
                prior_elapsed_seconds=prior,cleanup_report_reserve_seconds=reserve,
                measurement_budget_seconds=max(0,total-prior-reserve))
