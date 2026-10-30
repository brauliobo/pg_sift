# The server side of the fuzz: psql, the databases and the data a seed builds.
# The settings of the reference and of the base come with it.

import os
import re
import subprocess
import sys
import time

from . import tablemode

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(os.path.dirname(HERE))
TMP = os.environ.get('SIFT_TMP', '/tmp/sift-arch')
ENV = dict(os.environ, PGHOST=f'{TMP}/sock', PGPORT=os.environ.get('SIFT_PORT', '56100'), PGUSER='postgres',
           PGOPTIONS='-c client_min_messages=warning')

BASE = 'seq_page_cost=1000;random_page_cost=1000;sift.agg_left_lateral=on'
REF = 'sift.enable_agg=off;max_parallel_workers_per_gather=0'


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
    t = open(f'{HERE}/diff_fuzz_data.sql').read()
    for mode, keep in (('table', tablemode.TABLE), ('heap', not tablemode.TABLE)):
        t = re.sub(rf'^-- @{mode}\n(.*?)^-- @end\n', (lambda m: m.group(1)) if keep else '', t, flags=re.S | re.M)
    if tablemode.TABLE:
        t = re.sub(r'^CREATE INDEX \w+ ON \w+ USING sift_col.*?;\n', '', t, flags=re.S | re.M)
    return t.replace('@SEED@', str(seed)).replace('@ROWS@', str(rows)).replace('@PARTS@', str(parts))


def build_base(name, seed, rows, parts):
    create_db(name)
    psql(name, open(f'{REPO}/test/col/run.sql').read() + data_sql(seed, rows, parts) + open(f'{HERE}/diff_fuzz.sql').read())
