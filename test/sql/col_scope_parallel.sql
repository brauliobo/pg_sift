-- CREATE INDEX of an index with a scope by the participants of a parallel build (each scans its superblocks and writes entity segments and scope runs of its own, the leader publishes
-- all and merges the scope runs by the range of their key), and on a partitioned table (each partition builds its own scope segments from the shared scope relation). After each build
-- the scope read is the plain join (md5) from the scope segments, every live row has the entries of its member, and col_verify is clean. The pair relation has its journal index; the first
-- scope index of the relation starts the journal, so it is built twice (the REINDEX takes the stamps) and the builds after it are stamped.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

-- 12 tenants; 300 members in one to six of them
CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 6) j WHERE m % 25 <> 0;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');

-- one table, one index (tbl, idx): the rows of the scope read against the join per key, the entries against the join, whether the scope segments cover
CREATE FUNCTION check_index(tbl regclass, idx regclass, OUT covered boolean, OUT same boolean, OUT stored boolean, OUT missing bigint, OUT extra bigint) LANGUAGE plpgsql AS $$
DECLARE
	k int;
	a text;
	b text;
	p jsonb;
BEGIN
	covered := sift._test_col_scope_covered(idx, 'tenant_id');
	same := true;
	stored := true;
	FOREACH k IN ARRAY ARRAY[2, 5, 9, 12] LOOP
		SET LOCAL sift.enable_agg = on;
		EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*), sum(v), min(id), max(id) FROM %s e WHERE sift.in_scope(%L::regclass, ''tenant_id'', e.dim_a_id, %s)', tbl, idx, k) INTO p;
		EXECUTE format('SELECT md5(t::text) FROM (SELECT count(*), sum(v), min(id), max(id) FROM %s e WHERE sift.in_scope(%L::regclass, ''tenant_id'', e.dim_a_id, %s)) t', tbl, idx, k) INTO a;
		SET LOCAL sift.enable_agg = off;
		EXECUTE format('SELECT md5(t::text) FROM (SELECT count(*), sum(v), min(id), max(id) FROM %s e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = %s)) t', tbl, k) INTO b;
		SET LOCAL sift.enable_agg = on;
		same := same AND a = b;
		stored := stored AND jsonb_path_exists(p, '$.**."Scope Units Read"');
	END LOOP;
	EXECUTE format('SELECT count(*) FROM (SELECT e.ctid, s.tenant_id FROM %s e JOIN tenant_scope s USING (dim_a_id) EXCEPT SELECT tid, key::int FROM sift._test_col_scope_entries(%L)) m', tbl, idx) INTO missing;
	EXECUTE format('SELECT count(*) FROM (SELECT tid, key::int FROM sift._test_col_scope_entries(%L) EXCEPT SELECT e.ctid, s.tenant_id FROM %s e JOIN tenant_scope s USING (dim_a_id)) x', idx, tbl) INTO extra;
END $$;

-- 12 000 padded events, 3 to a page: 16 superblocks of 256 pages, enough for three participants
CREATE TABLE events (id int, dim_a_id int, created_at timestamptz, v int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 999 ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 12000) i;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) tenant_scope;
SET maintenance_work_mem = '128MB';
SET min_parallel_table_scan_size = 0;
SET max_parallel_maintenance_workers = 2;

-- the serial build, the reference
SET max_parallel_maintenance_workers = 0;
CREATE INDEX events_ser ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_ser;
SELECT 'serial' AS build, current_setting('sift.test_col_workers')::int AS workers, c.* FROM check_index('events', 'events_ser') c;
SELECT md5(string_agg(tid::text || ':' || key, ',' ORDER BY tid, key)) AS serial_entries FROM sift._test_col_scope_entries('events_ser') \gset
SET max_parallel_maintenance_workers = 2;

-- the participants: the same entries whatever the segment boundaries
CREATE INDEX events_par ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT 'parallel' AS build, current_setting('sift.test_col_workers')::int > 0 AS workers, c.* FROM check_index('events', 'events_par') c;
SELECT md5(string_agg(tid::text || ':' || key, ',' ORDER BY tid, key)) = :'serial_entries' AS same_entries_as_serial FROM sift._test_col_scope_entries('events_par');
SELECT sift.col_verify('events_par', true);
SELECT count(DISTINCT gen) FILTER (WHERE family = 'scope') AS scope_segments FROM sift._test_col_scope_stamps('events_par') WHERE scope = 0;
SELECT count(DISTINCT horizon) AS one_stamp FROM sift._test_col_scope_stamps('events_par') WHERE scope = 0 AND family = 'scope';

-- REINDEX in parallel
REINDEX INDEX events_par;
SELECT 'reindex' AS build, current_setting('sift.test_col_workers')::int > 0 AS workers, c.* FROM check_index('events', 'events_par') c;
-- rows after the build are promoted with their entries, and the participants' segments merge with them
INSERT INTO events SELECT 12000 + i, 1 + i % 300, timestamptz '2026-02-01' + i * interval '1 hour', i % 10, 'y' FROM generate_series(1, 300) i;
SELECT sift.col_promote('events_par') >= 0 AS promoted;
SELECT sift.col_merge('events_par', true) >= 0 AS merged;
SELECT 'promoted and merged' AS build, c.* FROM check_index('events', 'events_par') c;
SELECT sift.col_verify('events_par', true);
DROP INDEX events_ser, events_par;

-- a partitioned table: the index of each partition has its scope segments, from the same scope relation
CREATE TABLE pevents (id int, dim_a_id int, created_at timestamptz, v int) PARTITION BY RANGE (id);
CREATE TABLE pevents_1 PARTITION OF pevents FOR VALUES FROM (1) TO (4001) WITH (autovacuum_enabled = false);
CREATE TABLE pevents_2 PARTITION OF pevents FOR VALUES FROM (4001) TO (8001) WITH (autovacuum_enabled = false);
CREATE TABLE pevents_3 PARTITION OF pevents FOR VALUES FROM (8001) TO (12001) WITH (autovacuum_enabled = false);
INSERT INTO pevents SELECT id, dim_a_id, created_at, v FROM events WHERE id <= 12000;
CREATE INDEX pevents_col ON pevents USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT c.relname AS partition, p.* FROM pg_class c, LATERAL check_index(c.oid, (SELECT i.indexrelid FROM pg_index i WHERE i.indrelid = c.oid)) p
	WHERE c.relname LIKE 'pevents\_%' AND c.relkind = 'r' ORDER BY 1;
-- the scope relation cannot go while the partitions' indexes depend on it
DROP TABLE tenant_scope;
-- a partition attached later builds its own
CREATE TABLE pevents_4 (LIKE pevents) WITH (autovacuum_enabled = false);
INSERT INTO pevents_4 SELECT i, 1 + i % 300, timestamptz '2026-03-01', i % 10 FROM generate_series(12001, 13000) i;
ALTER TABLE pevents ATTACH PARTITION pevents_4 FOR VALUES FROM (12001) TO (13001);
SELECT c.relname AS partition, p.* FROM pg_class c, LATERAL check_index(c.oid, (SELECT i.indexrelid FROM pg_index i WHERE i.indrelid = c.oid)) p
	WHERE c.relname = 'pevents_4';
SELECT sift.col_verify(i.indexrelid, true) FROM pg_index i JOIN pg_class c ON c.oid = i.indrelid WHERE c.relname LIKE 'pevents\_%' AND c.relkind = 'r' ORDER BY c.relname;

DROP TABLE pevents, events, tenant_scope;
DROP FUNCTION check_index(regclass, regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
