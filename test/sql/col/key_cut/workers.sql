-- A key_cut index is built by one process, whatever max_parallel_maintenance_workers says, for CREATE INDEX, REINDEX and their CONCURRENTLY forms: the participants of a parallel build would each hold
-- every key and leave runs that overlap in it (a CONCURRENTLY build has no merge after it that cuts them: the index ended with its key in two units and nothing due), and the build by one process
-- writes the rows to files for the ranges of the key (col/v1/build/spill.c), so the key is in one unit at every size. sift.test_col_parallel_always is off here: it is the option of the tests of
-- the participants' merge, which its default (on) keeps in the tests of the other builds; a release build has no participant for a key_cut index.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;
SET sift.test_col_parallel_always = off;
SET max_parallel_maintenance_workers = 2;
SET min_parallel_table_scan_size = 0;
SET maintenance_work_mem = '1MB';
SET sift.test_col_segment_kb = 256;
SET sift.test_col_spill_run_kb = 256;

CREATE TABLE kw_res (n int, build text, workers int, key_in_one_unit bool);
-- plpgsql, so that nothing refers to the index before it exists
CREATE FUNCTION kw_note(n int, build text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO kw_res VALUES (n, build, current_setting('sift.test_col_workers')::int,
		(SELECT point_units < 1.05 FROM sift.col_clustering('kw_ix', 1) WHERE path = 'm'));
	PERFORM set_config('maintenance_work_mem', '64MB', true);
	PERFORM sift.col_verify('kw_ix', true);
END $$;
\set n 20000
CREATE TABLE kw_t (id int PRIMARY KEY, m int, d timestamptz, v int, pad text) WITH (autovacuum_enabled = false, parallel_workers = 2);
INSERT INTO kw_t SELECT i, 1 + (i::bigint * 7919 % 210)::int, timestamptz '2026-01-01' + (i * 37 % 300) * interval '1 day', i % 10, md5(i::text) || md5((i + 1)::text) FROM generate_series(1, :n) i;
VACUUM (FREEZE, ANALYZE) kw_t;

CREATE INDEX kw_ix ON kw_t USING sift_col (id, m, d, v, pad) WITH (key_cut = 'm');
SELECT kw_note(:n, 'create');
REINDEX INDEX kw_ix;
SELECT kw_note(:n, 'reindex');
REINDEX INDEX CONCURRENTLY kw_ix;
SELECT kw_note(:n, 'reindex concurrently');
DROP INDEX kw_ix;
CREATE INDEX CONCURRENTLY kw_ix ON kw_t USING sift_col (id, m, d, v, pad) WITH (key_cut = 'm');
SELECT kw_note(:n, 'concurrently');
DROP INDEX kw_ix;
DROP TABLE kw_t;

\set n 40000
CREATE TABLE kw_t (id int PRIMARY KEY, m int, d timestamptz, v int, pad text) WITH (autovacuum_enabled = false, parallel_workers = 2);
INSERT INTO kw_t SELECT i, 1 + (i::bigint * 7919 % 210)::int, timestamptz '2026-01-01' + (i * 37 % 300) * interval '1 day', i % 10, md5(i::text) || md5((i + 1)::text) FROM generate_series(1, :n) i;
VACUUM (FREEZE, ANALYZE) kw_t;

CREATE INDEX kw_ix ON kw_t USING sift_col (id, m, d, v, pad) WITH (key_cut = 'm');
SELECT kw_note(:n, 'create');
REINDEX INDEX kw_ix;
SELECT kw_note(:n, 'reindex');
REINDEX INDEX CONCURRENTLY kw_ix;
SELECT kw_note(:n, 'reindex concurrently');
DROP INDEX kw_ix;
CREATE INDEX CONCURRENTLY kw_ix ON kw_t USING sift_col (id, m, d, v, pad) WITH (key_cut = 'm');
SELECT kw_note(:n, 'concurrently');
DROP INDEX kw_ix;
DROP TABLE kw_t;

\set n 80000
CREATE TABLE kw_t (id int PRIMARY KEY, m int, d timestamptz, v int, pad text) WITH (autovacuum_enabled = false, parallel_workers = 2);
INSERT INTO kw_t SELECT i, 1 + (i::bigint * 7919 % 210)::int, timestamptz '2026-01-01' + (i * 37 % 300) * interval '1 day', i % 10, md5(i::text) || md5((i + 1)::text) FROM generate_series(1, :n) i;
VACUUM (FREEZE, ANALYZE) kw_t;

CREATE INDEX kw_ix ON kw_t USING sift_col (id, m, d, v, pad) WITH (key_cut = 'm');
SELECT kw_note(:n, 'create');
REINDEX INDEX kw_ix;
SELECT kw_note(:n, 'reindex');
REINDEX INDEX CONCURRENTLY kw_ix;
SELECT kw_note(:n, 'reindex concurrently');
DROP INDEX kw_ix;
CREATE INDEX CONCURRENTLY kw_ix ON kw_t USING sift_col (id, m, d, v, pad) WITH (key_cut = 'm');
SELECT kw_note(:n, 'concurrently');
DROP INDEX kw_ix;
DROP TABLE kw_t;

SELECT n, build, workers, key_in_one_unit FROM kw_res ORDER BY n, build;

RESET maintenance_work_mem;
RESET sift.test_col_segment_kb;
RESET sift.test_col_spill_run_kb;
DROP TABLE kw_res;
DROP FUNCTION kw_note(int, text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
