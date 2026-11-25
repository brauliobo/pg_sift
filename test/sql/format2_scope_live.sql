-- The scope summaries of an index of format major 2, live (col/f2/scope/scope.c): CREATE INDEX with a scope writes the entity parts once and, from their granules, one summary for each scope that
-- holds every scope key's aggregates, instead of a copy of every fact row for each key. The cube of every key (the rows and the sums of the measures: the integer and float leaves of the
-- index) is read back through the entity side a query reads (deletion vectors, strides) and compared with a join of the facts with the pairs of the scope over the heap; so are the newest rows
-- of a key and the cube of a month. The summary follows the pairs (an INSERT, UPDATE or DELETE of the mapping table) and the rows (INSERT, UPDATE, DELETE, VACUUM, the compaction it does)
-- once VACUUM has built it again; a CREATE INDEX CONCURRENTLY builds it in its validation pass; a float8 NaN is summed as PostgreSQL does; a session time zone changes none of the answers;
-- REINDEX CONCURRENTLY of an index in format major 1 with a scope moves it to format major 2 with the same answers. The journal index of the pairs relation is a key log of format major 1,
-- which the scope makes for itself whatever format a new index has.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.test_create_format1 = off;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
-- member m (1..1500) is in (m % 7) of the 40 keys: 0..6
INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 40 FROM generate_series(1, 1500) m, generate_series(0, 5) j WHERE j < m % 7;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, kind int NOT NULL, value_num int NOT NULL, price float8, flag bool) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE 1 + (i * 7919) % 1500 END, timestamptz '2026-01-01' + i * interval '7 minutes', i % 7, i % 1000,
	CASE WHEN i % 11 = 0 THEN NULL ELSE (i % 50) * 0.5 END, i % 3 = 0 FROM generate_series(1, 30000) i;

-- the summary of scope 0 of an index against the join over the heap: the keys, rows and sums that differ (month: the cube of that UTC month, NULL: of all time)
CREATE FUNCTION cube_diff(idx regclass, month int DEFAULT NULL) RETURNS bigint LANGUAGE plpgsql AS
$$ BEGIN
	RETURN (SELECT count(*) FROM sift._test_f2_scope_keys(idx, 0, month) k
		FULL JOIN (SELECT s.tenant_id AS scope_key, count(*) AS rows, ARRAY[sum(e.id), sum(e.kind), sum(e.value_num), coalesce(sum(e.price), 0)]::float8[] AS sums
				   FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id
				   WHERE month IS NULL OR (extract(year FROM e.created_at AT TIME ZONE 'UTC') - 2000) * 12 + extract(month FROM e.created_at AT TIME ZONE 'UTC') - 1 = month
				   GROUP BY 1) b USING (scope_key)
		WHERE k.rows IS DISTINCT FROM b.rows OR k.sums IS DISTINCT FROM b.sums);
END $$;
-- the newest rows of the keys against the heap: the keys whose newest 5 times differ
CREATE FUNCTION latest_diff(idx regclass) RETURNS bigint LANGUAGE plpgsql AS
$$ BEGIN
	RETURN (SELECT count(*) FROM (SELECT DISTINCT tenant_id FROM tenant_scope) k
		WHERE sift._test_f2_scope_latest(idx, 0, k.tenant_id, 5) IS DISTINCT FROM
			  ARRAY(SELECT ((extract(epoch FROM e.created_at) - 946684800) * 1000000)::int8 FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = k.tenant_id
					ORDER BY e.created_at DESC LIMIT 5));
END $$;

-- 1. Plain CREATE INDEX: format major 2, one summary, every cube and the newest rows of every key as the join says
CREATE INDEX events_plain ON events USING sift_col (id, dim_a_id, created_at, kind, value_num, price, flag) WITH (track_changes = 'ctid');
CREATE INDEX events_s ON events USING sift_col (id, dim_a_id, created_at, kind, value_num, price, flag) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)', track_changes = 'ctid');
SELECT sift._test_index_format('events_s') AS format_major;
SELECT sift._test_f2_scope_info('events_s', 0) ~ '^keys=40 parts=1 ' AS forty_keys;
SELECT cube_diff('events_s') AS cube_differences, latest_diff('events_s') AS latest_differences;
SELECT cube_diff('events_s', 312) AS month_differences;
SELECT sift._test_f2_scope_current('events_s', 0) AS current;
-- the facts are in the index once: the chunks of the granules take the bytes they take without the scope (with a copy of the rows for each key they would take 1 to 5 times as many), the
-- summary is what the scope adds
CREATE FUNCTION granule_bytes(idx regclass) RETURNS bigint LANGUAGE sql AS
$$ SELECT sum(bytes) FROM sift.index_size_breakdown(idx) WHERE category LIKE 'granule\_%' $$;
SELECT granule_bytes('events_s') = granule_bytes('events_plain') AS entity_chunks_once,
       (SELECT sum(nparts) FROM sift._test_f2_partitions('events_s')) = (SELECT sum(nparts) FROM sift._test_f2_partitions('events_plain')) AS same_entity_parts;

-- 2. CREATE INDEX CONCURRENTLY: the summary is built in the validation pass
CREATE INDEX CONCURRENTLY events_c ON events USING sift_col (id, dim_a_id, created_at, kind, value_num, price, flag) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)', track_changes = 'ctid');
SELECT sift._test_index_format('events_c') AS format_major;
SELECT cube_diff('events_c') AS cube_differences, latest_diff('events_c') AS latest_differences, sift._test_f2_scope_current('events_c', 0) AS current;

-- 3. The pairs change (INSERT, UPDATE and DELETE of the mapping table): the summary is not current until VACUUM builds it again
INSERT INTO tenant_scope SELECT m, 41 + m % 3 FROM generate_series(1, 300) m;
UPDATE tenant_scope SET tenant_id = tenant_id + 100 WHERE dim_a_id BETWEEN 400 AND 420 AND tenant_id < 10;
DELETE FROM tenant_scope WHERE dim_a_id BETWEEN 900 AND 960 AND tenant_id > 30;
SELECT sift._test_f2_scope_current('events_s', 0) AS current_after_pair_edits;
VACUUM events;
SELECT sift._test_f2_scope_current('events_s', 0) AS current_after_vacuum, cube_diff('events_s') AS cube_differences, latest_diff('events_s') AS latest_differences;

-- 4. Rows change: INSERT, UPDATE, DELETE, then VACUUM (the deletion vectors, the parts the inserts made, the compaction)
INSERT INTO events SELECT i, 1 + (i * 7919) % 1500, timestamptz '2026-03-01' + i * interval '9 minutes', i % 7, i % 1000, (i % 50) * 0.5, i % 3 = 0 FROM generate_series(30001, 36000) i;
UPDATE events SET value_num = value_num + 1, created_at = created_at + interval '1 day' WHERE id % 13 = 0;
DELETE FROM events WHERE id % 29 = 0;
VACUUM events;
SELECT cube_diff('events_s') AS cube_differences, latest_diff('events_s') AS latest_differences, sift._test_f2_scope_current('events_s', 0) AS current;
SELECT cube_diff('events_s', 314) AS month_differences;
-- rows deleted after the last build: the summary is not current for the parts it names, and the deletion vector a VACUUM writes is taken out by the reader
DELETE FROM events WHERE id % 31 = 0;
VACUUM events;
SELECT cube_diff('events_s') AS cube_differences, latest_diff('events_s') AS latest_differences;

-- 5. float8 NaN and the infinities are summed as PostgreSQL does; a time zone of the session changes no answer
CREATE TABLE nan_facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, kind int NOT NULL, value_num int NOT NULL, price float8, flag bool) WITH (autovacuum_enabled = false);
INSERT INTO nan_facts SELECT i, 1 + i % 30, timestamptz '2026-02-28 23:30:00+00' + i * interval '41 minutes', i % 7, i % 100,
	CASE WHEN i % 97 = 0 THEN 'NaN' WHEN i % 89 = 0 THEN 'Infinity' WHEN i % 83 = 0 THEN '-Infinity' ELSE (i % 40) * 0.25 END, i % 2 = 0 FROM generate_series(1, 3000) i;
CREATE INDEX nan_s ON nan_facts USING sift_col (id, dim_a_id, created_at, kind, value_num, price, flag) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT count(*) AS nan_keys_differ FROM sift._test_f2_scope_keys('nan_s', 0) k
	FULL JOIN (SELECT s.tenant_id AS scope_key, count(*) AS rows, sum(e.price) AS price FROM nan_facts e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id GROUP BY 1) b USING (scope_key)
	WHERE k.rows IS DISTINCT FROM b.rows OR k.sums[4] IS DISTINCT FROM coalesce(b.price, 0);
CREATE TEMP TABLE zone_answers AS SELECT 'UTC'::text AS zone, md5(string_agg(k::text, ',' ORDER BY (k).scope_key)) AS digest FROM sift._test_f2_scope_keys('events_s', 0) k;
SET TimeZone = 'America/New_York';
INSERT INTO zone_answers SELECT 'America/New_York', md5(string_agg(k::text, ',' ORDER BY (k).scope_key)) FROM sift._test_f2_scope_keys('events_s', 0) k;
SELECT cube_diff('events_s', 314) AS month_differences_new_york, latest_diff('events_s') AS latest_differences_new_york;
SELECT count(DISTINCT digest) AS answers_over_two_zones FROM zone_answers;
RESET TimeZone;

-- 6. An index of format major 1 with a scope: REINDEX CONCURRENTLY writes format major 2 and the same answers
CREATE TABLE old_events (LIKE events INCLUDING DEFAULTS);
INSERT INTO old_events SELECT * FROM events WHERE id % 3 = 0;
SET sift.test_create_format1 = on;
CREATE INDEX old_events_s ON old_events USING sift_col (id, dim_a_id, created_at, kind, value_num, price, flag) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SET sift.test_create_format1 = off;
SELECT sift._test_index_format('old_events_s') AS format_before;
REINDEX INDEX CONCURRENTLY old_events_s;
SELECT sift._test_index_format('old_events_s') AS format_after;
SELECT count(*) AS keys_differ FROM sift._test_f2_scope_keys('old_events_s', 0) k
	FULL JOIN (SELECT s.tenant_id AS scope_key, count(*) AS rows, ARRAY[sum(e.id), sum(e.kind), sum(e.value_num), coalesce(sum(e.price), 0)]::float8[] AS sums
			   FROM old_events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id GROUP BY 1) b USING (scope_key)
	WHERE k.rows IS DISTINCT FROM b.rows OR k.sums IS DISTINCT FROM b.sums;

DROP TABLE nan_facts, old_events, events, tenant_scope;
DROP FUNCTION cube_diff(regclass, int), latest_diff(regclass), granule_bytes(regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
