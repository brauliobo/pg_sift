CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ': ' || SQLERRM;
END $$;

-- Damage found by a sift_col index is recorded in the registry of the server; sift.on_corruption = error records nothing; a function that reports damage does not record it;
-- an index rebuilt by REINDEX is a new file and leaves the list
SET sift.on_corruption = quarantine;
CREATE TABLE t (id int, v int);
INSERT INTO t SELECT g, g % 7 FROM generate_series(1, 20000) g;
CREATE INDEX t_idx ON t USING sift_col (id, v);
SELECT count(*) AS listed FROM sift.quarantine;

-- the data page of the segment gets another page type
SELECT sift._test_poke('t_idx', 2, 8176, 9, 2);
SELECT expect($$ SELECT sift.col_verify('t_idx') $$);
SELECT count(*) AS listed_after_verify FROM sift.quarantine;
SELECT count(*) AS problems FROM sift.check('t_idx');
SELECT count(*) AS listed_after_check FROM sift.quarantine;

SET sift.on_corruption = error;
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$);
SELECT count(*) AS listed_in_error_mode FROM sift.quarantine;

SET sift.on_corruption = quarantine;
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$);
SELECT index, error_code, reason, detections, skipped_writes, skipped_vacuums, refused_scans FROM sift.quarantine;

REINDEX INDEX t_idx;
SELECT count(*) AS listed_after_reindex FROM sift.quarantine;
DROP TABLE t;

-- the VACUUM of a table whose sift_col index is damaged fails once with the error of the damage and takes the index out of service: the VACUUM that follows skips it (the warning
-- says so), the dead rows are removed from the heap, and the statements that follow give the answers of the heap
SET sift.on_corruption = quarantine;
CREATE TABLE t (id int, v int);
INSERT INTO t SELECT g, g % 7 FROM generate_series(1, 20000) g;
CREATE INDEX t_idx ON t USING sift_col (id, v);
DELETE FROM t WHERE id % 5 = 0;
SELECT count(*) AS rows_before, sum(id) FILTER (WHERE v = 3) AS ids_before FROM t;

-- the liveness run of the segment gets another page type
SELECT first_block AS live FROM sift.col_extents('t_idx') WHERE what = 'liveness run' \gset
SELECT sift._test_poke('t_idx', :live, 8176, 9, 2);
\set VERBOSITY terse
VACUUM t;
\set VERBOSITY default
SELECT index, error_code, detections, skipped_writes, skipped_vacuums, refused_scans FROM sift.quarantine;
SELECT n_dead_tup AS dead_after_vacuum FROM pg_stat_all_tables WHERE relid = 't'::regclass;
SELECT count(*) AS rows_after, sum(id) FILTER (WHERE v = 3) AS ids_after FROM t;

-- a second VACUUM has nothing to do with the index and says so
VACUUM t;
SELECT index, detections, skipped_vacuums FROM sift.quarantine;
VACUUM (ANALYZE) t;

REINDEX INDEX t_idx;
SELECT count(*) AS listed_after_reindex FROM sift.quarantine;
VACUUM t;
SELECT count(*) AS rows_after_reindex FROM t;

DROP TABLE t;

-- An index whose metapage is damaged cannot say anything of itself: it is quarantined by what the metapage shows, and DROP INDEX, REINDEX, REINDEX CONCURRENTLY and
-- the functions that report on indexes still work
SET client_min_messages = warning;
SET sift.on_corruption = quarantine;
CREATE TABLE t (id int, v int);
INSERT INTO t SELECT g, g % 7 FROM generate_series(1, 5000) g;
CREATE INDEX t_idx ON t USING sift_col (id, v);
CREATE INDEX t_bm ON t USING btree (v);

-- the magic number of the metapage
SELECT sift._test_poke('t_idx', 0, 24, 0, 4);
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$) AS detecting_statement;
SELECT index, error_code, reason FROM sift.quarantine;
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$) AS next_statement;
SELECT expect($$ INSERT INTO t VALUES (5001, 1) $$) AS insert;
SELECT expect($$ UPDATE t SET v = 2 WHERE id = 7 $$) AS update;
SELECT expect($$ DELETE FROM t WHERE id = 8 $$) AS delete;
VACUUM t;
SELECT count(*) AS problems FROM sift.check('t_idx');

REINDEX INDEX t_idx;
SELECT count(*) AS listed_after_reindex FROM sift.quarantine;
SELECT sift._test_poke('t_idx', 0, 24, 0, 4);
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$) AS detecting_again;
REINDEX INDEX CONCURRENTLY t_idx;
SELECT count(*) AS listed_after_concurrently FROM sift.quarantine;
SELECT sift._test_poke('t_idx', 0, 24, 0, 4);
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$) AS detecting_a_third_time;
SELECT expect($$ DROP INDEX t_idx $$) AS drop_index;
SELECT count(*) AS listed_after_drop FROM sift.quarantine;

DROP TABLE t;

-- sift.workers, sift.col_index_status and sift.indexes say which indexes are out of service, since when and why; with the metapage of an index damaged they still list it
SET client_min_messages = warning;
SET sift.on_corruption = quarantine;
CREATE TABLE t (id int, v int, body text);
INSERT INTO t SELECT g, g % 7, 'alpha beta' || g % 5 FROM generate_series(1, 20000) g;
CREATE INDEX t_col ON t USING sift_col (id, v);
CREATE INDEX t_bm25 ON t USING sift (body);
CREATE INDEX t_btree ON t (v);
ANALYZE t;

SELECT index, quarantined, quarantined_since IS NULL AS no_since, quarantine_reason FROM sift.workers ORDER BY index::text;
SELECT index, quarantined, quarantined_since IS NULL AS no_since, quarantine_reason FROM sift.col_index_status;
SELECT relation, state FROM sift.indexes ORDER BY relation::text;

-- the data of a segment of the column index
SELECT sift._test_poke('t_col', 2, 8176, 9, 2);
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$) AS detecting_statement;
SELECT index, quarantined, quarantined_since IS NOT NULL AS since, left(quarantine_reason, 40) AS reason FROM sift.workers ORDER BY index::text;
SELECT index, quarantined, quarantined_since IS NOT NULL AS since, left(quarantine_reason, 40) AS reason FROM sift.col_index_status;
SELECT relation, state FROM sift.indexes ORDER BY relation::text;

-- the metapage of the BM25 index: the views read it and still list the index
SELECT sift._test_poke('t_bm25', 0, 24, 0, 4);
SELECT left(expect($$ SELECT count(*) FROM t WHERE body @@> 'alpha' $$), 5) AS detecting_statement;
SELECT index, quarantined FROM sift.workers ORDER BY index::text;
SELECT relation, state FROM sift.indexes ORDER BY relation::text;

REINDEX INDEX t_col;
REINDEX INDEX t_bm25;
SELECT index, quarantined FROM sift.workers ORDER BY index::text;
SELECT relation, state FROM sift.indexes ORDER BY relation::text;

DROP TABLE t;
DROP FUNCTION expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
