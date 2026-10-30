# Commands, the SQL connection and the sockets of the clusters a run starts.
# command() is bounded by the deadline; exact_sha() names a commit.

class SQL(cell.Connection):
    """Reuse cell's libpq ABI/connection; copy all rows only for untimed checks."""
    def rows(self, sql):
        if time.monotonic()>=DEADLINE:
            raise BudgetExpired('global runtime budget exhausted before SQL validation')
        lib = self.lib
        result = lib.PQexec(self.handle, sql.encode())
        if not result:
            raise RuntimeError('libpq returned no result')
        try:
            if lib.PQresultStatus(result) not in (cell.COMMAND_OK, cell.TUPLES_OK):
                raise RuntimeError(lib.PQresultErrorMessage(result).decode().strip())
            if time.monotonic()>=DEADLINE:
                raise BudgetExpired('global runtime budget exhausted after SQL validation')
            return [[None if lib.PQgetisnull(result, r, c) else lib.PQgetvalue(result, r, c).decode()
                     for c in range(lib.PQnfields(result))] for r in range(lib.PQntuples(result))]
        finally:
            lib.PQclear(result)

    def exec(self, sql):
        # Execute separately: PQexec only returns the last result of a multi-command string.
        # Fixture scripts use psql ON_ERROR_STOP; runtime SQL is deliberately one statement.
        if time.monotonic()>=DEADLINE:
            raise BudgetExpired('global runtime budget exhausted before SQL operation')
        result = self.run(sql.encode())
        if time.monotonic()>=DEADLINE:
            raise BudgetExpired('global runtime budget exhausted after SQL operation')
        return result

    def script(self, text):
        for sql in text.split(';'):
            if sql.strip():
                self.exec(sql)


def command(argv, *, cwd=None, env=None, log=None, bounded=True):
    timeout = DEADLINE-time.monotonic() if bounded else 60
    if timeout <= 0:
        raise BudgetExpired('global runtime budget exhausted')
    proc = subprocess.Popen([str(a) for a in argv], cwd=cwd, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            start_new_session=True)
    try:
        stdout, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid,signal.SIGTERM)
        try:
            stdout, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid,signal.SIGKILL)
            stdout, _ = proc.communicate()
        if log:
            with Path(log).open('a') as f:
                f.write(stdout)
        raise BudgetExpired('global runtime budget exhausted during '+str(argv[0]))
    result = subprocess.CompletedProcess(argv,proc.returncode,stdout)
    if log:
        with Path(log).open('a') as f:
            f.write('$ ' + ' '.join(map(str, argv)) + '\n' + result.stdout)
    if result.returncode:
        raise RuntimeError(f'{argv[0]} exited {result.returncode}: {result.stdout[-3000:]}')
    return result.stdout.strip()


def exact_sha(ref):
    if not re.fullmatch('[0-9a-f]{40}', ref):
        raise ValueError('refs must be exact full 40-character commit SHAs')
    got = command(['git', 'rev-parse', ref+'^{commit}'], cwd=ROOT)
    require_equal(ref, got, 'commit identity')
    return got


# Socket endpoints only: data/build/TMPDIR remain on the caller's test backing.
SOCKET_PARENT = Path('/tmp')


def validate_socket_path(directory, port):
    if sys.platform.startswith('linux'):
        limit=107  # sockaddr_un.sun_path[108], including the terminating NUL.
    elif sys.platform.startswith(('darwin','freebsd','openbsd','netbsd','dragonfly')):
        limit=103  # These platforms use sun_path[104].
    else:
        raise ValueError('unsupported platform for UNIX socket pathname contract: '+sys.platform)
    if type(port) is not int or not 1<=port<=65535:
        raise ValueError('invalid PostgreSQL socket port')
    path=Path(directory)/f'.s.PGSQL.{port}'
    length=len(os.fsencode(path))
    if not path.is_absolute() or length>limit:
        raise ValueError(f'UNIX socket path is {length} bytes; {sys.platform} maximum {limit} pathname bytes plus NUL: {path}; use a shorter socket parent')
    return dict(path=str(path),path_bytes=length,pathname_limit_bytes=limit,port=port,
                platform=sys.platform,encoding=sys.getfilesystemencoding(),
                parent_policy='private0700 socket-only directory under /tmp; independent of scratch-root and TMPDIR')


def allocate_socket_directory(port):
    directory=Path(tempfile.mkdtemp(prefix='pgs-',dir=SOCKET_PARENT))
    try:
        directory.chmod(0o700)
        evidence=validate_socket_path(directory,port)
    except Exception:
        shutil.rmtree(directory)  # No cluster can exist before this constructor returns.
        raise
    return directory,evidence
