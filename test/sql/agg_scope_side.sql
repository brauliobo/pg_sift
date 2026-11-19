-- The build writes the scope sidecar of the entity segments (the maintenance writes those a merge or a concurrent build left without), and a scope read of the entity rows takes the runs of the scope sidecar (col/side.h) for the units they stand for, and its rows are the join's whatever the state of the pairs: the runs of a key answer
-- for the member conjunct of the scan, a run with a member whose pair changed since the stamp of the sidecar is read unit by unit, and a scan told not to take the runs reads the same rows from the units.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.col_unit_rows = 150;
SET timezone = 'UTC';
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 FROM generate_series(1, 60) m WHERE m % 6 < 5;
INSERT INTO tenant_scope SELECT m, 2 FROM generate_series(1, 60) m WHERE m % 3 = 0;
INSERT INTO tenant_scope SELECT m, 3 FROM generate_series(1, 60) m WHERE m IN (7, 8);
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL);
INSERT INTO events SELECT i, 1 + i % 60, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 24000) i;
VACUUM (FREEZE, ANALYZE) events, tenant_scope;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, v)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT count(*) > 0 AS sidecar_written_by_the_build FROM sift._test_col_formats('events_col') s WHERE s.format >= 53;
SELECT sift.col_merge('events_col') AS second_call_writes;

-- the statement of one key: the join's rows (the normal plan), the rows of the entity scan with the runs and without them, and the runs the scan took
CREATE FUNCTION q(key int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT kind, count(*) AS n, sum(v) AS s FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = %s) GROUP BY kind', key) $$;

CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	DROP TABLE IF EXISTS side_rows;
	EXECUTE 'CREATE TEMP TABLE side_rows AS ' || q;
	SELECT md5(coalesce(string_agg(t::text, '|' ORDER BY t::text), '')) INTO r FROM side_rows t;
	DROP TABLE side_rows;
	RETURN r;
END $$;

CREATE FUNCTION runs_of(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; r int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Scope Sidecar Runs' THEN r := regexp_replace(l, '^.*: ', '')::int; END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE FUNCTION scanned_of(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text; r bigint := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ '^ *Rows: ' THEN r := substring(l from 'Rows: ([0-9]+) scanned')::bigint; END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE FUNCTION state(key int) RETURNS TABLE (k int, same_as_join bool, same_without_runs bool, runs_taken bool, no_runs_without bool, fewer_rows_read bool) LANGUAGE plpgsql AS $$
DECLARE join_md5 text; entity_md5 text; plain_md5 text; with_runs int; without_runs int; scanned_with bigint; scanned_without bigint;
BEGIN
	SET LOCAL sift.agg_scope = on;
	SET LOCAL sift.enable_agg = off;
	join_md5 := rows_md5(q(key));
	SET LOCAL sift.enable_agg = on;
	SET LOCAL sift.test_scope_entity = on;
	SET LOCAL sift.test_scope_no_side = off;
	entity_md5 := rows_md5(q(key));
	with_runs := runs_of(q(key));
	scanned_with := scanned_of(q(key));
	SET LOCAL sift.test_scope_no_side = on;
	plain_md5 := rows_md5(q(key));
	without_runs := runs_of(q(key));
	scanned_without := scanned_of(q(key));
	RETURN QUERY SELECT key, entity_md5 = join_md5, plain_md5 = join_md5, with_runs > 0, without_runs = 0, scanned_with < scanned_without;
END $$;

-- a fresh sidecar
SELECT * FROM state(1) UNION ALL SELECT * FROM state(2) UNION ALL SELECT * FROM state(3) ORDER BY 1;

-- a pair of key 1 ended, a pair inserted, a pair of another key moved: the runs of the members that changed are read unit by unit
DELETE FROM tenant_scope WHERE tenant_id = 1 AND dim_a_id IN (10, 11, 40);
INSERT INTO tenant_scope VALUES (5, 1), (17, 1), (12, 3);
UPDATE tenant_scope SET tenant_id = 2 WHERE tenant_id = 3 AND dim_a_id = 8;
SELECT * FROM state(1) UNION ALL SELECT * FROM state(2) UNION ALL SELECT * FROM state(3) ORDER BY 1;

-- new rows of a member inside a run, and a row deleted
INSERT INTO events SELECT i, 1 + i % 60, timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(24001, 24300) i;
DELETE FROM events WHERE id % 97 = 0;
SELECT * FROM state(1) UNION ALL SELECT * FROM state(2) UNION ALL SELECT * FROM state(3) ORDER BY 1;

-- facts with no member: the units that hold some are summarised for the rows that have one, the units of such rows only hold no key's rows, and the sidecar is made all the same
ALTER TABLE events ALTER COLUMN dim_a_id DROP NOT NULL;
UPDATE events SET dim_a_id = NULL WHERE id % 97 = 0 OR id > 23000;
VACUUM (FREEZE, ANALYZE) events;
REINDEX INDEX events_col;
SELECT count(*) > 0 AS sidecar_written_with_facts_with_no_member FROM sift._test_col_formats('events_col') s WHERE s.format >= 53;
SELECT * FROM state(1) UNION ALL SELECT * FROM state(2) UNION ALL SELECT * FROM state(3) ORDER BY 1;

-- an index built before its first pair exists
CREATE TABLE no_pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id));
CREATE INDEX no_pairs_journal ON no_pairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX events_no_pairs_col ON events USING sift_col (id, dim_a_id, created_at, kind, v)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> no_pairs(dim_a_id, tenant_id)');
SELECT count(*) > 0 AS sidecar_written_with_no_pairs FROM sift._test_col_formats('events_no_pairs_col') s WHERE s.format >= 53;

DROP FUNCTION state(int);
DROP FUNCTION scanned_of(text);
DROP FUNCTION runs_of(text);
DROP FUNCTION rows_md5(text);
DROP FUNCTION q(int);
DROP TABLE events, tenant_scope, no_pairs;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
