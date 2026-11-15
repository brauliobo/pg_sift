# A psql process driven statement by statement, and the databases of a run.

import os
import select
import subprocess
import sys
import time

TMP = os.environ.get('SIFT_TMP', '/tmp/sift-arch')
ENV = dict(os.environ, PGHOST=f'{TMP}/sock', PGPORT=os.environ.get('SIFT_PORT', '56100'), PGUSER='postgres',
           PGOPTIONS='-c client_min_messages=warning -c lc_messages=C')
READ_TIMEOUT = 90.0


class Hang(Exception):
    pass


class Session:
    """psql -At on a pipe: run(sql) sends the text and a sentinel, and returns the lines before it (rows, then the errors the text raised)."""

    def __init__(self, db):
        self.p = subprocess.Popen(['psql', '-X', '-q', '-At', '-d', db, '-v', 'ON_ERROR_STOP=0'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1, env=ENV)
        self.fd, self.buf, self.n = self.p.stdout.fileno(), b'', 0
        self.run("SET statement_timeout = '40s'; SET lock_timeout = '300ms'")

    def run(self, sql):
        self.n += 1
        tag = f'@@END {self.n}'
        sql = sql.rstrip()
        self.p.stdin.write(sql + ('' if sql.endswith((';', '\\.')) else ';') + f'\n\\echo {tag}\n')
        self.p.stdin.flush()
        out = []
        end = time.time() + READ_TIMEOUT
        while True:
            while b'\n' not in self.buf:
                left = end - time.time()
                if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                    self.kill()
                    raise Hang(sql[:300])
                data = os.read(self.fd, 65536)
                if not data:
                    raise Hang('psql ended: ' + sql[:300])
                self.buf += data
            line, _, self.buf = self.buf.partition(b'\n')
            line = line.decode(errors='replace')
            if line == tag:
                return out
            out.append(line)

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=20)
        except Exception:
            self.kill()

    def kill(self):
        self.p.kill()
        self.p.wait()


def errors(lines):
    return [l for l in lines if l.startswith(('ERROR:', 'FATAL:', 'PANIC:'))]


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


def drop_db(*names):
    psql('postgres', ''.join(f'DROP DATABASE IF EXISTS {n} WITH (FORCE);' for n in names), check=False)
