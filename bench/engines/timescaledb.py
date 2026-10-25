# TimescaleDB (the community image, PostgreSQL 18): a table with a `ts` column is a hypertable (bench/sql/SUITE/timescaledb.sql, before the load), the others plain heaps; a case runs its pg
# statement (arm `timescaledb`). A case with a `--@ timescaledb_cagg` section also runs on the continuous aggregate of bench/sql/SUITE/timescaledb_cagg.sql (arm `timescaledb_cagg`, refreshed
# after the load): the way TimescaleDB is used for a dashboard, not the same table scan.
import os

from suite_engine import SQL, PgServer


class Engine(PgServer):
    IMAGE = 'STORE_TIMESCALE_IMAGE'

    def arms(self, case):
        return ['timescaledb'] + ['timescaledb_cagg'] * ('timescaledb_cagg' in case.sec)

    def server_args(self):
        return ['-c', 'shared_preload_libraries=timescaledb', '-c', 'timescaledb.telemetry_level=off']

    def extension_sql(self):
        return 'CREATE EXTENSION IF NOT EXISTS timescaledb'

    def load(self, suite, work):
        super().load(suite, work)
        path = os.path.join(SQL, suite, 'timescaledb_cagg.sql')
        if os.path.exists(path):
            for stmt in [s for s in open(path).read().split(';\n') if s.strip()]:
                self.psql('bench', stmt)
