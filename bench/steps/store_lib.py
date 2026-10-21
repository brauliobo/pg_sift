"""The store benchmark's settings: the lanes (engines), ports, the SQL files and the questions (steps/store.py)."""
import os
import re
import subprocess
import sys
import time
HERE = os.path.dirname(os.path.abspath(__file__))


SQL = os.path.join(HERE, '..', 'sql', 'store')


NAME = os.environ.get('STORE_NAME', 'sift-store')


PORT = int(os.environ.get('STORE_PORT', 56280))


START_LOAD = open('/proc/loadavg').read().split()[:3]     # the load average when this command started: only recorded, for the older tables


QUIET = os.path.join(HERE, '..', '..', 'scripts', 'quiet.sh')


NULL = '\\N'


TOL = 1e-9                      # relative tolerance of numbers (sums of money are exact in all engines; averages differ in the last digits)


CH_AUTH = {'X-ClickHouse-User': 'bench', 'X-ClickHouse-Key': 'bench'}


def quiet(*args):
    """scripts/quiet.sh COMMAND: its stdout, stripped; the exit status is the helper's (0 quiet, 1 timeout, 3 no PSI)"""
    p = subprocess.run(['bash', QUIET, *args], capture_output=True, text=True)
    return p.returncode, p.stdout.strip() or p.stderr.strip()


START_PRESSURE = quiet('report')[1]                       # the pressure when this command started


NOISY = []                                                # the cells whose window (still, after a second measurement) had other work in it: Window.judge


COMPRESSION = {'sift_none': 'none', 'sift_enc': 'for+delta+rle+dict+scale+alp', 'sift_auto': 'auto', 'sift_lz4': 'lz4', 'sift_zstd': 'zstd:3'}   # the `compression` option of the sift_col index, one database each


# id -> (label, container, port offset, database or kind)
LANES = {
    'pg':         ('PostgreSQL', 'pg', 0, 'store'),
    'sift':       ('pg_sift `sift_col`', 'sift', 1, 'store_col'),
    'sift_table': ('pg_sift `sift_table`', 'sift', 1, 'store_tbl'),
    'es':         ('Elasticsearch', 'es', 2, None),
    'es_cached':  ('Elasticsearch, request cache on', 'es', 2, None),
    'ch':         ('ClickHouse', 'ch', 3, None),
    'ch_mut':     ('ClickHouse, ALTER UPDATE', 'ch', 3, None),
    'ch_rmt':     ('ClickHouse, ReplacingMergeTree', 'ch', 3, None),
    'citus':      ('Citus columnar', 'citus', 4, 'store'),
    **{e: (f'pg_sift `sift_col`, compression `{p}`', 'sift', 1, 'store_col_' + e[5:]) for e, p in COMPRESSION.items()},
}


DEFAULT_ENGINES = ['pg', 'sift', 'sift_table', 'es', 'es_cached', 'ch', 'citus']


PG_ENGINES = ['pg', 'sift', 'sift_table', 'citus', *COMPRESSION]


UPDATE_ENGINES = ['pg', 'sift', 'sift_table', 'es', 'ch', 'ch_mut', 'ch_rmt']


CH_TABLE = {'ch': 'store.orders', 'ch_mut': 'store.orders_mut', 'ch_rmt': 'store.orders_rmt'}


is_ch = lambda eid: eid in CH_TABLE


is_es = lambda eid: eid in ('es', 'es_cached')


DDL = {'pg': 'pg.sql', 'sift': 'sift.sql', 'sift_table': 'sift_table.sql', 'citus': 'citus.sql', **{e: 'sift.sql' for e in COMPRESSION}}


def die(msg):
    sys.exit(f'store: {msg}')


def log(msg):
    print(f'[{time.strftime("%T")}] {msg}', file=sys.stderr, flush=True)


def container(eid):
    return f'{NAME}-{LANES[eid][1]}'


def port(eid):
    return PORT + LANES[eid][2]


def sections(text):
    """`--@ name` lines cut a file into sections (text before the first one is the header)"""
    out, cur = {'': []}, ''
    for line in text.split('\n'):
        m = re.match(r'--@ (\w+)$', line)
        if m:
            cur = m.group(1)
            out[cur] = []
        else:
            out[cur].append(line)
    return {k: '\n'.join(v).strip() for k, v in out.items()}


def read(name):
    return open(os.path.join(SQL, name)).read()


class Query:
    def __init__(self, path):
        self.id, self.name = os.path.basename(path)[:-4].split('_', 1)
        text = read(os.path.join('queries', os.path.basename(path)))
        self.sec = sections(text)
        head = self.sec.pop('').split('\n')
        self.title = head[0][3:]
        self.label = next((m[10:] for m in head if m.startswith('-- label: ')), self.title)
        self.key = next(([int(k) - 1 for k in m[8:].split()] for m in head if m.startswith('-- key: ')), [])

    def statement(self, eid, n, cache=False):
        """the text sent to engine eid: SQL, or for Elasticsearch (method, path, body, rows spec, tolerance)"""
        sub = lambda s: s.replace('{point_id}', str(n * 42 // 100)).replace('{rc}', 'true' if cache else 'false')
        if is_es(eid):
            lines = sub(self.sec['es']).split('\n')
            method, path = lines[0].split()
            spec = [l for l in lines if l.startswith('rows:')][0][5:].strip()
            tol = [float(l[4:]) for l in lines if l.startswith('tol:')]
            body = '\n'.join(l for l in lines[1:] if not l.startswith(('rows:', 'tol:'))).strip()
            return method, path, body.encode() if body else None, spec, tol[0] if tol else TOL
        family = 'ch' if is_ch(eid) else 'sift' if eid.startswith('sift') else eid   # a `--@ sift` or `--@ citus` section replaces the PostgreSQL text for that system
        sql = sub(self.sec[family if family in self.sec else 'pg'])
        return sql.replace('store.orders', CH_TABLE[eid] + (' FINAL' if eid == 'ch_rmt' else '')) if is_ch(eid) else sql


def load_queries(only=None):
    names = sorted(f for f in os.listdir(os.path.join(SQL, 'queries')) if f.endswith('.sql'))
    qs = [Query(os.path.join(SQL, 'queries', f)) for f in names]
    return [q for q in qs if not only or q.id in only or q.name in only]


CHUNK = 1000000


LOAD_QUERIES = ['03', '05', '09']        # the reads timed before, during and after the churn


LOAD_QUERY = '03'
