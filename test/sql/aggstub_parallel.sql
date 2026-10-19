-- The column source of sift_colstub opens in a parallel worker (the executor splits the superblocks of a table into morsels):
-- every worker has its own source and its own reads of the heap, and the answer does not depend on who reads which superblock.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE TABLE w (id int, s text, d date, metrics jsonb, items jsonb) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO w SELECT i, 'v' || i % 13, date '2024-01-01' + i % 90, jsonb_build_object('a', i, 'b', i % 7),
	   jsonb_build_array(jsonb_build_object('c', 'c' || i % 5, 'q', i % 4), jsonb_build_object('c', 'd' || i % 3, 'q', i % 2))
FROM generate_series(1, 12000) i;
CREATE INDEX w_col ON w USING sift_colstub (id, s, d, metrics, items) WITH (paths = 'metrics: *:float8; items: []{c text, q int4}');
ANALYZE w;

-- the morsels: one per superblock; the reference is read by this backend
CREATE TABLE morsels AS SELECT sb::int AS sb FROM generate_series(0, (SELECT count(*) FROM sift._test_aggstub_units('w_col')) - 1) sb;
ALTER TABLE morsels SET (parallel_workers = 2);
CREATE TABLE reference AS SELECT sb, r.rows, r.checksum, r.in_worker FROM morsels, LATERAL sift._test_aggstub_sb('w_col', sb) r;
SELECT count(*) > 3 AS several_morsels, sum(rows) = (SELECT count(*) FROM w) AS all_rows, NOT bool_or(in_worker) AS read_by_the_backend FROM reference;

SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET parallel_leader_participation = off;
EXPLAIN (COSTS OFF) SELECT m.sb, r.rows, r.checksum, r.in_worker FROM morsels m, LATERAL sift._test_aggstub_sb('w_col', m.sb) r;
CREATE TABLE parallel_read AS SELECT m.sb, r.rows, r.checksum, r.in_worker FROM morsels m, LATERAL sift._test_aggstub_sb('w_col', m.sb) r;
SELECT bool_and(in_worker) AS all_read_by_workers FROM parallel_read;
SELECT count(*) AS differences FROM ((SELECT sb, rows, checksum FROM parallel_read EXCEPT SELECT sb, rows, checksum FROM reference)
									 UNION ALL (SELECT sb, rows, checksum FROM reference EXCEPT SELECT sb, rows, checksum FROM parallel_read)) x;

-- the same through the debug mode that runs the plan in a worker
RESET parallel_leader_participation;
SELECT max(sb) AS last_sb, (SELECT count(*) FROM w) AS nrows FROM morsels \gset
SET debug_parallel_query = on;
SELECT bool_and(r.in_worker) AS in_a_worker, sum(r.rows) = :nrows AS all_rows
FROM generate_series(0, :last_sb) sb, LATERAL sift._test_aggstub_sb('w_col', sb) r;
RESET debug_parallel_query;

-- the decoded leaves, element leaves and dictionary codes included, come out the same from a worker
CREATE TABLE dump_serial AS SELECT unit, ord, elem, tid, code, val FROM sift._test_aggstub_dump('w_col', 7);
CREATE TABLE dump_serial_s AS SELECT unit, ord, elem, tid, code, val FROM sift._test_aggstub_dump('w_col', 1);
SET debug_parallel_query = on;
EXPLAIN (COSTS OFF) SELECT count(*) FROM sift._test_aggstub_dump('w_col', 7);
SELECT count(*) AS dump_differences FROM (
	(SELECT unit, ord, elem, tid, code, val FROM sift._test_aggstub_dump('w_col', 7) EXCEPT SELECT * FROM dump_serial)
	UNION ALL (SELECT * FROM dump_serial EXCEPT SELECT unit, ord, elem, tid, code, val FROM sift._test_aggstub_dump('w_col', 7))
	UNION ALL (SELECT unit, ord, elem, tid, code, val FROM sift._test_aggstub_dump('w_col', 1) EXCEPT SELECT * FROM dump_serial_s)
	UNION ALL (SELECT * FROM dump_serial_s EXCEPT SELECT unit, ord, elem, tid, code, val FROM sift._test_aggstub_dump('w_col', 1))) x;
RESET debug_parallel_query;

DROP TABLE w, morsels, reference, parallel_read, dump_serial, dump_serial_s;
-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
