# Citus (one node, the release image of the store benchmark): the tables are its `columnar` access method (arm `citus`) and, for the suites that have a bench/sql/SUITE/citus.sql, the same rows as
# heap tables distributed over 4 shards of the one node (arm `citus_dist`, schema dist). A case runs its pg statement.
import os

from suite_engine import SQL, PgServer


class Engine(PgServer):
    IMAGE = 'STORE_CITUS_IMAGE'
    TABLE_OPTIONS = 'USING columnar'
    RUN_SETUP = False

    def __init__(self, ctx):
        super().__init__(ctx)
        self.dist = set()   # the suites with distributed tables

    def arms(self, case):
        return ['citus'] + ['citus_dist'] * (case.dataset in self.dist)

    def server_args(self):
        return ['-c', 'shared_preload_libraries=citus', '-c', 'citus.shard_count=4', '-c', 'citus.enable_repartition_joins=on']

    def extension_sql(self):
        return 'CREATE EXTENSION IF NOT EXISTS citus_columnar; CREATE EXTENSION IF NOT EXISTS citus'

    def arm_settings(self, arm):
        return ['SET search_path TO dist, public'] * (arm == 'citus_dist')

    def load(self, suite, work):
        super().load(suite, work)
        path = os.path.join(SQL, suite, 'citus.sql')
        if not os.path.exists(path):
            return
        if not self.dist:
            self.psql('bench', "SELECT citus_set_coordinator_host('localhost', 5432); SELECT citus_set_node_property('localhost', 5432, 'shouldhaveshards', true); CREATE SCHEMA dist")
        self.dist.add(suite)
        for table, cols in self.schemas[suite].items():
            self.psql('bench', 'CREATE TABLE dist.%s (%s)' % (table, ', '.join(' '.join(c) for c in cols)))
        for stmt in [s for s in open(path).read().split(';\n') if s.strip()]:
            self.psql('bench', stmt)
        for table in self.schemas[suite]:
            self.psql('bench', 'INSERT INTO dist.%s SELECT * FROM public.%s' % (table, table))
        self.psql('bench', 'ANALYZE')
