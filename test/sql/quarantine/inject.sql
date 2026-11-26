-- The test hooks that damage a private copy of a healthy index read it through the real readers and catch what they refuse: that is no damage of the index, so it stays in service and its writes
-- are indexed. Damage of the index that a caller catches still takes it out of service, and the log says why (SQLSTATE and text of the first cause).
CREATE EXTENSION pg_sift_test CASCADE;
-- Format 1: the superunit and point-file damage hooks read Format 1 files (col/v1)
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
SET sift.on_corruption = quarantine;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE inj (id int, kind int, val bigint, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE inj ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO inj SELECT g, g % 6, (g * 977) % 100000, repeat('p', 300) FROM generate_series(1, 9000) g;
CREATE INDEX inj_i ON inj USING sift_col (id, kind, val) WITH (rollup_columns = 'off', write_buffer_limit = 4096, superunit = 3, cube_minmax = false);

SELECT supers > 0 AS has_blocks, damaged > 0 AS damages_made, refused = damaged AS all_refused FROM sift._test_col_super_damage('inj_i');
CREATE INDEX inj_p ON inj USING sift_col (id, kind, val) WITH (write_buffer_limit = 256, point_tid_columns = 'id, kind');
SELECT runs > 0 AS has_sections, damaged > 0 AS damages_made, refused > 0 AS refusals FROM sift._test_col_point_file_damage('inj_p', 9973);
SELECT count(*) AS recorded FROM sift.quarantine;
INSERT INTO inj SELECT g, g % 6, g, repeat('p', 300) FROM generate_series(9001, 9500) g;
SELECT sift.col_verify('inj_i', true);
SELECT count(*) AS recorded FROM sift.quarantine;
DROP TABLE inj;

CREATE TABLE t (id int, v int);
INSERT INTO t SELECT i, i % 7 FROM generate_series(1, 20000) i;
CREATE INDEX t_idx ON t USING sift_col (id, v);
ANALYZE t;
SELECT sift._test_poke('t_idx', 2, 8176, 9, 2);
SET client_min_messages = log;
DO $$
BEGIN
	PERFORM count(*) FROM t WHERE v = 3;
EXCEPTION WHEN OTHERS THEN
	RAISE LOG 'caught %', SQLSTATE;
END $$;
SET client_min_messages = warning;
SELECT index, error_code, detections FROM sift.quarantine;
DROP TABLE t;

DROP EXTENSION pg_sift_test CASCADE;
DROP EXTENSION pg_sift;
