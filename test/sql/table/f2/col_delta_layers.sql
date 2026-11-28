-- The map of a part's column deltas is laid in layers over the map the backend cached: statements of one transaction that patch overlapping rows and columns, with a savepoint rolled back and
-- commits in between, read back as the heap twin does after each statement, through scans and through the row fetches of the updates.
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;

CREATE TABLE l (id int PRIMARY KEY, a int, s text, f float8, pad text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE lh (LIKE l INCLUDING ALL);
INSERT INTO l SELECT g, g, 's' || g, g / 4.0, repeat('x', 30) FROM generate_series(1, 6000) g;
INSERT INTO lh SELECT * FROM l;
SELECT sift.table_maintain('l') IS NOT NULL AS maintained;

CREATE FUNCTION same() RETURNS boolean LANGUAGE sql AS $$
	SELECT NOT EXISTS (SELECT * FROM l EXCEPT SELECT * FROM lh) AND NOT EXISTS (SELECT * FROM lh EXCEPT SELECT * FROM l) $$;
CREATE FUNCTION twin(sql text) RETURNS boolean LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE replace(sql, 'TBL', 'l');
	EXECUTE replace(sql, 'TBL', 'lh');
	RETURN same();
END $$;

BEGIN;
SELECT twin('UPDATE TBL SET a = a + 1 WHERE id % 3 = 0') AS own_1;
SELECT twin('UPDATE TBL SET s = s || ''x'' WHERE id % 5 = 0') AS own_2;
SELECT twin('UPDATE TBL SET f = f * 2 WHERE id % 7 = 0') AS own_3;
SELECT twin('UPDATE TBL SET a = a + 10 WHERE id % 3 = 0') AS own_4;
SELECT twin('UPDATE TBL SET s = ''s'' || id, a = -a WHERE id % 2 = 0') AS own_5;
SAVEPOINT p;
UPDATE l SET a = 0, s = 'gone' WHERE id % 4 = 1;
ROLLBACK TO p;
SELECT same() AS after_rollback;
SELECT twin('UPDATE TBL SET f = NULL WHERE id % 11 = 0') AS own_6;
COMMIT;
SELECT same() AS committed;

SELECT twin('UPDATE TBL SET a = a * 3 WHERE id % 6 = 0') AS next_1;
SELECT twin('UPDATE TBL SET s = NULL WHERE id % 9 = 0') AS next_2;

BEGIN;
SELECT k, twin(format('UPDATE TBL SET a = a + %s WHERE id %% %s = 0', k, k + 1)) AS many FROM generate_series(1, 9) k;
SELECT twin('UPDATE TBL SET s = s || id WHERE id < 50') AS many_last;
COMMIT;
SELECT same() AS settled, count(*) AS check_problems FROM sift.check('l'::regclass, true);

DROP TABLE l, lh;
DROP FUNCTION same(), twin(text);
DROP EXTENSION pg_sift;
