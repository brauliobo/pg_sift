#!/usr/bin/env python3
"""Coverage and plan-gate benchmark: every read query of docs/coverage.tsv and test/plan/shapes.sql on PostgreSQL 18, pg_sift v5.1.0 and this tree.

    bench/bench cov-plan [--rows N] [--suites coverage,plan] [--storage col,table] [--workers 1,4] [--no-v510] [--v510-src PATH]
                         [--filter REGEX] [--reps N] [--plan-only] [--no-lock] [--out DIR] [--keep] [--selftest]

Release builds (SIFT_TESTING=), the stock planner costs and jit = off, on two throwaway clusters under SIFT_TMP (ports --port and --port + 1). The arms are pg-Nt (this tree
with sift.enable_agg, enable_scan and enable_col_scan off: the oracle; 1 worker is always run), v510-{col,tbl}-Nt and sift60-{col,tbl}-Nt. Every answer is compared with
the one of pg-1t before it is timed (BAD_RESULT); a v5.1.0 statement that raises is UNSUPPORTED_510. Per query and arm: the plan (node, buffers, workers, estimate error),
the user instructions of the backend (perf stat, leader only, NA without perf) and the wall clock of interleaved rounds under flock /tmp/sift-perf.lock. A forced-plan probe
(sift60-*-1t-forced) runs only where the stock planner kept the normal plan of a query with a pushdown. Output: manifest.tsv, raw.tsv, summary.md in --out.
"""
import argparse, atexit, collections, fcntl, glob, json, math, os, re, shutil, signal, statistics, subprocess, sys, time
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import plan_matrix

TAGS = {'sets', 'key', 'tol', 'name', 'shape', 'expect', 'scenario', 'guc', 'prepare', 'args', 'requires', 'wrap', 'error', 'isolation', 'runs'}
DROPPED_GUC = re.compile(r'^\s*(seq_page_cost|random_page_cost|max_parallel_workers_per_gather|sift\.test_\w+)\s*(=|TO)\b', re.I)
READ = re.compile(r'^[\s(]*(select|with|values|table|execute)\b', re.I)
MACRO = re.compile(r'\{(tenants|sources|span\(|range\()')
SESSION_PRE = re.compile(r'^\s*(SET|RESET|PREPARE|DEALLOCATE)\b', re.I)
STOCK_OFF = ('sift.enable_agg', 'sift.enable_scan', 'sift.enable_col_scan')
OFF_GUCS = ('sift.col_auto_merge', 'sift.table_auto_defrag', 'sift.async_maintenance', 'pg_sift.install_notice')
LOCK = '/tmp/sift-perf.lock'
END, FIN, TIMEOUT_S = '__END__', '__FIN__', 60
C_ENV = {**os.environ, 'LC_ALL': 'C', 'LANGUAGE': 'C', 'PGCLIENTENCODING': 'UTF8'}
RAW_COLS = ['suite', 'qid', 'family', 'expect', 'arm', 'status', 'node', 'median_ms', 'iqr_ms', 'reps', 'exec_instr', 'plan_instr', 'shared_blks', 'heap_fetches',
            'workers_planned', 'workers_launched', 'est_x', 'detail']


@dataclass
class Query:
    suite: str
    qid: str
    family: str
    expect: str
    body: str
    pre: list = field(default_factory=list)
    gucs: list = field(default_factory=list)
    prepare: str = ''
    args: str = ''
    wrap: str = ''
    key: str = ''

    @property
    def ranks(self):
        return 'sift.score' in self.body.lower()

    @property
    def pushdown_expected(self):
        return self.expect.split()[0] in ('agg', 'scan', 'count', 'bitmap', 'either')

    def stmt(self, body, args):
        return f'EXECUTE cov_p({args})' if self.prepare else f'SELECT * FROM ({body}) cov_w' if self.wrap == 'subquery' else body


def parse_text(txt):
    """The `-- pre:` lines, the tags of the header (coldiff.header) and the body of a corpus file."""
    pre, lines = [], []
    for line in txt.split('\n'):
        (pre if line.startswith('-- pre: ') else lines).append(line[8:] if line.startswith('-- pre: ') else line)
    head, body, inhead = {}, [], True
    for line in lines:
        if inhead and line.startswith('--'):
            if (m := re.match(r'--\s*(\w+):[ \t]*(.*?)\s*$', line)) and m[1] in TAGS:
                head.setdefault(m[1], []).append(m[2])
            continue
        inhead = False
        body.append(line)
    return pre, head, '\n'.join(body).strip(' \n\t;')


def load_coverage():
    included, excluded = [], []
    for line in open(os.path.join(REPO, 'docs', 'coverage.tsv')):
        f = line.rstrip('\n').split('\t')
        if f[0].startswith('#') or f[0] == 'id':
            continue
        qid, area, check, text = f[0], f[1], f[9], f[10]
        text = open(os.path.join(REPO, 'test', 'col', 'fixtures', text[1:] + '.sql')).read() if text.startswith('@') else text.replace('\\n', '\n')
        pre, head, body = parse_text(text)
        reason = ('boolean self-check' if check == 'true' else 'error path' if check.startswith('error:') else 'second cluster' if check.startswith('external:')
                  else None if READ.match(body) else 'DDL/DML statement')
        if reason:
            excluded.append((qid, reason))
            continue
        tag = lambda t: head.get(t, [''])[-1]
        included.append(Query('coverage', qid, area, check.split()[0], body, [p for p in pre if not re.match(r'\s*SET\s+sift\.test_', p, re.I)],
                              [g for g in head.get('guc', []) if not DROPPED_GUC.match(g)], tag('prepare'), tag('args'), tag('wrap'), tag('key')))
    return included, excluded


def load_plan():
    classes = dict(l.rstrip('\n').split('\t')[:2] for l in open(os.path.join(REPO, 'test', 'plan', 'calibrate.tsv')) if l.strip() and not l.startswith('#'))
    shapes, cur = [], None
    for line in open(os.path.join(REPO, 'test', 'plan', 'shapes.sql')):
        if m := re.match(r'-- name: (\w+)\s*$', line):
            cur = Query('plan', m[1], classes.get(m[1], 'other'), '', '')
            shapes.append(cur)
        elif cur and (m := re.match(r'-- expect: (.*?)\s*$', line)):
            cur.expect = m[1]
        elif cur and line.strip() and not line.startswith('--'):
            cur.body = (cur.body + '\n' + line).strip(' \n\t;')
    return shapes


def selftest():
    inc, exc = load_coverage()
    plan = load_plan()
    assert len({q.qid for q in inc}) == len(inc) and len({q.qid for q in plan}) == len(plan), 'duplicate query ids'
    assert all(q.body for q in inc + plan) and all(q.expect for q in plan), 'a query without a body or an expectation'
    print(f'coverage: {len(inc)} included / {len(exc)} excluded ({len(inc) + len(exc)} total)')
    print('excluded: ' + ', '.join(f'{q} ({r})' for q, r in exc))
    print(f'plan: {len(plan)} included (' + ', '.join(f'{n} {c}' for c, n in sorted(collections.Counter(q.expect.split()[0] for q in plan).items())) + ')')
    print(f'total: {len(inc) + len(plan)}')


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


class Session:
    """One persistent psql: statements go in; the rows, the timing line and the SQLSTATE of the last statement come out."""

    def __init__(self, cl, db):
        self.cl, self.db = cl, db
        self.p = subprocess.Popen(['psql', '-X', '-qAt', '-h', cl.sock, '-p', str(cl.port), '-U', 'postgres', '-d', db], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, env=C_ENV)
        self.pid = int(self.run('SELECT pg_backend_pid()').out[0])

    def run(self, sql, timing=False, quiet=False):
        """quiet drops the rows into /dev/null; the Time line of timing stays on stdout."""
        pre = ('\\timing on\n' if timing else '') + ('\\o /dev/null\n' if quiet else '')
        post = ('\\o\n' if quiet else '') + ('\\timing off\n' if timing else '')
        self.p.stdin.write(f'{pre}{sql.rstrip(";")};\n{post}\\echo {END} :SQLSTATE :LAST_ERROR_MESSAGE\n\\echo {FIN}\n')
        self.p.stdin.flush()
        out = []
        while (line := self.p.stdout.readline()) and not line.startswith(END):
            out.append(line)
        if not line:
            raise ConnectionError('psql ended: the backend died')
        state, _, msg = line[len(END):].strip().partition(' ')
        while (tail := self.p.stdout.readline()) and not tail.startswith(FIN):
            msg += ' ' + tail.strip()
        ms = next((float(m[1]) for l in out if (m := re.match(r'Time: ([\d.]+) ms', l))), None)
        return Result([l for l in out if not l.startswith('Time: ')], state, msg.strip(), ms)

    def close(self):
        self.p.stdin.close()
        self.p.wait()


@dataclass
class Result:
    out: list
    state: str
    msg: str
    ms: float

    @property
    def err(self):
        return self.state != '00000'


def inline(path):
    """The script with its `\\ir` includes spliced in, so that a relaxed run drops the ON_ERROR_STOP of the included files too."""
    return re.sub(r'^\\ir\s+(\S+)\s*$', lambda m: inline(os.path.join(os.path.dirname(path), m[1])), open(path).read(), flags=re.M)


class Cluster:
    def __init__(self, name, port, base, root):
        self.name, self.port = name, port
        self.data, self.sock = f'{base}/{name}-data', f'{base}/{name}-sock'
        self.lib = f'{root}{sh(["pg_config", "--pkglibdir"]).stdout.strip()}'
        self.share = f'{root}{sh(["pg_config", "--sharedir"]).stdout.strip()}'
        self.warnings = collections.defaultdict(list)

    def start(self):
        os.makedirs(self.sock)
        subprocess.run(['initdb', '--no-sync', '-D', self.data, '-A', 'trust', '-U', 'postgres', '-E', 'UTF8', '--no-locale', '--no-instructions'], check=True, capture_output=True)
        so = open(f'{self.lib}/pg_sift.so', 'rb').read()
        conf = f"""listen_addresses = ''
unix_socket_directories = '{self.sock}'
port = {self.port}
shared_buffers = '512MB'
work_mem = '64MB'
maintenance_work_mem = '512MB'
jit = off
autovacuum = off
fsync = off
synchronous_commit = on
full_page_writes = off
max_wal_size = '8GB'
checkpoint_timeout = '30min'
max_worker_processes = 16
max_parallel_workers = 8
seq_page_cost = 1.0
random_page_cost = 4.0
parallel_setup_cost = 1000
parallel_tuple_cost = 0.1
log_min_messages = warning
shared_preload_libraries = 'pg_sift'
dynamic_library_path = '{self.lib}:$libdir'
extension_control_path = '{self.share}:$system'
""" + ''.join(f'{g} = off\n' for g in OFF_GUCS if g.encode() in so)
        open(f'{self.data}/postgresql.conf', 'a').write(conf)
        subprocess.run(['pg_ctl', '-D', self.data, '-l', f'{self.data}/log', '-w', 'start'], check=True, capture_output=True)

    def stop(self):
        if os.path.exists(f'{self.data}/postmaster.pid'):
            sh(['pg_ctl', '-D', self.data, '-m', 'immediate', '-w', 'stop'])

    def wait_ready(self):
        for _ in range(240):
            if sh(['pg_isready', '-h', self.sock, '-p', str(self.port)]).returncode == 0:
                return
            time.sleep(0.5)
        sys.exit(f'cov-plan: {self.name} did not come back after a crash')

    def psql(self, db, text, strict=True, **vars):
        """Runs a script as psql -f: strict stops at its first error, else every failing statement is kept in warnings[db] and the script goes on."""
        text = text if strict else text.replace('\\set ON_ERROR_STOP on', '\\set ON_ERROR_STOP off')
        args = ['psql', '-X', '-q', '-h', self.sock, '-p', str(self.port), '-U', 'postgres', '-d', db, '-v', f'ON_ERROR_STOP={int(strict)}']
        r = sh(args + [x for k, v in vars.items() for x in ('-v', f'{k}={v}')] + ['-f', '-'], input=text, env=C_ENV)
        if strict and r.returncode:
            sys.exit(f'cov-plan: {self.name} {db}: ' + '\n'.join(l for l in r.stderr.splitlines() if 'NOTICE' not in l)[:800])
        self.warnings[db] += [l for l in r.stderr.splitlines() if 'ERROR:' in l]
        return r.stdout

    def file(self, db, path, strict, **vars):
        return self.psql(db, inline(os.path.join(REPO, path)), strict, **vars)


def group_paths(spec):
    """The declaration of a sift_col index split by the jsonb column each path starts with."""
    parts, depth, cur = [], 0, ''
    for ch in spec.strip('"'):
        depth += (ch in '{(') - (ch in '})')
        if ch == ',' and not depth:
            parts.append(cur.strip())
            cur = ''
        else:
            cur += ch
    groups = collections.defaultdict(list)
    for p in filter(None, parts + [cur.strip()]):
        groups[re.search(r'\w+', p)[0]].append(p)
    return {c: ', '.join(ps) for c, ps in groups.items()}


def to_tables(cl, db, strict):
    """The sift_col tables of db as sift_table: the column indexes dropped, every table and partition rewritten, the jsonb paths of the indexes declared on the tables."""
    fmt = "\\g (format=unaligned fieldsep='|' tuples_only=on)"
    rows = [l.split('|') for l in cl.psql(db, f"""SELECT i.indrelid::regclass, c.relname, coalesce((SELECT substr(o, 7) FROM unnest(c.reloptions) o WHERE o LIKE 'paths=%'), '')
        FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_col' ORDER BY 1, 2 {fmt}""").splitlines()]
    tables = sorted({r[0] for r in rows})
    leaves = {t: cl.psql(db, f"SELECT relid::regclass FROM pg_partition_tree('{t}') WHERE isleaf {fmt}").split() for t in tables}
    ddl = [f'DROP INDEX IF EXISTS {r[1]};' for r in rows] + [f'ALTER TABLE {t} SET ACCESS METHOD sift_table;' for t in dict.fromkeys(tables + sum(leaves.values(), []))]
    ddl += [f"SELECT sift.table_set_paths('{t}', '{c}', '{p}');" for r in rows for c, p in group_paths(r[2]).items() for t in leaves[r[0]]]
    cl.psql(db, '\n'.join(ddl + [f'VACUUM FULL {t};' for t in tables] + ['VACUUM (FREEZE, ANALYZE);']), strict)


def build_dbs(cl, suite, rows, storage, strict):
    pre = 'cov' if suite == 'coverage' else 'plan'
    cl.psql('postgres', f'DROP DATABASE IF EXISTS {pre}_col WITH (FORCE); DROP DATABASE IF EXISTS {pre}_tbl WITH (FORCE); CREATE DATABASE {pre}_col;')
    cl.psql(f'{pre}_col', 'CREATE EXTENSION pg_sift', strict)
    if pre == 'cov':
        cl.psql('postgres', 'DROP ROLE IF EXISTS sift_cov_user')
        cl.file('cov_col', 'test/col/run.sql', strict)
        cl.file('cov_col', 'test/col/fixture.sql', strict, n=rows, seed=0)
        cl.file('cov_col', 'test/col/coverage_data.sql', strict, n=rows)
    else:
        cl.file('plan_col', 'test/plan/gen.sql', strict, n=rows, seed=1)
    cl.psql(f'{pre}_col', 'VACUUM (FREEZE, ANALYZE)', strict)
    if 'table' in storage:
        cl.psql('postgres', f'CREATE DATABASE {pre}_tbl TEMPLATE {pre}_col')
        to_tables(cl, 'cov_tbl', False) if pre == 'cov' else cl.file('plan_tbl', 'test/plan/table.sql', strict)


def git_repos(start, seen=None):
    """The local repositories a clone leads to: itself, then its alternates and its origin, recursively."""
    seen = [] if seen is None else seen
    if start in seen or not os.path.isdir(start):
        return seen
    seen.append(start)
    alt = os.path.join(sh(['git', '-C', start, 'rev-parse', '--absolute-git-dir']).stdout.strip(), 'objects', 'info', 'alternates')
    for line in open(alt).read().split() if os.path.exists(alt) else []:
        git_repos(os.path.dirname(os.path.dirname(line)), seen)
    return git_repos(sh(['git', '-C', start, 'config', 'remote.origin.url']).stdout.strip(), seen)


def find_v510(arg):
    siblings = [d for d in sorted(glob.glob(os.path.join(os.path.dirname(REPO), '*'))) if os.path.exists(os.path.join(d, '.git'))]
    for c in [arg, os.environ.get('SIFT_V510_SRC')] + git_repos(REPO) + siblings + [os.path.join(REPO, '..', '..', 'wt-510')]:
        if c and os.path.isdir(c) and sh(['git', '-C', c, 'rev-parse', '--verify', '--quiet', 'v5.1.0^{commit}']).returncode == 0:
            return os.path.abspath(c)
    sys.exit('cov-plan: no repository with the tag v5.1.0 found (--v510-src PATH or SIFT_V510_SRC, or --no-v510)')


@dataclass
class Arm:
    eng: str
    storage: str
    n: int
    cl: object

    @property
    def name(self):
        return f'pg-{self.n}t' if self.eng == 'pg' else f'{self.eng}-{"col" if self.storage == "col" else "tbl"}-{self.n}t'

    def db(self, suite):
        return f'{"cov" if suite == "coverage" else "plan"}_{"col" if self.storage == "col" else "tbl"}'

    def setup(self):
        return [f"RESET ALL; SET jit = off; SET max_parallel_workers_per_gather = {0 if self.n == 1 else self.n}; SET statement_timeout = '{TIMEOUT_S}s';"] + \
               [f'SET {g} = off;' for g in STOCK_OFF if self.eng == 'pg']


def walk(plan):
    yield plan
    for c in plan.get('Plans', []):
        yield from walk(c)


class Perf:
    """perf stat of the user instructions of one backend around some statements: started disabled, then enabled over a control pipe and acknowledged, so no sleep orders it."""

    def __init__(self, tmp):
        self.out, self.ok = os.path.join(tmp, 'perf.out'), bool(shutil.which('perf'))

    def count(self, s, sqls):
        if not self.ok:
            return None
        ctl_r, ctl_w = os.pipe()
        ack_r, ack_w = os.pipe()
        p = subprocess.Popen(['perf', 'stat', '-x,', '-e', 'instructions:u', '-p', str(s.pid), '-D', '-1', '--control', f'fd:{ctl_r},{ack_w}', '-o', self.out],
                             stderr=subprocess.DEVNULL, pass_fds=(ctl_r, ack_w))
        os.close(ctl_r)
        os.close(ack_w)
        os.write(ctl_w, b'enable\n')
        if os.read(ack_r, 16).startswith(b'ack'):
            for q in sqls:
                s.run(q, quiet=True)
        p.send_signal(signal.SIGINT)
        p.wait()
        os.close(ctl_w)
        os.close(ack_r)
        m = re.search(r'^(\d+),,instructions', open(self.out).read(), re.M)
        self.ok = bool(m)
        return int(m[1]) if m else None


def plan_facts(s, cache, stmt):
    r = s.run('EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, FORMAT JSON) ' + stmt)
    if r.err:
        return r, None
    root = json.loads(''.join(r.out))[0]['Plan']
    ns = list(walk(root))

    def sift_index(name):
        if name not in cache:
            cache[name] = bool(s.run(f"SELECT 1 FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift' AND c.relname = '{name}'").out)
        return cache[name]
    node = next((n['Custom Plan Provider'] for n in ns if n.get('Custom Plan Provider', '').startswith('Sift')), None) or (
        'BitmapSift' if any(n.get('Index Name') and sift_index(n['Index Name']) for n in ns) else 'normal')
    return r, {'node': node, 'shared_blks': root.get('Shared Hit Blocks', 0) + root.get('Shared Read Blocks', 0), 'heap_fetches': sum(n.get('Heap Fetches', 0) for n in ns),
               'workers_planned': sum(n.get('Workers Planned', 0) for n in ns), 'workers_launched': sum(n.get('Workers Launched', 0) for n in ns),
               'est_x': round(plan_matrix.est_factor(root), 1)}


def fetch(s, stmt):
    """The answer of stmt: (signature, rows as lists of values), or (None, SQLSTATE)."""
    r = s.run(f'DROP TABLE IF EXISTS pg_temp.cov_r; CREATE TEMP TABLE cov_r AS {stmt};')
    if r.err:
        return None, r.state
    sig = s.run("SELECT string_agg(a.attname || ' ' || format_type(a.atttypid, a.atttypmod), ', ' ORDER BY a.attnum) FROM pg_attribute a "
                "WHERE a.attrelid = 'pg_temp.cov_r'::regclass AND a.attnum > 0 AND NOT a.attisdropped").out[0].strip()
    return sig, [json.loads(l, object_pairs_hook=lambda p: [v for _, v in p]) for l in s.run('SELECT to_json(cov_r)::text FROM pg_temp.cov_r ORDER BY ctid').out]


def feq(x, y):
    if x == y:
        return True
    if not all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in (x, y)):
        return False
    return abs(x / 2 - y / 2) <= 1e-12 / 2 + 1e-9 * max(abs(x), abs(y)) / 2


def differs(q, ref, got):
    """None when got answers as ref (floats within 1e-9 relative and 1e-12 absolute; rows sorted by q.key unless it is -), else what differs."""
    (rsig, rrows), (gsig, grows) = ref, got
    if rsig != gsig:
        return f'columns differ: pg ({rsig}) arm ({gsig})'
    keys = [int(k) - 1 for k in q.key.split()] if q.key not in ('', '-') else []
    order = (lambda rows: rows) if q.key == '-' else (lambda rows: sorted(rows, key=lambda r: [str(r[i]) for i in keys] + [str(v) for v in r]))
    floats = [t.split()[-1] in ('real', 'precision') for t in re.split(r', (?=\w+ )', rsig)]
    a, b = order(rrows), order(grows)
    for i, (x, y) in enumerate(zip(a, b)):
        if len(x) != len(y) or not all(feq(u, v) if f else u == v for u, v, f in zip(x, y, floats)):
            return f'{len(a)} rows pg, {len(b)} rows arm; row {i}: pg {str(x)[:200]} arm {str(y)[:200]}'
    return None if len(a) == len(b) else f'{len(a)} rows pg, {len(b)} rows arm'


def iqr(ts):
    q = statistics.quantiles(ts, n=4, method='inclusive') if len(ts) > 1 else [0, 0, 0]
    return q[2] - q[0]


def settled(ts):
    m = statistics.median(ts)
    return len(ts) >= 3 and (m < 1 or iqr(ts) / m < 0.08 or sum(ts) >= 2000)


def record(q, arm, status='OK', **kw):
    return {'suite': q.suite, 'qid': q.qid, 'family': q.family, 'expect': q.expect.split()[0], 'arm': arm, 'status': status, 'node': '-', **kw}


def prepare_arm(q, a, s, shared_done):
    """The settings of the arm and of q, the statements of its `-- pre:` lines (those of the database once, the ones of a session in each): the Result of a failing one."""
    for stmt in a.setup() + [f'SET {g};' for g in q.gucs]:
        if (r := s.run(stmt)).err:
            return r
    for p in q.pre:
        if (SESSION_PRE.match(p) or (s.cl.name, s.db) not in shared_done) and (r := s.run(p)).err:
            return r
    shared_done.add((s.cl.name, s.db))


def classify(a, ref, got, q):
    """(status, detail) of an arm whose answer got is not the one of the oracle ref, None when it is."""
    if ref[0] is None and got[0] is None and ref[1] == got[1]:
        return 'ERROR', f'raised {got[1]} as pg'
    if got[0] is None:
        return ('UNSUPPORTED_510' if a.eng == 'v510' else 'BAD_RESULT'), f'raised {got[1]}'
    if ref[0] is None:
        return 'BAD_RESULT', f'pg raised {ref[1]}, the arm answers'
    if not q.ranks and (d := differs(q, ref, got)):
        return 'BAD_RESULT', d


def run_query(q, arms, S, args, perf, caches):
    """The records of q, one per arm (and per forced probe). S: arm name -> Session."""
    recs, shared_done = {}, set()
    for a in arms:
        if r := prepare_arm(q, a, S[a.name], shared_done):
            recs[a.name] = record(q, a.name, 'UNSUPPORTED_510' if a.eng == 'v510' else 'ERROR', detail=r.msg[:160])
    if 'pg-1t' in recs:
        return {a.name: recs['pg-1t'] | {'arm': a.name} for a in arms}
    ref = S['pg-1t']
    body, qargs = (ref.run(f'SELECT coldiff.expand($q${t}$q$, NULL)').out[0].strip() if MACRO.search(t) else t for t in (q.body, q.args))
    stmt = q.stmt(body, qargs)
    for a in arms:
        if q.prepare and a.name not in recs and (r := S[a.name].run(f'DEALLOCATE ALL; PREPARE cov_p {q.prepare} AS {body};')).err:
            recs[a.name] = record(q, a.name, 'UNSUPPORTED_510' if a.eng == 'v510' else 'ERROR', detail=r.msg[:160])
    answers = {a.name: fetch(S[a.name], stmt) for a in arms if a.n == 1 and a.name not in recs}
    for a in arms:
        if a.name in answers and a.name != 'pg-1t' and (c := classify(a, answers['pg-1t'], answers[a.name], q)):
            recs[a.name] = record(q, a.name, c[0], detail=c[1][:300])
    if answers['pg-1t'][0] is None:
        recs['pg-1t'] = record(q, 'pg-1t', 'ERROR', detail=f'raised {answers["pg-1t"][1]}')
    for a in arms:
        if a.n != 1 and (sib := recs.get(a.name.replace(f'-{a.n}t', '-1t'))):
            recs[a.name] = sib | {'arm': a.name}
    live = []
    for a in arms:
        if a.name in recs:
            continue
        r, facts = plan_facts(S[a.name], caches[(a.cl.name, S[a.name].db)], stmt)
        if facts is None:
            recs[a.name] = record(q, a.name, 'TIMEOUT' if r.state == '57014' else 'UNSUPPORTED_510' if a.eng == 'v510' else 'ERROR', detail=r.msg[:160])
            continue
        plan = perf.count(S[a.name], [f'EXPLAIN (FORMAT JSON) {stmt}'] * 3)
        recs[a.name] = record(q, a.name, **facts, exec_instr=perf.count(S[a.name], [stmt]), plan_instr=None if plan is None else round(plan / 3))
        live.append(a.name)
    times = {n: [] for n in live}
    for rnd in range(0 if args.plan_only else args.reps):
        order = live[rnd % len(live):] + live[:rnd % len(live)]
        for n in order[::-1] if rnd % 2 else order:
            r = S[n].run(stmt, timing=True, quiet=True)
            if r.err:
                recs[n].update(status='TIMEOUT' if r.state == '57014' else 'ERROR', detail=r.msg[:160])
            else:
                times[n].append(r.ms)
        live = [n for n in live if recs[n]['status'] == 'OK']
        if all(settled(times[n]) for n in live):
            break
    for n in live if times and times[live[0]] else []:
        recs[n].update(median_ms=statistics.median(times[n]), iqr_ms=iqr(times[n]), reps=len(times[n]))
    for a in arms:
        if a.eng == 'sift60' and a.n == 1 and recs[a.name]['status'] == 'OK' and recs[a.name]['node'] == 'normal' and q.pushdown_expected:
            recs[a.name + '-forced'] = probe(q, a, S[a.name], args, stmt, caches[(a.cl.name, S[a.name].db)])
    for a in arms:
        S[a.name].run('DEALLOCATE ALL')
    return recs


def probe(q, a, s, args, stmt, cache):
    s.run('SET seq_page_cost = 10000; SET random_page_cost = 10000;')
    r, facts = plan_facts(s, cache, stmt)
    rec = record(q, a.name + '-forced', **facts) if facts else record(q, a.name + '-forced', 'ERROR', detail=r.msg[:160])
    if facts and not args.plan_only:
        ts = [s.run(stmt, timing=True, quiet=True).ms for _ in range(4)][1:]
        rec.update(median_ms=statistics.median(ts), iqr_ms=iqr(ts), reps=len(ts))
    return rec


def geomean(xs):
    return math.exp(sum(map(math.log, xs)) / len(xs)) if xs else None


def md(head, rows):
    return '\n'.join(['| ' + ' | '.join(head) + ' |', '|' + '---|' * len(head)] + ['| ' + ' | '.join('-' if c is None else f'{c:.2f}' if isinstance(c, float) else str(c) for c in r) + ' |' for r in rows]) + '\n'


class Report:
    """summary.md from the raw records: the rollups and the six red-flag tables."""

    def __init__(self, rows, notes):
        self.rows, self.notes = rows, notes
        self.R = collections.defaultdict(dict)
        for r in rows:
            self.R[(r['suite'], r['qid'])][r['arm']] = r
        self.arms = sorted({r['arm'] for r in rows if r['arm'].startswith('sift60') and not r['arm'].endswith('forced')})

    @staticmethod
    def ms(r):
        return r.get('median_ms') if r and r['status'] == 'OK' else None

    @staticmethod
    def pushed(r):
        return bool(r) and r['status'] == 'OK' and r['node'] != 'normal'

    def sibling(self, arm, eng):
        return f'pg-{arm.rsplit("-", 1)[1]}' if eng == 'pg' else arm.replace('sift60', eng)

    def pairs(self, arm, eng, suite=None, both_pushed=False):
        """(key, ratio of the time of the other engine to the one of arm, arm record, other record) for the queries both ran."""
        out = []
        for k, d in self.R.items():
            a, o = d.get(arm), d.get(self.sibling(arm, eng))
            if (suite is None or k[0] == suite) and self.ms(a) and self.ms(o) and (not both_pushed or (self.pushed(a) and self.pushed(o))):
                out.append((k, self.ms(o) / self.ms(a), a, o))
        return out

    def capped(self, title, items, head, row, note=''):
        items = sorted(items, key=lambda x: -x[0])
        return f'### {title} ({len(items)})\n\n{note}\n\n' + (md(head, [row(i) for _, i in items[:25]]) if items else 'none\n') + '\n'

    def headline(self):
        rows = []
        for scope in ('coverage', 'plan', None):
            for arm in self.arms:
                rs = [d[arm] for k, d in self.R.items() if arm in d and scope in (None, k[0])]
                ok = [r for r in rs if r['status'] == 'OK']
                pg, v5 = self.pairs(arm, 'pg', scope), self.pairs(arm, 'v510', scope)
                rows.append([scope or 'combined', arm, len(rs), f'{100 * sum(map(self.pushed, rs)) / max(len(ok), 1):.0f}%', geomean([p[1] for p in pg]), geomean([p[1] for p in v5]),
                             f'{sum(p[1] > 1.15 for p in pg)}/{sum(.85 <= p[1] <= 1.15 for p in pg)}/{sum(p[1] < .85 for p in pg)}', sum(r['status'] == 'BAD_RESULT' for r in rs),
                             sum(r['status'] == 'UNSUPPORTED_510' for k, d in self.R.items() for r in [d.get(self.sibling(arm, 'v510'))] if r and scope in (None, k[0]))])
        return '## Headline\n\nGeometric-mean speedup (time of the other arm / time of the arm), pushdown rate of the answered queries, wins/ties/losses against pg (above 1.15x / within 15% / below 0.85x).\n\n' + \
            md(['scope', 'arm', 'queries', 'pushed', 'vs pg', 'vs v5.1.0', 'W/T/L vs pg', 'BAD_RESULT', 'UNSUPPORTED_510 (v5.1.0)'], rows) + '\n'

    def breakdown(self):
        out = '## By area and class\n\n'
        for suite in ('coverage', 'plan'):
            rows = []
            for fam in sorted({r['family'] for r in self.rows if r['suite'] == suite}):
                keys = [k for k in self.R if k[0] == suite and next(iter(self.R[k].values()))['family'] == fam]
                cells = [fam, len(keys)]
                for arm in ('sift60-col-1t', 'sift60-tbl-1t', 'sift60-col-4t'):
                    rs = [self.R[k][arm] for k in keys if arm in self.R[k]]
                    pg = [p[1] for p in self.pairs(arm, 'pg', suite) if p[2]['family'] == fam]
                    cells += [f'{100 * sum(map(self.pushed, rs)) // max(len(rs), 1)}%', geomean(pg)]
                cells.append(geomean([p[1] for p in self.pairs('sift60-col-1t', 'v510', suite, True) if p[2]['family'] == fam]))
                rows.append(cells)
            out += f'### {suite}\n\n' + md(['family', 'queries', 'col-1t pushed', 'col-1t vs pg', 'tbl-1t pushed', 'tbl-1t vs pg', 'col-4t pushed', 'col-4t vs pg', 'col-1t vs v5.1.0 (both pushed)'], rows) + '\n'
        return out

    def parity(self):
        rows, outliers = [], []
        for n in ('1t', '4t'):
            col, tbl = f'sift60-col-{n}', f'sift60-tbl-{n}'
            both = [(k, d[tbl]['median_ms'] / d[col]['median_ms'], d[col], d[tbl]) for k, d in self.R.items() if self.ms(d.get(col)) and self.ms(d.get(tbl))]
            rows.append([n, len(both), geomean([b[1] for b in both])])
            outliers += [(r, (k, n, c['median_ms'], t['median_ms'], c['node'], t['node'])) for k, r, c, t in both if r > 1.2 and t['median_ms'] - c['median_ms'] > 0.2]
        return '## Storage parity (sift_table time / sift_col time)\n\n' + md(['workers', 'queries', 'geomean tbl/col'], rows) + '\n' + \
            self.capped('sift_table trails sift_col by more than 20%', outliers, ['query', 'workers', 'col ms', 'tbl ms', 'col node', 'tbl node'], lambda i: [f'{i[0][0]}/{i[0][1]}', *i[1:]])

    def flags(self):
        bad = [(0, r) for r in self.rows if r['status'] == 'BAD_RESULT']
        lose, miss, reg_a, reg_b, flat, tax = [], [], [], [], [], []
        for arm in self.arms:
            for k, ratio, a, o in self.pairs(arm, 'pg'):
                dins = (a['exec_instr'] or 0) - (o['exec_instr'] or 0)
                if self.pushed(a) and ratio < 1 / 1.2 and (a['median_ms'] - o['median_ms'] > 1.0 or dins > 1e6):
                    lose.append((1 / ratio, (k[1], arm, o['median_ms'], a['median_ms'], a['node'], dins)))
                forced = self.R[k].get(arm + '-forced')
                if a['node'] == 'normal' and self.ms(forced) and self.ms(forced) * 1.5 < a['median_ms']:
                    miss.append((a['median_ms'] / forced['median_ms'], (k[1], arm, a['median_ms'], forced['median_ms'], forced['node'])))
            for k, ratio, a, o in self.pairs(arm, 'v510'):
                dins = (a['exec_instr'] or 0) - (o['exec_instr'] or 0)
                if ratio < 1 / 1.15 and (a['median_ms'] - o['median_ms'] > 0.5 or dins > 1e6) and self.pushed(o):
                    (reg_a if self.pushed(a) else reg_b).append((1 / ratio, (k[1], arm, o['median_ms'], a['median_ms'], o['node'], a['node'])))
            if arm.endswith('-1t'):
                for k, d in self.R.items():
                    t1, t4, r4 = self.ms(d.get(arm)), self.ms(d.get(arm.replace('1t', '4t'))), d.get(arm.replace('1t', '4t'))
                    if t1 and t4 and t1 > 10 and t1 / t4 < 1.3:
                        flat.append((t4 / t1, (k[1], arm, t1, t4, f'{r4["workers_launched"]}/{r4["workers_planned"]}')))
            for k, d in self.R.items():
                a, pg = d.get(arm), d.get('pg-1t')
                if a and pg and a['status'] == 'OK' and pg['status'] == 'OK' and a['plan_instr'] and pg['plan_instr'] and a['plan_instr'] > 1.4 * pg['plan_instr'] and a['plan_instr'] - pg['plan_instr'] > 20000:
                    tax.append((a['plan_instr'] / pg['plan_instr'], (k[1], arm, pg['plan_instr'], a['plan_instr'])))
        out = '## Red flags (worst first, 25 shown)\n\n'
        out += self.capped('BAD_RESULT', bad, ['suite', 'query', 'arm', 'detail'], lambda r: [r['suite'], r['qid'], r['arm'], r['detail'][:200]], 'An answer that differs from the one of pg-1t; must be 0.')
        out += self.capped('LOSE_VS_PG', lose, ['query', 'arm', 'pg ms', 'arm ms', 'node', 'instr delta'], lambda i: list(i), 'A pushdown plan more than 20% slower than pg (and over 1 ms or 1M instructions).')
        out += self.capped('MISS_VS_PG', miss, ['query', 'arm', 'stock ms', 'forced ms', 'forced node'], lambda i: list(i), 'The stock planner kept the normal plan; the forced plan is over 1.5x faster.')
        out += self.capped('REGRESS_VS_510 (a) both pushed', reg_a, ['query', 'arm', 'v5.1.0 ms', '6.0 ms', 'v5.1.0 node', '6.0 node'], lambda i: list(i), 'Slower than v5.1.0 by over 15% (and 0.5 ms or 1M instructions).')
        out += self.capped('REGRESS_VS_510 (b) plan flips', reg_b, ['query', 'arm', 'v5.1.0 ms', '6.0 ms', 'v5.1.0 node', '6.0 node'], lambda i: list(i), 'v5.1.0 pushed down and 6.0 fell back to the normal plan.')
        out += self.capped('PAR_FLAT', flat, ['query', 'arm', '1t ms', '4t ms', 'launched/planned'], lambda i: list(i), 'A 1t time over 10 ms and a 4t speedup below 1.3x.')
        out += self.capped('PLAN_TAX', tax, ['query', 'arm', 'pg plan instr', 'arm plan instr'], lambda i: list(i), 'Planning instructions over 1.4x pg-1t and 20,000 more.')
        wins = [(r, (k[1], arm, a['median_ms'], o['median_ms'], round(r, 1), a['node'], o['node'])) for arm in self.arms for k, r, a, o in self.pairs(arm, 'v510') if r > 1]
        return out + '## NEW_WINS_VS_510 (top 20)\n\n' + md(['query', 'arm', '6.0 ms', 'v5.1.0 ms', 'x', '6.0 node', 'v5.1.0 node'], [i for _, i in sorted(wins, key=lambda x: -x[0])[:20]])

    def write(self, path):
        open(path, 'w').write(f'# Coverage and plan-gate benchmark\n\n{self.notes}\n' + self.headline() + self.breakdown() + self.parity() + self.flags())


def host_cpu():
    return next((l.split(':', 1)[1].strip() for l in open('/proc/cpuinfo') if l.startswith('model name')), 'unknown')


def build(args, work, run_env):
    """Release libraries of this tree (scripts/pgtest.sh install) and of v5.1.0 (scripts/build-v510-fixture.sh), each staged in a root of the run."""
    tmp6 = f'{work}/build60'
    subprocess.run([f'{REPO}/scripts/pgtest.sh', 'install'], check=True, env={**run_env, 'SIFT_TMP': tmp6, 'SIFT_TESTING': ''}, stdout=sys.stderr)
    roots = {'v60': f'{tmp6}/root'}
    if not args.no_v510:
        src = find_v510(args.v510_src)
        lib = os.environ.get('SIFT_V510_LIBDIR') or f'{work}/v510-lib'
        subprocess.run([f'{REPO}/scripts/build-v510-fixture.sh', lib], check=True, env={**run_env, 'SIFT_V510_SRC': src}, stdout=sys.stderr)
        roots['v510'] = lib
        roots['v510_sha'] = sh(['git', '-C', src, 'rev-parse', '--short', 'v5.1.0^{commit}']).stdout.strip()
    return roots


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rows', type=int, default=250000)
    ap.add_argument('--suites', default='coverage,plan')
    ap.add_argument('--storage', default='col,table')
    ap.add_argument('--workers', default='1,4')
    ap.add_argument('--no-v510', action='store_true')
    ap.add_argument('--v510-src')
    ap.add_argument('--filter')
    ap.add_argument('--reps', type=int, default=5)
    ap.add_argument('--plan-only', action='store_true')
    ap.add_argument('--no-lock', action='store_true')
    ap.add_argument('--port', type=int, default=56290)
    ap.add_argument('--out')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.no_lock and not args.plan_only:
        sys.exit('cov-plan: --no-lock is for --plan-only runs: a wall clock needs the quiet host')
    sha = sh(['git', '-C', REPO, 'rev-parse', '--short', 'HEAD']).stdout.strip() + ('+dirty' if sh(['git', '-C', REPO, 'status', '--porcelain', '-uno']).stdout.strip() else '')
    out = args.out or os.path.join(REPO, 'bench', 'out', f'cov-plan-{sha.split("+")[0]}-{args.rows}')
    work = os.path.join(os.environ.get('SIFT_TMP', '/tmp/sift-cov-plan'), f'cov-plan-{os.getpid()}')
    os.makedirs(out, exist_ok=True)
    os.makedirs(work)
    clusters, sessions = [], []

    def cleanup():
        for s in sessions:
            s.p.kill()
        for c in clusters:
            c.stop()
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
    atexit.register(cleanup)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    roots = build(args, work, dict(os.environ))
    c60 = Cluster('v60', args.port, work, roots['v60'])
    c510 = None if args.no_v510 else Cluster('v510', args.port + 1, work, roots['v510'])
    storage, workers = args.storage.split(','), sorted({1} | set(map(int, args.workers.split(','))))
    arms = [Arm('pg', 'col', n, c60) for n in workers] + [Arm(e, s, n, c) for e, c in (('v510', c510), ('sift60', c60)) if c for s in storage for n in workers]
    queries = {'coverage': load_coverage()[0], 'plan': load_plan()}
    queries = {s: [q for q in queries[s] if not args.filter or re.search(args.filter, q.qid)] for s in args.suites.split(',')}
    for c in filter(None, (c60, c510)):
        clusters.append(c)
        c.start()
        for suite in [s for s in queries if queries[s]]:
            build_dbs(c, suite, args.rows, storage, c is c60)
    lock = None
    if not args.plan_only and not args.no_lock:
        lock = open(LOCK, 'w')
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print(f'cov-plan: waiting for {LOCK}', file=sys.stderr)
            fcntl.flock(lock, fcntl.LOCK_EX)
    perf, rows, excluded = Perf(work), [], load_coverage()[1]
    for suite, qs in queries.items():
        S, caches = {a.name: Session(a.cl, a.db(suite)) for a in arms}, collections.defaultdict(dict)
        sessions.extend(S.values())
        for i, q in enumerate(qs, 1):
            print(f'[{suite} {i}/{len(qs)}] {q.qid}', file=sys.stderr)
            try:
                recs = run_query(q, arms, S, args, perf, caches)
            except ConnectionError:
                for c in clusters:
                    c.wait_ready()
                recs = {a.name: record(q, a.name, 'CRASH', detail='the backend died') for a in arms}
                S = {a.name: Session(a.cl, a.db(suite)) for a in arms}
                sessions.extend(S.values())
            rows += recs.values()
        for s in S.values():
            s.close()
    cols = lambda r: '\t'.join('-' if r.get(c) is None else f'{r[c]:.3f}' if isinstance(r[c], float) else re.sub(r'\s+', ' ', str(r[c])) for c in RAW_COLS)
    open(f'{out}/raw.tsv', 'w').write('\t'.join(RAW_COLS) + '\n' + ''.join(cols(r) + '\n' for r in rows))
    meta = {'git_sha': sha, 'v510_sha': roots.get('v510_sha', '-'), 'pg_version': sh(['pg_config', '--version']).stdout.strip(), 'rows': args.rows, 'seed': 'coverage 0, plan 1',
            'host_cpu': host_cpu(), 'lock_held': 'yes' if lock else 'no', 'perf': 'yes' if perf.ok else 'no (NA)', 'suites': args.suites, 'storage': args.storage, 'workers': ','.join(map(str, workers)),
            'filter': args.filter or '-', 'reps_max': args.reps, 'plan_only': args.plan_only, 'arms': ' '.join(a.name for a in arms),
            'ddl_errors': '; '.join(f'{c.name}/{db}: {len(w)}' for c in clusters for db, w in c.warnings.items() if w) or '-'}
    open(f'{out}/manifest.tsv', 'w').write(''.join(f'{k}\t{v}\n' for k, v in meta.items()) + ''.join(f'excluded\t{q}\t{r}\n' for q, r in excluded))
    Report(rows, '\n'.join(f'- {k}: {v}' for k, v in meta.items()) + f'\n- excluded: {", ".join(f"{q} ({r})" for q, r in excluded)}\n').write(f'{out}/summary.md')
    print(f'cov-plan: {len(rows)} records in {out}', file=sys.stderr)
    bad = sum(r['status'] == 'BAD_RESULT' for r in rows)
    sys.exit(3 if bad else 0)


if __name__ == '__main__':
    main()
