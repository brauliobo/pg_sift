# ClickHouse (one container, HTTP): the tables are bench/sql/SUITE/ch.sql, loaded from the CSV of the source; a case runs its pg statement unless it has a `--@ ch` section.
import http.client
import os
import time
from urllib.parse import quote

from suite_engine import SQL, Server, Unsupported, image, sh  # noqa: F401


class Engine(Server):
    section = 'ch'

    def __init__(self, ctx):
        super().__init__(ctx)
        self.conn = None

    def up(self):
        self.docker_run(8123, image('STORE_CH_IMAGE'), env=['CLICKHOUSE_USER=bench', 'CLICKHOUSE_PASSWORD=bench'], run_args=['--ulimit', 'nofile=262144:262144'])
        for _ in range(120):
            try:
                if self.query('SELECT 1', db='default').strip() == '1':
                    break
            except (OSError, RuntimeError, http.client.HTTPException):
                self.conn = None
            time.sleep(1)
        else:
            raise SystemExit('generic: ClickHouse did not start (docker logs %s)' % self.name)
        self.query('CREATE DATABASE IF NOT EXISTS bench', db='default')

    def query(self, sql, body=None, db='bench'):
        """the response text; sql travels in the URL when a body is sent (an INSERT), else it is the body"""
        for attempt in (0, 1):
            try:
                if self.conn is None:
                    self.conn = http.client.HTTPConnection('127.0.0.1', self.ctx.port, timeout=600)
                path = '/?database=%s&default_format=TabSeparated&max_threads=%d&date_time_input_format=best_effort&aggregate_functions_null_for_empty=1' % (db, self.ctx.cpus)
                headers = {'X-ClickHouse-User': 'bench', 'X-ClickHouse-Key': 'bench'}
                if body is None:
                    self.conn.request('POST', path, body=sql.encode(), headers=headers)
                else:
                    self.conn.request('POST', path + '&query=' + quote(sql, safe=''), body=body, headers=headers)
                r = self.conn.getresponse()
                data = r.read().decode(errors='replace')
                if r.status >= 400:
                    raise RuntimeError('ClickHouse %d: %s' % (r.status, data[:300]))
                return data
            except (http.client.HTTPException, OSError) as e:
                self.conn = None
                if attempt:
                    raise RuntimeError(str(e))

    def load(self, suite, work):
        for stmt in [s for s in open(os.path.join(SQL, suite, 'ch.sql')).read().split(';\n') if s.strip()]:
            self.query(stmt)
        for table in [os.path.splitext(f)[0] for f in sorted(os.listdir(os.path.join(work, suite))) if f.endswith('.csv')]:
            with open(os.path.join(work, suite, table + '.csv'), 'rb') as f:
                self.query('INSERT INTO %s FORMAT CSV' % table, body=f.read())

    def unload(self, suite, work):
        for f in sorted(os.listdir(os.path.join(work, suite))):
            if f.endswith('.csv'):
                self.query('DROP TABLE IF EXISTS %s' % os.path.splitext(f)[0])

    def runner(self, arm, sql):
        def go():
            t = time.perf_counter()
            try:
                text = self.query(sql)
            except RuntimeError as e:
                raise Unsupported('error: ' + str(e)[:200])
            return (time.perf_counter() - t) * 1000, [['' if c == '\\N' else c for c in l.split('\t')] for l in text.split('\n') if l != '']
        return go
