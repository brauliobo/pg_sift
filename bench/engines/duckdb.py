# DuckDB itself (the command line client of the release pg_duckdb embeds, in-memory tables loaded from the CSV of the source): the same engine as the arm `pg_duckdb` without the PostgreSQL executor,
# its heap tables and its protocol around it. A case runs its pg statement (DuckDB's dialect is close), or its `--@ duck` one.
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import urllib.request
import zipfile

from suite_engine import REPO, Server, Unsupported, log, sh

ERROR = re.compile(r'^(?:[A-Z][A-Za-z ]* )?Error:')


def tool(work):
    """the pinned command line client in work/tools, downloaded once and checked against the sha256 of bench/versions.env"""
    version, sha = sh('bash', '-c', '. %s/bench/versions.env; echo "$DUCKDB_CLI_VERSION $DUCKDB_CLI_SHA256"' % REPO).split()
    path = os.path.join(work, 'tools', 'duckdb-' + version)
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        zpath = path + '.zip'
        log('downloading the DuckDB %s command line client' % version)
        urllib.request.urlretrieve('https://github.com/duckdb/duckdb/releases/download/v%s/duckdb_cli-linux-amd64.zip' % version, zpath)
        if hashlib.sha256(open(zpath, 'rb').read()).hexdigest() != sha:
            sys.exit('generic: %s does not match DUCKDB_CLI_SHA256 of bench/versions.env' % zpath)
        zipfile.ZipFile(zpath).extract('duckdb', os.path.dirname(path))
        os.rename(os.path.join(os.path.dirname(path), 'duckdb'), path)
        os.chmod(path, 0o755)
    return path


class Engine(Server):
    section = 'duck'

    def up(self):
        self.p = subprocess.Popen([tool(self.ctx.work), '-noheader', '-list', '-separator', '|', '-nullvalue', '', ':memory:'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.execute("SET threads = %d; SET memory_limit = '%s'" % (self.ctx.cpus, self.ctx.memory.upper().replace('G', 'GB')))

    def execute(self, sql):
        """(milliseconds from the write to the end marker, the rows, the error line) of a statement"""
        self.p.stdin.write('%s;\nSELECT \'__END__\';\n' % sql.rstrip(';'))
        t = time.perf_counter()
        self.p.stdin.flush()
        lines = []
        for line in self.p.stdout:
            if line.strip() == '__END__':
                break
            lines.append(line.rstrip('\n'))
        else:
            sys.exit('generic: the DuckDB client ended')
        ms = (time.perf_counter() - t) * 1000   # the client's timer has a millisecond; this is the clock of the other engines' clients
        err = next((l for l in lines if ERROR.match(l)), '')
        return ms, [l for l in lines if not ERROR.match(l)], err

    def load(self, suite, work):
        for table, cols in json.load(open(os.path.join(work, suite, 'schema.json'))).items():
            for sql in ('CREATE TABLE %s (%s)' % (table, ', '.join(' '.join(c) for c in cols)),
                        "COPY %s FROM '%s' (FORMAT csv, nullstr '\\N')" % (table, os.path.join(work, suite, table + '.csv'))):
                err = self.execute(sql)[2]
                if err:
                    sys.exit('generic: DuckDB: loading %s: %s' % (table, err))

    def unload(self, suite, work):
        for table in json.load(open(os.path.join(work, suite, 'schema.json'))):
            self.execute('DROP TABLE IF EXISTS %s' % table)

    def runner(self, arm, sql):
        def go():
            ms, rows, err = self.execute(sql)
            if err:
                raise Unsupported('DuckDB: ' + err[:200])
            return ms, [r.split('|') for r in rows]
        return go

    def down(self):
        self.p.stdin.close()
        self.p.wait()
