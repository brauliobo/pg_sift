# pg_duckdb on PostgreSQL 18 (the release image of the duckdb benchmark): the source of the suites. It generates them with bench/sql/SUITE/gen.sql (the pg_sift index lines left out), answers with
# PostgreSQL's own plan (the arm `pg`, the reference of every other engine) and with duckdb.force_execution = on (the arm `pg_duckdb`), and exports the tables and their schema to CSV and JSON.
import json
import os
import subprocess
import sys

from suite_engine import SQL, PgServer, Unsupported, image, log, session_run, sh  # noqa: F401  (log: the adapters share the import line)

SOURCE = True
PREPEND = {'orders': ['store/gen.sql']}   # the suite is made on top of another suite's file


class Engine(PgServer):
    IMAGE = 'STORE_DUCKDB_IMAGE'
    section = 'duck'

    def arms(self, case):
        return ['pg', 'pg_duckdb']

    def server_args(self):
        c = self.ctx.cpus
        return ['-c', 'duckdb.threads=%d' % c, '-c', 'duckdb.threads_for_postgres_scan=%d' % c, '-c', 'duckdb.max_workers_per_postgres_scan=%d' % (c - 1)]

    def extension_sql(self):
        return 'CREATE EXTENSION IF NOT EXISTS pg_duckdb'

    def statement(self, case, arm):
        return case.sec['pg'] if arm == 'pg' else super().statement(case, arm)

    def sess(self, arm):
        if arm not in self.sessions:
            self.sessions[arm] = self.session(arm, 'on' if arm == 'pg_duckdb' else None)
        return self.sessions[arm]

    def runner(self, arm, sql):
        return session_run(self.sess(arm), sql)

    def precheck(self, arm, sql):
        if arm == 'pg_duckdb' and 'DuckDBScan' not in '\n'.join(self.sess(arm).run('EXPLAIN ' + sql)[1]):
            return 'it runs on the PostgreSQL executor (no DuckDBScan in the plan)'

    def tables(self):
        return self.psql('bench', "SELECT relname FROM pg_class WHERE relnamespace = 'public'::regnamespace AND relkind = 'r'").split()

    def generate(self, suite, rows, work):
        """the suite in the server, its tables as CSV and the schema (column and type) of each in work/SUITE"""
        before = self.tables()
        gen = ''.join(l for f in PREPEND.get(suite, []) + [suite + '/gen.sql'] for l in open(os.path.join(SQL, f)) if 'USING sift_col' not in l)
        p = subprocess.run(['psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', '-v', 'n=%d' % rows, '-h', '127.0.0.1', '-p', str(self.ctx.port), '-U', 'postgres', '-d', 'bench', '-f', '-'],
                           input=gen, capture_output=True, text=True, env={**os.environ, 'PGOPTIONS': '-c client_min_messages=warning'})
        if p.returncode:
            sys.exit('generic: generating %s failed: %s' % (suite, p.stderr.strip()[:400]))
        wd = os.path.join(work, suite)
        os.makedirs(wd, exist_ok=True)
        schema = {}
        for table in sorted(set(self.tables()) - set(before)):
            cols = [c.split('|') for c in self.psql('bench', "SELECT attname || '|' || format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'public.%s'::regclass AND attnum > 0 "
                                                      "AND NOT attisdropped ORDER BY attnum" % table).split('\n')]
            schema[table] = cols
            with open(os.path.join(wd, table + '.csv'), 'w') as f:
                subprocess.run(['psql', '-X', '-q', '-h', '127.0.0.1', '-p', str(self.ctx.port), '-U', 'postgres', '-d', 'bench', '-c',
                                'COPY (SELECT %s FROM %s) TO STDOUT CSV' % (', '.join(c[0] for c in cols), table)], stdout=f, check=True)
        json.dump(schema, open(os.path.join(wd, 'schema.json'), 'w'))
        self.schemas[suite] = schema

    def load(self, suite, work):
        raise NotImplementedError('the source generates its suites')
