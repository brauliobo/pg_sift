-- The three primitives of mutation on format major 2: rows staged in memory by the transaction (nothing is written for a rollback), L0 slabs and micro-parts, and copy on write
-- deletion vectors with a buffer of pending deletes; the visibility of the result; the subtraction of dead rows from a pre-aggregate; the compaction that drops them.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);
SELECT sift._test_f2_init('t_idx');

-- Staged rows: a rolled-back transaction and a rolled-back savepoint write no WAL and leave no part
SELECT pg_current_wal_insert_lsn() AS lsn0 \gset
BEGIN;
SELECT sift._test_f2_stage('t_idx', 500, 0, 1);
SAVEPOINT a;
SELECT sift._test_f2_stage('t_idx', 100, 10000, 100000);
SELECT sift._test_f2_stage_rows('t_idx');
ROLLBACK TO a;
SELECT sift._test_f2_stage_rows('t_idx');
ROLLBACK;
SELECT sift._test_f2_stage_rows('t_idx');
SELECT pg_current_wal_insert_lsn() = :'lsn0' AS no_wal;
SELECT part_id, nparts, total_rows, has_slab FROM sift._test_f2_partitions('t_idx');

-- A commit writes the rows that survived: the released savepoint's rows are kept, the rolled back ones are not
BEGIN;
SELECT sift._test_f2_stage('t_idx', 500, 0, 1);
SAVEPOINT a;
SELECT sift._test_f2_stage('t_idx', 100, 10000, 100000);
RELEASE a;
SAVEPOINT b;
SELECT sift._test_f2_stage('t_idx', 50, 20000, 200000);
ROLLBACK TO b;
COMMIT;
SELECT part_id, nparts, total_rows, l0_parts, has_slab FROM sift._test_f2_partitions('t_idx');
SELECT level, nrows, ndead, key_lo, key_hi FROM sift._test_f2_parts('t_idx', 1, NULL);

-- A small batch lands in the slab of the partition; flushing it makes a micro-part
CREATE TABLE s (v int);
CREATE INDEX s_idx ON s USING sift_store_test (v);
SELECT sift._test_f2_init('s_idx');
BEGIN; SELECT sift._test_f2_stage('s_idx', 10, 0, 1); COMMIT;
SELECT nparts, total_rows, has_slab FROM sift._test_f2_partitions('s_idx');
SELECT sift._test_f2_slab('s_idx', 1, NULL);
BEGIN; SELECT sift._test_f2_stage('s_idx', 10, 100, 100); COMMIT;
SELECT sift._test_f2_slab('s_idx', 1, NULL);
SELECT sift._test_f2_slab_flush('s_idx', 1, NULL);
SELECT nparts, total_rows, l0_parts, has_slab FROM sift._test_f2_partitions('s_idx');
SELECT sift._test_f2_slab_flush('s_idx', 1, NULL);
SELECT sift._test_f2_check('s_idx');

-- Deletes: below 64 rows they wait in the pending buffer and are subtracted when counting; more go straight to deletion vectors
CREATE TABLE d (v int);
CREATE INDEX d_idx ON d USING sift_store_test (v);
SELECT sift._test_f2_init('d_idx');
SELECT sift._test_f2_add_part('d_idx', 1, NULL, 20000, 0, 1) > 0 AS added;
SELECT min(part_gen) AS g FROM sift._test_f2_parts('d_idx', 1, NULL) \gset
SELECT visible, judged FROM sift._test_f2_visible('d_idx', 1, NULL, :g);
SELECT sift._test_f2_delete('d_idx', 1, NULL, 1, 5, 40, 3);
SELECT sift._test_f2_pending('d_idx', 1, NULL);
SELECT ndead, has_dv FROM sift._test_f2_parts('d_idx', 1, NULL);
SELECT visible, judged FROM sift._test_f2_visible('d_idx', 1, NULL, :g);
SELECT sift._test_f2_candidates('d_idx', 1, NULL);

-- The kinds of deletion vector: a few rows (array), many (bitmap), all (all dead)
SELECT sift._test_f2_delete('d_idx', 1, NULL, 1, 4000, 5000, 1);
SELECT ndead, has_dv FROM sift._test_f2_parts('d_idx', 1, NULL);
SELECT * FROM sift._test_f2_dv('d_idx', 1, NULL, :g);
SELECT visible, judged FROM sift._test_f2_visible('d_idx', 1, NULL, :g);
SELECT sift._test_f2_delete('d_idx', 1, NULL, 1, 16384, 3616, 1);
SELECT * FROM sift._test_f2_dv('d_idx', 1, NULL, :g) ORDER BY granule;
SELECT sift._test_f2_candidates('d_idx', 1, NULL);

-- Compaction drops the dead rows into a new generation of the partition
SELECT done, rows_in, rows_out, dead_dropped, aborted_dropped FROM sift._test_f2_compact('d_idx', 1, NULL, ARRAY[:g]);
SELECT level, nrows, ndead, has_dv FROM sift._test_f2_parts('d_idx', 1, NULL);
SELECT sift._test_f2_check('d_idx');

-- A delete by a transaction that rolled back does not hide the row; per-row visibility is used until the deleter is settled
CREATE TABLE a (v int);
CREATE INDEX a_idx ON a USING sift_store_test (v);
SELECT sift._test_f2_init('a_idx');
SELECT sift._test_f2_add_part('a_idx', 1, NULL, 20000, 100000, 500000, 0, 1, 0) > 0 AS added;
SELECT min(part_gen) AS g2 FROM sift._test_f2_parts('a_idx', 1, NULL) \gset
BEGIN;
SELECT txid_current() AS x \gset
SELECT sift._test_f2_delete('a_idx', 1, NULL, 500000, 0, 200, 1, :x);
SELECT visible, judged FROM sift._test_f2_visible('a_idx', 1, NULL, :g2);
ROLLBACK;
SELECT visible FROM sift._test_f2_visible('a_idx', 1, NULL, :g2);
SELECT granule, kind, ndead, unsettled FROM sift._test_f2_dv('a_idx', 1, NULL, :g2);
SELECT sift._test_f2_check('a_idx');

DROP TABLE t;
DROP TABLE s;
DROP TABLE d;
DROP TABLE a;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
