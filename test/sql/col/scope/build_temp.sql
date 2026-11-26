-- The index of a scope respects its keys when it is made, with few temporary bytes: a scope key is in one unit of the scope segments (the entries of one key are read together), the entries
-- are the join of the facts with the pairs, and the temporary files of the build (the rows of the key ranges, sift.test_col_spill_bytes, and the sorts and merges of the runs, sift._test_col_cut_sort_disk_kb;
-- not the sort of the addresses of the table that PostgreSQL makes in the validation pass of CREATE INDEX CONCURRENTLY, 30 bytes a row of it at this memory) take at most 110 bytes for each row of the
-- table at every size, for CREATE INDEX and CREATE INDEX CONCURRENTLY (whose second pass fills the scope segments); with a key_cut the rows of the entities go to files as well: 200 bytes, and the key
-- of the cut is in one unit as well. The rows of the files are the compressed rows of the entries (the table has an incompressible column of 64 bytes and 1.43 entries a row). The key_cut of the other
-- builds is off: the automatic decision of the key reads a sample through temporary files of its own. Maintenance memory and segments are small on purpose so that every build writes temporary files.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
-- Format 1: the scope segments, their units and entries (col/v1/scope) are this format's; Format 2.0 keeps a scope summary (col/f2/scope_*)
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.test_col_parallel_always = off;

CREATE TABLE sb_pairs (m int NOT NULL, t int NOT NULL) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX sb_pairs_u ON sb_pairs (m, t);
INSERT INTO sb_pairs SELECT m, 1 + (m * 7 + j * 53) % 160 FROM generate_series(1, 200) m, generate_series(0, 2) j WHERE j < m % 4 ON CONFLICT DO NOTHING;
CREATE INDEX sb_pairs_j ON sb_pairs USING sift_col (m, t) WITH (track_changes = 'ctid');

VACUUM (FREEZE, ANALYZE) sb_pairs;
CREATE TABLE sb_res (n int, build text, temp_within_bound bool, scope_key_in_one_unit bool, pairs_are_the_join bool, key_in_one_unit bool);
-- plpgsql, so that nothing refers to the index before it exists
CREATE FUNCTION sb_note(n int, build text, cut bool, temp_bytes bigint, a_row int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO sb_res VALUES (n, build,
		temp_bytes < a_row * n,
		(SELECT max(u) FROM (SELECT count(DISTINCT (gen, unit)) u FROM sift._test_col_scope_entries('sb_ix') GROUP BY key) s) = 1,
		(SELECT md5(string_agg(tid::text || ':' || key, ',' ORDER BY tid, key)) FROM sift._test_col_scope_entries('sb_ix')) =
			(SELECT md5(string_agg(e.ctid::text || ':' || c.t, ',' ORDER BY e.ctid, c.t)) FROM sb_t e JOIN sb_pairs c ON c.m = e.m),
		CASE WHEN cut THEN (SELECT point_units < 1.05 FROM sift.col_clustering('sb_ix', 1) WHERE path = 'd') END);
	PERFORM set_config('maintenance_work_mem', '64MB', true);
	PERFORM sift.col_verify('sb_ix', true);
END $$;

SET maintenance_work_mem = '2MB';
SET sift.test_col_segment_kb = 256;
SET sift.test_col_spill_run_kb = 256;
\set scope '''t: m -> sb_pairs(m, t)'''
\set n 20000
CREATE TABLE sb_t (id int PRIMARY KEY, m int, d timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO sb_t SELECT i, 1 + (i * 7919) % 210, timestamptz '2026-01-01' + (i * 37 % 300) * interval '1 day', i % 10, md5(i::text) || md5((i + 1)::text) FROM generate_series(1, :n) i;
VACUUM (FREEZE, ANALYZE) sb_t;

SELECT sift._test_col_cut_sorts_reset();
CREATE INDEX sb_ix ON sb_t USING sift_col (id, m, d, v, pad) WITH (scope = :scope, key_cut = 'off');
SELECT sb_note(:n, 'create', false, current_setting('sift.test_col_spill_bytes')::bigint + sift._test_col_cut_sort_disk_kb() * 1024, 110);
DROP INDEX sb_ix;

SELECT sift._test_col_cut_sorts_reset();
CREATE INDEX CONCURRENTLY sb_ix ON sb_t USING sift_col (id, m, d, v, pad) WITH (scope = :scope, key_cut = 'off');
SELECT sb_note(:n, 'concurrently', false, current_setting('sift.test_col_spill_bytes')::bigint + sift._test_col_cut_sort_disk_kb() * 1024, 110);
DROP INDEX sb_ix;

SELECT sift._test_col_cut_sorts_reset();
CREATE INDEX sb_ix ON sb_t USING sift_col (id, m, d, v, pad) WITH (scope = :scope, key_cut = 'd');
SELECT sb_note(:n, 'create, key_cut', true, current_setting('sift.test_col_spill_bytes')::bigint + sift._test_col_cut_sort_disk_kb() * 1024, 200);
DROP INDEX sb_ix;

SELECT sift._test_col_cut_sorts_reset();
CREATE INDEX CONCURRENTLY sb_ix ON sb_t USING sift_col (id, m, d, v, pad) WITH (scope = :scope, key_cut = 'd');
SELECT sb_note(:n, 'concurrently, key_cut', true, current_setting('sift.test_col_spill_bytes')::bigint + sift._test_col_cut_sort_disk_kb() * 1024, 200);
DROP INDEX sb_ix;

DROP TABLE sb_t;

\set n 40000
CREATE TABLE sb_t (id int PRIMARY KEY, m int, d timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO sb_t SELECT i, 1 + (i * 7919) % 210, timestamptz '2026-01-01' + (i * 37 % 300) * interval '1 day', i % 10, md5(i::text) || md5((i + 1)::text) FROM generate_series(1, :n) i;
VACUUM (FREEZE, ANALYZE) sb_t;

SELECT sift._test_col_cut_sorts_reset();
CREATE INDEX sb_ix ON sb_t USING sift_col (id, m, d, v, pad) WITH (scope = :scope, key_cut = 'off');
SELECT sb_note(:n, 'create', false, current_setting('sift.test_col_spill_bytes')::bigint + sift._test_col_cut_sort_disk_kb() * 1024, 110);
DROP INDEX sb_ix;

SELECT sift._test_col_cut_sorts_reset();
CREATE INDEX CONCURRENTLY sb_ix ON sb_t USING sift_col (id, m, d, v, pad) WITH (scope = :scope, key_cut = 'off');
SELECT sb_note(:n, 'concurrently', false, current_setting('sift.test_col_spill_bytes')::bigint + sift._test_col_cut_sort_disk_kb() * 1024, 110);
DROP INDEX sb_ix;

SELECT sift._test_col_cut_sorts_reset();
CREATE INDEX sb_ix ON sb_t USING sift_col (id, m, d, v, pad) WITH (scope = :scope, key_cut = 'd');
SELECT sb_note(:n, 'create, key_cut', true, current_setting('sift.test_col_spill_bytes')::bigint + sift._test_col_cut_sort_disk_kb() * 1024, 200);
DROP INDEX sb_ix;

SELECT sift._test_col_cut_sorts_reset();
CREATE INDEX CONCURRENTLY sb_ix ON sb_t USING sift_col (id, m, d, v, pad) WITH (scope = :scope, key_cut = 'd');
SELECT sb_note(:n, 'concurrently, key_cut', true, current_setting('sift.test_col_spill_bytes')::bigint + sift._test_col_cut_sort_disk_kb() * 1024, 200);
DROP INDEX sb_ix;

DROP TABLE sb_t;

\set n 80000
CREATE TABLE sb_t (id int PRIMARY KEY, m int, d timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO sb_t SELECT i, 1 + (i * 7919) % 210, timestamptz '2026-01-01' + (i * 37 % 300) * interval '1 day', i % 10, md5(i::text) || md5((i + 1)::text) FROM generate_series(1, :n) i;
VACUUM (FREEZE, ANALYZE) sb_t;

SELECT sift._test_col_cut_sorts_reset();
CREATE INDEX sb_ix ON sb_t USING sift_col (id, m, d, v, pad) WITH (scope = :scope, key_cut = 'off');
SELECT sb_note(:n, 'create', false, current_setting('sift.test_col_spill_bytes')::bigint + sift._test_col_cut_sort_disk_kb() * 1024, 110);
DROP INDEX sb_ix;

SELECT sift._test_col_cut_sorts_reset();
CREATE INDEX CONCURRENTLY sb_ix ON sb_t USING sift_col (id, m, d, v, pad) WITH (scope = :scope, key_cut = 'off');
SELECT sb_note(:n, 'concurrently', false, current_setting('sift.test_col_spill_bytes')::bigint + sift._test_col_cut_sort_disk_kb() * 1024, 110);
DROP INDEX sb_ix;

SELECT sift._test_col_cut_sorts_reset();
CREATE INDEX sb_ix ON sb_t USING sift_col (id, m, d, v, pad) WITH (scope = :scope, key_cut = 'd');
SELECT sb_note(:n, 'create, key_cut', true, current_setting('sift.test_col_spill_bytes')::bigint + sift._test_col_cut_sort_disk_kb() * 1024, 200);
DROP INDEX sb_ix;

SELECT sift._test_col_cut_sorts_reset();
CREATE INDEX CONCURRENTLY sb_ix ON sb_t USING sift_col (id, m, d, v, pad) WITH (scope = :scope, key_cut = 'd');
SELECT sb_note(:n, 'concurrently, key_cut', true, current_setting('sift.test_col_spill_bytes')::bigint + sift._test_col_cut_sort_disk_kb() * 1024, 200);
DROP INDEX sb_ix;

DROP TABLE sb_t;

-- one scope key holding a third of the rows among many small ones: the small keys are each in one unit, the big one (larger than a segment) fills several, and the entries are the join
CREATE TABLE sb_big (m int NOT NULL, t int NOT NULL) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX sb_big_u ON sb_big (m, t);
INSERT INTO sb_big SELECT m, 1 FROM generate_series(1, 70) m;
INSERT INTO sb_big SELECT m, 2 + (m * 7 + j * 53) % 159 FROM generate_series(71, 210) m, generate_series(0, 2) j WHERE j < m % 4 ON CONFLICT DO NOTHING;
CREATE INDEX sb_big_j ON sb_big USING sift_col (m, t) WITH (track_changes = 'ctid');
VACUUM (FREEZE, ANALYZE) sb_big;
CREATE TABLE sb_t (id int PRIMARY KEY, m int, d timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO sb_t SELECT i, 1 + (i * 7919) % 210, timestamptz '2026-01-01' + (i * 37 % 300) * interval '1 day', i % 10, md5(i::text) || md5((i + 1)::text) FROM generate_series(1, 20000) i;
VACUUM (FREEZE, ANALYZE) sb_t;
CREATE INDEX sb_ix ON sb_t USING sift_col (id, m, d, v, pad) WITH (scope = 't: m -> sb_big(m, t)', key_cut = 'off');
SELECT (SELECT max(u) FROM (SELECT key, count(DISTINCT (gen, unit)) u FROM sift._test_col_scope_entries('sb_ix') GROUP BY key) s WHERE key <> 1) AS small_keys_max_units,
	(SELECT count(DISTINCT (gen, unit)) FROM sift._test_col_scope_entries('sb_ix') WHERE key = 1) > 1 AS big_key_in_pieces,
	(SELECT md5(string_agg(tid::text || ':' || key, ',' ORDER BY tid, key)) FROM sift._test_col_scope_entries('sb_ix')) =
		(SELECT md5(string_agg(e.ctid::text || ':' || c.t, ',' ORDER BY e.ctid, c.t)) FROM sb_t e JOIN sb_big c ON c.m = e.m) AS pairs_are_the_join;
DROP INDEX sb_ix;
DROP TABLE sb_t, sb_big;

SELECT n, build, temp_within_bound, scope_key_in_one_unit, pairs_are_the_join, key_in_one_unit FROM sb_res ORDER BY n, build;

RESET maintenance_work_mem;
RESET sift.test_col_segment_kb;
RESET sift.test_col_spill_run_kb;
RESET sift.test_limits;
DROP TABLE sb_res, sb_pairs;
DROP FUNCTION sb_note(int, text, bool, bigint, int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
