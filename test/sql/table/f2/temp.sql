-- TEMP sift_table and sift_col: a table with no block yet is Format 2.0 for every function of sift.*, and stays so after sift.maintain; ON COMMIT PRESERVE ROWS, DELETE ROWS and DROP,
-- TRUNCATE inside a savepoint and DISCARD TEMP all give what a heap twin gives; a unique sift_col index of a temporary table still refuses a duplicate.
CREATE EXTENSION pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

-- an empty one: nothing to maintain, and the first insert still makes Format 2.0
CREATE TEMP TABLE tmp_empty (id int, v text) USING sift_table;
SELECT format_note, state FROM sift.status('tmp_empty');
SELECT busy, dead_rows, promoted, merged FROM sift.maintain('tmp_empty');
SELECT count(*) AS check_rows FROM sift.check('tmp_empty');
SELECT count(*) AS part_rows FROM sift.parts('tmp_empty');
SELECT relation_blocks, parts FROM sift.space('tmp_empty');
SELECT busy, dead_rows, promoted, merged FROM sift.maintain('tmp_empty');
INSERT INTO tmp_empty SELECT g, 'x' || g FROM generate_series(1, 100) g;
SELECT format_major, format_minor, format_note FROM sift.status('tmp_empty');
SELECT busy, dead_rows, promoted, merged FROM sift.maintain('tmp_empty');
SELECT count(*) AS check_rows FROM sift.check('tmp_empty');
UPDATE tmp_empty SET v = 'y' WHERE id < 5;
DELETE FROM tmp_empty WHERE id > 90;
SELECT count(*), count(*) FILTER (WHERE v = 'y') AS updated FROM tmp_empty;
TRUNCATE tmp_empty;
SELECT format_note FROM sift.status('tmp_empty');
SELECT busy, dead_rows, promoted, merged FROM sift.maintain('tmp_empty');
INSERT INTO tmp_empty SELECT g, 'z' || g FROM generate_series(1, 10) g;
SELECT format_major, format_minor, format_note FROM sift.status('tmp_empty');
SELECT count(*) FROM tmp_empty;
DROP TABLE tmp_empty;

-- ON COMMIT, against heap twins
CREATE TEMP TABLE twin_preserve (id int, v text) ON COMMIT PRESERVE ROWS;
CREATE TEMP TABLE twin_delete (id int, v text) ON COMMIT DELETE ROWS;
CREATE TEMP TABLE sift_preserve (id int, v text) USING sift_table ON COMMIT PRESERVE ROWS;
CREATE TEMP TABLE sift_delete (id int, v text) USING sift_table ON COMMIT DELETE ROWS;
CREATE FUNCTION counts() RETURNS TABLE (tbl text, rows_sift bigint, rows_heap bigint) LANGUAGE sql AS $$
	SELECT 'preserve', (SELECT count(*) FROM sift_preserve), (SELECT count(*) FROM twin_preserve)
	UNION ALL SELECT 'delete', (SELECT count(*) FROM sift_delete), (SELECT count(*) FROM twin_delete) $$;
BEGIN;
CREATE TEMP TABLE sift_drop (id int, v text) USING sift_table ON COMMIT DROP;
INSERT INTO sift_drop SELECT g, 'd' FROM generate_series(1, 50) g;
INSERT INTO sift_preserve SELECT g, 'p' FROM generate_series(1, 50) g;
INSERT INTO twin_preserve SELECT g, 'p' FROM generate_series(1, 50) g;
INSERT INTO sift_delete SELECT g, 'x' FROM generate_series(1, 50) g;
INSERT INTO twin_delete SELECT g, 'x' FROM generate_series(1, 50) g;
SELECT * FROM counts();
COMMIT;
SELECT to_regclass('sift_drop') IS NULL AS dropped;
SELECT * FROM counts();
INSERT INTO sift_delete SELECT g, 'again' FROM generate_series(1, 7) g;
INSERT INTO twin_delete SELECT g, 'again' FROM generate_series(1, 7) g;
SELECT * FROM counts();
SELECT format_major FROM sift.status('sift_delete');

-- a savepoint that truncates and rolls back; a truncate that commits, then the rows again
BEGIN;
INSERT INTO sift_preserve SELECT g, 'q' FROM generate_series(1, 20) g;
INSERT INTO twin_preserve SELECT g, 'q' FROM generate_series(1, 20) g;
SAVEPOINT s;
TRUNCATE sift_preserve, twin_preserve;
INSERT INTO sift_preserve SELECT g, 'r' FROM generate_series(1, 3) g;
INSERT INTO twin_preserve SELECT g, 'r' FROM generate_series(1, 3) g;
SELECT * FROM counts() WHERE tbl = 'preserve';
ROLLBACK TO s;
SELECT * FROM counts() WHERE tbl = 'preserve';
COMMIT;
BEGIN;
TRUNCATE sift_preserve, twin_preserve;
INSERT INTO sift_preserve SELECT g, 's' FROM generate_series(1, 9) g;
INSERT INTO twin_preserve SELECT g, 's' FROM generate_series(1, 9) g;
COMMIT;
SELECT * FROM counts() WHERE tbl = 'preserve';
SELECT count(DISTINCT ctid) = count(*) AS distinct_addresses FROM sift_preserve;
DROP FUNCTION counts();

-- a unique sift_col index of a temporary table
CREATE TEMP TABLE tmp_uniq (id int, v int);
CREATE UNIQUE INDEX tmp_uniq_i ON tmp_uniq USING sift_col (id, v);
INSERT INTO tmp_uniq SELECT g, g FROM generate_series(1, 100) g;
INSERT INTO tmp_uniq VALUES (5, 5);
SELECT count(*) FROM tmp_uniq;
DROP TABLE tmp_uniq;

-- DISCARD TEMP: the name is free again and the new table is not the old one
CREATE TEMP TABLE tmp_again (id int, v text) USING sift_table;
INSERT INTO tmp_again SELECT g, 'old' FROM generate_series(1, 30) g;
SELECT count(*) FROM tmp_again;
DISCARD TEMP;
CREATE TEMP TABLE tmp_again (id int, v text) USING sift_table;
SELECT count(*) FROM tmp_again;
INSERT INTO tmp_again SELECT g, 'new' FROM generate_series(1, 5) g;
SELECT count(*), min(v), max(v) FROM tmp_again;
SELECT format_major, format_note FROM sift.status('tmp_again');
DISCARD TEMP;

DROP EXTENSION pg_sift;
