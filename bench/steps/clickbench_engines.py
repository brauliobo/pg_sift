"""The engines of the ClickBench lane behind one interface. An arm answers `rows(q)` (the decoded result of the verify query), `run(q)` (one timed
execution of the official query: wall milliseconds of the client, server milliseconds from the engine) and `evict()` (empty the engine's own caches).
Server time: PostgreSQL `EXPLAIN (ANALYZE, TIMING OFF)` Execution Time, ClickHouse `X-ClickHouse-Summary` elapsed_ns (the header of a buffered
response, so it is final), DuckDB the CLI `.timer` (one millisecond resolution). Standard library only."""
import http.client
import json
import os
import re
import subprocess
import tempfile
import time
import urllib.parse
import uuid

import clickbench_md5 as md5
import tmpfs  # noqa: F401  (tempfile on tmpfs)

END = '__END__'
CLEAN_ENV = {k: v for k, v in os.environ.items() if k not in ('AI_AGENT', 'CLAUDECODE')}


class OsCache:
    """The operating system's page cache before a cold run: `DROP_CACHES_CMD` (for example `sudo -n sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'`),
    else POSIX_FADV_DONTNEED on the files of the engine (`paths` of this host, `container` paths through `dd iflag=nocache`). `state` says which."""

    def __init__(self, cmd='', paths=(), container=None):
        """container: the name of a docker container whose files `container_paths()` lists (set by the arm that knows them)"""
        self.cmd, self.paths, self.container, self.state = cmd, list(paths), container, 'kept'
        self.container_paths = lambda: []

    def drop(self):
        if self.cmd:
            subprocess.run(self.cmd, shell=True, check=True)
            self.state = 'dropped'
            return
        files = [os.path.join(d, f) for p in self.paths for d, _, fs in os.walk(p) for f in fs] + [p for p in self.paths if os.path.isfile(p)]
        for f in files:
            try:
                fd = os.open(f, os.O_RDONLY)
            except FileNotFoundError:   # the engine removed it since the walk (relation init files, merged parts)
                continue
            try:
                os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
            finally:
                os.close(fd)
        inside = self.container_paths() if self.container else []
        if inside:
            fadvise = "dd if=\"$0\" iflag=nocache count=0 status=none of=/dev/null 2>/dev/null || [ ! -e \"$0\" ]"   # a part merged away meanwhile is gone, not an error
            subprocess.run(['docker', 'exec', self.container, 'sh', '-c', "find %s -type f -print0 2>/dev/null | xargs -0 -r -n 1 -P 8 sh -c '%s'" % (
                ' '.join(inside), fadvise)], check=True)
        if files or inside:
            self.state = 'dropped'


class Psql:
    def __init__(self, args, init=()):
        self.p = subprocess.Popen(['psql', '-X', '-q', '-A', '-t', *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, env=CLEAN_ENV)
        for sql in init:
            self.run(sql)

    def run(self, sql, sink=False):
        """(milliseconds, output lines); a PostgreSQL error is an exception. sink: the rows go to /dev/null (timed runs)"""
        text = sql.rstrip().rstrip(';') + ';\n'
        self.p.stdin.write(('\\o /dev/null\n%s\\o\n' if sink else '%s') % text + '\\echo %s\n' % END)
        t = time.perf_counter()
        self.p.stdin.flush()
        out = []
        for line in self.p.stdout:
            if line.rstrip('\n') == END:
                break
            out.append(line.rstrip('\n'))
        else:
            raise RuntimeError('psql ended: ' + ' '.join(out)[:300])
        ms = (time.perf_counter() - t) * 1000
        err = next((l for l in out if re.match(r'(psql:.*)?(ERROR|FATAL):', l)), None)
        if err:
            raise RuntimeError(err[:300])
        return ms, out

    def close(self):
        self.p.stdin.close()
        self.p.wait()


class PgArm:
    engine = 'pg'

    def __init__(self, name, conn, table='hits', threads=4, work_mem='256MB', restart_cmd='', os_cache=None):
        self.name, self.conn, self.table, self.restart_cmd = name, conn, table, restart_cmd
        self.os_cache = os_cache or OsCache()
        self.init = ['SET max_parallel_workers_per_gather = %d' % (threads - 1), 'SET max_parallel_workers = %d' % threads,
                     "SET work_mem = '%s'" % work_mem, 'SET jit = off', 'SET statement_timeout = 0']
        self.s = Psql(conn, self.init)

    def rows(self, q):
        return md5.decode_tsv('\n'.join(self.s.run('COPY (%s) TO STDOUT' % q.render_verify('pg', self.table))[1]))

    def run(self, q, server=True):
        sql = q.render('pg', self.table)
        wall = self.s.run(sql, sink=True)[0]
        if not server:
            return wall, None
        plan = json.loads('\n'.join(self.s.run('EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON, FORMAT JSON) ' + sql)[1]))[0]
        return wall, plan['Execution Time']

    def server_only(self, q):
        return self.run(q)[1]

    def evict(self):
        if self.restart_cmd:
            self.s.close()
            subprocess.run(self.restart_cmd, shell=True, check=True)
            self.s = Psql(self.conn, self.init)
        else:
            self.s.run('SELECT pg_buffercache_evict_all()')

    def close(self):
        self.s.close()


class ChArm:
    engine = 'ch'

    def __init__(self, name, url, user='default', password='', table='hits', threads=4, os_cache=None):
        host, port = urllib.parse.urlparse(url).netloc.split(':')
        self.name, self.table, self.os_cache, self.ids = name, table, os_cache or OsCache(), []
        self.headers = {'X-ClickHouse-User': user, 'X-ClickHouse-Key': password}
        self.host, self.port, self.threads, self.conn = host, int(port), threads, None
        self.os_cache.container_paths = self.parts_dirs

    def parts_dirs(self):
        sql = "SELECT DISTINCT path FROM system.parts WHERE active AND database = 'default' AND table = '%s'" % self.table
        return [l for l in self.request(sql)[0].decode().splitlines() if l]

    def request(self, sql, body=None, **params):
        """the response (data, http response); with a body (an INSERT's rows) the statement travels in the URL"""
        query = urllib.parse.urlencode(dict(database='default', default_format='TabSeparated', max_threads=self.threads, **params,
                                            **({} if body is None else dict(query=sql))))
        for attempt in (0, 1):
            try:
                self.conn = self.conn or http.client.HTTPConnection(self.host, self.port, timeout=3600)
                self.conn.request('POST', '/?' + query, body=sql.encode() if body is None else body, headers=self.headers)
                r = self.conn.getresponse()
                data = r.read()
                if r.status >= 400:
                    raise RuntimeError('ClickHouse %d: %s' % (r.status, data.decode(errors='replace')[:300]))
                return data, r
            except (http.client.HTTPException, OSError):
                self.conn = None
                if attempt:
                    raise

    def rows(self, q):
        return md5.decode_tsv(self.request(q.render_verify('ch', self.table))[0].decode())

    def run(self, q, server=True):
        qid = str(uuid.uuid4())
        t = time.perf_counter()
        _, r = self.request(q.render('ch', self.table), wait_end_of_query=1, query_id=qid)
        wall = (time.perf_counter() - t) * 1000
        self.ids.append(qid)
        return wall, int(json.loads(r.getheader('X-ClickHouse-Summary'))['elapsed_ns']) / 1e6

    def server_only(self, q):
        return self.run(q)[1]

    def evict(self):
        for what in ('MARK CACHE', 'UNCOMPRESSED CACHE', 'PRIMARY INDEX CACHE', 'INDEX MARK CACHE', 'INDEX UNCOMPRESSED CACHE', 'QUERY CACHE'):
            self.request('SYSTEM DROP ' + what)

    def log(self):
        """{query_id: (read_rows, read_bytes, projections used)} of the timed runs, from system.query_log"""
        self.request('SYSTEM FLUSH LOGS')
        ids = ', '.join("'%s'" % i for i in self.ids)
        text = self.request("SELECT query_id, read_rows, read_bytes, arrayStringConcat(projections, '+') FROM system.query_log "
                            "WHERE type = 'QueryFinish' AND query_id IN (%s)" % ids)[0].decode() if self.ids else ''
        return {r[0]: (int(r[1]), int(r[2]), r[3]) for r in md5.decode_tsv(text)}

    def close(self):
        self.conn = None


class DuckArm:
    engine = 'duckdb'

    def __init__(self, name, binary, dbfile, threads=4, memory='16GB', os_cache=None):
        self.name, self.binary, self.dbfile, self.threads, self.memory = name, binary, dbfile, threads, memory
        self.os_cache = os_cache or OsCache(paths=[dbfile])
        self.start()

    def start(self):
        self.p = subprocess.Popen([self.binary, '-noheader', self.dbfile], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, env=CLEAN_ENV)
        for sql in ('.timer on', '.mode trash', 'SET threads = %d' % self.threads, "SET memory_limit = '%s'" % self.memory):
            self.exec(sql)

    def exec(self, sql):
        """(wall milliseconds, server milliseconds from .timer of the last statement, output lines)"""
        self.p.stdin.write((sql if sql.startswith('.') else sql.rstrip().rstrip(';') + ';') + '\n.print %s\n' % END)
        t = time.perf_counter()
        self.p.stdin.flush()
        out = []
        for line in self.p.stdout:
            if line.rstrip('\n') == END:
                break
            out.append(line.rstrip('\n'))
        else:
            raise RuntimeError('duckdb ended: ' + ' '.join(out)[:300])
        wall = (time.perf_counter() - t) * 1000
        err = next((l for l in out if re.match(r'(Parser |Binder |Catalog |Conversion |Out of Memory |IO |Invalid Input |Constraint )?Error:', l)), None)
        if err:
            raise RuntimeError(err[:300])
        times = [float(m.group(1)) * 1000 for l in out for m in [re.match(r'Run Time \(s\): real ([0-9.]+)', l)] if m]
        return wall, (times[-1] if times else None), [l for l in out if not l.startswith('Run Time (s)')]

    def rows(self, q):
        with tempfile.TemporaryDirectory(prefix='sift-cbpkg-', dir=tmpfs.require_tmpfs(tmpfs.root())) as d:
            f = os.path.join(d, 'out.csv')
            self.exec("COPY (%s) TO '%s' (FORMAT csv, HEADER false, DELIMITER '\\t', NULL '\\N')" % (q.render_verify('duckdb'), f))
            return md5.decode_csv_tab(open(f, newline='').read())

    def run(self, q, server=True):
        wall, server_ms, _ = self.exec(q.render('duckdb'))
        return wall, server_ms

    def server_only(self, q):
        return self.run(q)[1]

    def evict(self):
        self.close()
        self.start()

    def close(self):
        self.p.stdin.close()
        self.p.wait()
