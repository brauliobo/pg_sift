"""What the analytic engines of the generic suites (bench/engines/ID.py, steps/generic.py) share: containers, the PostgreSQL-family server, the comparison of answers and the timing loop. Standard
library, psql and docker only.

An adapter module defines `class Engine(Server)` with `up()`, `load(suite, work)`, `runner(arm, sql)` (a function that runs the statement once: (milliseconds, rows of text cells)) and `down()`; `arms`
(default: the engine's id) names the rows it writes, `statement(case, arm)` picks the case's text and `precheck(arm, sql)` may refuse a statement the engine does not run natively. `SOURCE = True`
marks the engine that generates the suites and gives PostgreSQL's answers (pg_duckdb)."""
import datetime
import json
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import benchlib  # noqa: E402  (steps/benchlib.py)
from duckdb import Session  # noqa: E402  (a persistent psql session)

REPO = os.path.dirname(os.path.dirname(HERE))
SQL = os.path.join(REPO, 'bench', 'sql')
NAME = 'sift-generic'
UNSUPPORTED = '-- unsupported:'


log = benchlib.logger('generic')[0]


def sh(*cmd, check=True, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if check and p.returncode:
        sys.exit('generic: %s failed: %s' % (' '.join(cmd[:4]), (p.stderr or p.stdout).strip()[:400]))
    return p.stdout.strip()


def image(var):
    return sh('bash', '-c', '. %s/bench/versions.env; echo "$%s"' % (REPO, var))


class Unsupported(Exception):
    """the engine does not run this statement: the message is the reason, written to the results"""


class Server:
    """a container (or process) of one engine at a time; SECTION is the registry's"""
    SOURCE = False
    FALLBACK_PG = True      # a case with no section of the engine's dialect runs its pg statement
    section = 'pg'

    def __init__(self, ctx):
        self.ctx = ctx
        self.id = type(self).__module__.replace('engine_', '')
        self.name = '%s-%s' % (NAME, self.id)

    def arms(self, case):
        return [self.id]

    def statement(self, case, arm):
        """the text of the case for the arm, or raises Unsupported with the reason"""
        text = case.sec.get(arm) or case.sec.get(self.section) or (case.sec['pg'] if self.FALLBACK_PG else None)
        if text is None:
            raise Unsupported('no statement in the dialect of %s (the case has no `--@ %s` section)' % (self.id, self.section))
        if text.lstrip().startswith(UNSUPPORTED):
            raise Unsupported(text.lstrip()[len(UNSUPPORTED):].split('\n')[0].strip())
        return text

    def precheck(self, arm, sql):
        return None

    def approximate(self, sql):
        """whether the engine's answer to this statement is an estimate (a distinct count of a sketch): a difference from PostgreSQL's is then a fact about the engine, not a failure"""
        return False

    def docker_run(self, port_map, *args, env=(), run_args=()):
        sh('docker', 'rm', '-fv', self.name, check=False)
        flags = ['--label', 'sift-generic=1', '--cpus', str(self.ctx.cpus), '--memory', self.ctx.memory, '--memory-swap', self.ctx.memory]
        env = [x for e in env for x in ('-e', e)]
        sh('docker', 'run', '-d', '--name', self.name, *flags, *run_args, '-p', '127.0.0.1:%d:%d' % (self.ctx.port, port_map), *env, *args)

    def unload(self, suite, work):
        """drops the tables of the suite, so that the next suite may name one alike"""

    def down(self):
        sh('docker', 'rm', '-fv', self.name, check=False)


class PgServer(Server):
    """a PostgreSQL container loaded from the CSV files of the source engine with the tables of schema.json"""
    IMAGE = 'STORE_PG_IMAGE'
    PG_FLAGS = ['-c', 'shared_buffers=2GB', '-c', 'effective_cache_size=6GB', '-c', 'work_mem=64MB', '-c', 'maintenance_work_mem=512MB', '-c', 'jit=off', '-c', 'max_wal_size=8GB']
    TABLE_OPTIONS = ''      # appended to CREATE TABLE (USING columnar)
    RUN_SETUP = True        # the statements of bench/sql/SUITE/ID.sql run after the tables are created and before the load (create_hypertable)

    def __init__(self, ctx):
        super().__init__(ctx)
        self.sessions, self.schemas = {}, {}

    def up(self):
        cpus = self.ctx.cpus
        self.docker_run(5432, image(self.IMAGE), 'postgres', *self.PG_FLAGS, '-c', 'max_parallel_workers_per_gather=%d' % (cpus - 1), *self.server_args(),
                        env=['POSTGRES_HOST_AUTH_METHOD=trust'], run_args=['--shm-size', '1g'])
        for _ in range(300):
            if subprocess.run(['pg_isready', '-q', '-h', '127.0.0.1', '-p', str(self.ctx.port)]).returncode == 0 and self.psql('postgres', 'SELECT 1', check=False) == '1':
                break
            time.sleep(1)
        else:
            sys.exit('generic: %s did not start (docker logs %s)' % (self.id, self.name))
        self.psql('postgres', 'DROP DATABASE IF EXISTS bench')
        self.psql('postgres', 'CREATE DATABASE bench')
        self.psql('bench', self.extension_sql())

    def server_args(self):
        return []

    def extension_sql(self):
        return ''

    def psql(self, db, sql, *args, stdin=None, check=True):
        return sh('psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-h', '127.0.0.1', '-p', str(self.ctx.port), '-U', 'postgres', '-d', db, '-c', sql, *args, input=stdin, check=check)

    def load(self, suite, work):
        schema = self.schemas[suite] = json.load(open(os.path.join(work, suite, 'schema.json')))
        for table, cols in schema.items():
            self.psql('bench', 'CREATE TABLE %s (%s) %s' % (table, ', '.join(' '.join(c) for c in cols), self.TABLE_OPTIONS))
        path = os.path.join(SQL, suite, self.id + '.sql')
        if self.RUN_SETUP and os.path.exists(path):
            for stmt in [s for s in open(path).read().split(';\n') if s.strip()]:
                self.psql('bench', stmt)
        for table in schema:
            with open(os.path.join(work, suite, table + '.csv')) as f:
                sh('psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', '-h', '127.0.0.1', '-p', str(self.ctx.port), '-U', 'postgres', '-d', 'bench', '-c', 'COPY %s FROM STDIN CSV' % table, stdin=f)
        self.psql('bench', 'ANALYZE')

    def unload(self, suite, work):
        for table in self.schemas[suite]:
            self.psql('bench', 'DROP TABLE IF EXISTS %s CASCADE' % table)

    def session(self, arm, setting=None):
        s = Session(self.ctx.port, self.ctx.timeout, setting, db='bench')
        for st in self.arm_settings(arm):
            s.run(st)
        return s

    def arm_settings(self, arm):
        return []

    def runner(self, arm, sql):
        return session_run(self.sessions.setdefault(arm, self.session(arm)), sql)

    def down(self):
        for s in self.sessions.values():
            s.close()
        super().down()


def session_run(session, sql):
    def go():
        ms, rows, err, _ = session.run(sql)
        if err:
            raise Unsupported('error: ' + err[:200])
        return ms, [r.split('|') for r in rows]
    return go


def norm(cell):
    """one cell to a comparable value: a timestamp to microseconds, a number to 6 significant digits, anything else as text"""
    cell = cell.strip()
    if re.fullmatch(r'\d{4}-\d\d-\d\d', cell):   # pg_duckdb returns a date for date_trunc('day', ts), PostgreSQL a timestamp at midnight
        return datetime.date.fromisoformat(cell).isoformat() + 'T00:00:00'
    if re.fullmatch(r'\d{4}-\d\d-\d\d[ T]\d\d:\d\d:\d\d(\.\d+)?(Z|\+00(:00)?)?', cell):
        return datetime.datetime.fromisoformat(re.sub(r'(Z|\+00(:00)?)$', '', cell).replace(' ', 'T')).isoformat()
    try:
        return float('%.6g' % float(cell))
    except ValueError:
        return cell


def canon(rows):
    """rows (lists of text cells) in their order; the query has an ORDER BY, and a tie is a failure of the query, not of the engine"""
    return [[norm(c) for c in r] for r in rows]


def same(a, b):
    if len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if len(x) != len(y):
            return False
        for u, v in zip(x, y):
            if u != v and not (isinstance(u, float) and isinstance(v, float) and abs(u - v) <= 1e-5 * max(1.0, abs(u))):
                return False
    return True


def first_diff(a, b):
    for x, y in zip(a, b):
        if x != y:
            return ' (first difference: %s against %s)' % (x, y)
    return ''


def timed(fn, runs):
    """(median ms, min, max, answer) of `runs` timed executions after one untimed; the answer of the last; with no runs the answer of one"""
    answer = fn()[1]
    ms = []
    for _ in range(runs):
        t, answer = fn()
        ms.append(t)
    return (statistics.median(ms), min(ms), max(ms), answer) if ms else (None, None, None, answer)
