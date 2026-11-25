# The server side of the fuzz: psql, the databases and the data a seed builds.
# The settings of the reference and of the base come with it.

import os
import re
import subprocess
import sys
import time

import fmt
from . import tablemode

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
TMP = os.environ.get('SIFT_TMP', '/tmp/sift-arch')
CLIENT_OPTIONS = f"-c client_min_messages=warning {os.environ.get('PGOPTIONS', '')}"
ENV = dict(os.environ, PGHOST=f'{TMP}/sock', PGPORT=os.environ.get('SIFT_PORT', '56100'), PGUSER='postgres', PGOPTIONS=CLIENT_OPTIONS)
LIB = None   # the library of the cluster, probed by configure

BASE = 'seq_page_cost=1000;random_page_cost=1000;sift.agg_left_lateral=on'
REF = 'sift.enable_agg=off;max_parallel_workers_per_gather=0'
# the settings of the test library that act on Format 1 only (the sorted runs, the unit floors, the heap of the top-N, the scope patch): Format 2.0 and the release library do not run them
V1_ONLY = frozenset(f'sift.{n}' for n in ('test_limits', 'test_agg_dead_delta', 'test_topn_gorder', 'test_scope_patch', 'test_agg_two_level', 'test_agg_dense', 'test_child_warm',
                                          'test_col_floor_sbs', 'test_col_unit_floor', 'test_col_unit_rows', 'test_col_gorder_ends', 'agg_left_lateral'))


def configure(name, offline=False):
    """probes the library once and makes every psql of the run start in the format; offline: no server (a selftest), every setting exists"""
    global LIB
    LIB = fmt.Lib.offline(name) if offline else fmt.Lib(name, env=ENV)
    ENV['PGOPTIONS'] = f'{CLIENT_OPTIONS} {LIB.pgoptions}'.strip()


def knob(name):
    """the setting exists in the library and applies to its format"""
    return LIB.has(name) and (name not in V1_ONLY or LIB.v1_hooks)


def sets(*pairs):
    """the SET statements of the (name, value) pairs that the library takes"""
    return [f'SET {n} = {v}' for n, v in pairs if knob(n)]


def usable(spec):
    """a 'name=value;name=value' spec of which every setting the library takes"""
    return all(knob(kv.split('=')[0]) for kv in spec.split(';') if kv)


def settings(spec):
    """spec without the settings the library does not take (BASE and REF name some of the test library)"""
    return ';'.join(kv for kv in spec.split(';') if kv and knob(kv.split('=')[0]))


# an f-string expression of Python before 3.12 cannot hold a backslash
COLLATE_C = ' COLLATE "C"'

# ---------- psql ----------
def psql(db, text, check=True):
    for _ in range(120):
        p = subprocess.run(['psql', '-X', '-q', '-At', '-d', db, '-v', 'ON_ERROR_STOP=%d' % check], input=text, capture_output=True, text=True, env=ENV)
        if 'not yet accepting connections' not in p.stderr and 'the database system is' not in p.stderr:
            break
        time.sleep(1)
    if check and p.returncode:
        sys.exit(f'psql {db} failed: {p.stderr.strip()[:2000]}')
    return p.stdout, p.stderr


def create_db(name, template=None):
    psql('postgres', f'DROP DATABASE IF EXISTS {name} WITH (FORCE); CREATE DATABASE {name}' + (f' TEMPLATE {template}' if template else ''))


def data_sql(seed, rows, parts):
    t = open(f'{HERE}/data.sql').read()
    for mode, keep in (('table', tablemode.TABLE), ('heap', not tablemode.TABLE)):
        t = re.sub(rf'^-- @{mode}\n(.*?)^-- @end\n', (lambda m: m.group(1)) if keep else '', t, flags=re.S | re.M)
    t = re.sub(r'^(?:SET|RESET) (sift\.\w+)\b[^\n]*\n', lambda m: m.group(0) if knob(m.group(1)) else '', t, flags=re.M)    # the settings of the test library that this library or format lacks
    if tablemode.TABLE:
        t = re.sub(r'^CREATE INDEX \w+ ON \w+ USING sift_col.*?;\n', '', t, flags=re.S | re.M)
    return t.replace('@SEED@', str(seed)).replace('@ROWS@', str(rows)).replace('@PARTS@', str(parts))


def build_base(name, seed, rows, parts):
    create_db(name)
    psql(name, open(f'{REPO}/test/col/run.sql').read() + data_sql(seed, rows, parts) + open(f'{HERE}/schema.sql').read())
