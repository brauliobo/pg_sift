-- The merges after a parallel build of a scope index end. The rewrite is due for the scope segments (the runs of the participants each hold every scope key); an index without key_cut
-- (a table clustered by its key) has no end state of its own for the entity segments, and the merge, whose writer cuts its output at the memory it was given (maintenance_work_mem 1 MB: a budget
-- of 16 MB), wrote as many segments as it read and was chosen again, for each of the 128 steps of the rewrite.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 2;
SET min_parallel_table_scan_size = 0;
SET maintenance_work_mem = '1MB';

CREATE TABLE facts (id int PRIMARY KEY, member int NOT NULL, d date NOT NULL, v float8 NOT NULL) WITH (autovacuum_enabled = false, parallel_workers = 2);
INSERT INTO facts SELECT g, 1 + (g - 1) * 400 / 400000, date '2025-01-01' + g % 700, g % 100 FROM generate_series(1, 400000) g;
VACUUM (ANALYZE, FREEZE) facts;
CREATE TABLE pairs (member int NOT NULL, tenant int NOT NULL);
INSERT INTO pairs SELECT m, 1 + (m * 7) % 40 FROM generate_series(1, 400) m;
CREATE UNIQUE INDEX pairs_uniq ON pairs (member, tenant);
CREATE INDEX pairs_journal ON pairs USING sift_col (member, tenant) WITH (track_changes = 'ctid');

CREATE INDEX facts_col ON facts USING sift_col (id, member, d, v) WITH (scope = 'tenant: member -> pairs(member, tenant)');
SELECT max(gen) < 64 AS merges_end, count(*) > 1 AS cut_by_memory FROM sift.col_segments('facts_col') WHERE kind = 'immutable';
-- the stale connection the build leaves is refreshed by one more pass, and the next passes find nothing to do (the refresh does not come back: a rewrite restamps what was stale)
DO $$ DECLARE n int := 0; BEGIN WHILE sift.col_maintain('facts_col') LOOP n := n + 1; IF n > 4 THEN RAISE EXCEPTION 'the maintenance of facts_col does not end'; END IF; END LOOP; END $$;
SELECT max(gen) < 64 AS still_ends FROM sift.col_segments('facts_col') WHERE kind = 'immutable';
SELECT sift.col_verify('facts_col', true);

DROP TABLE facts, pairs;
DROP EXTENSION pg_sift;
