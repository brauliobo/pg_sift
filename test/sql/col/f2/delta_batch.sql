-- The rows of one UPDATE that patch the same columns are written as batch records of the column delta chain (SIFT_CDENT_FLAG_BATCH): a sift_col index on a heap table (the new address of each row
-- and its patched leaves), a sift_table (a string, and a NULL), and the rows are those of the heap twin the same UPDATEs ran on, before and after a VACUUM folds the chain.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;

-- the bytes of the column delta records of the relation
CREATE FUNCTION delta_bytes(rel regclass) RETURNS bigint LANGUAGE sql AS $$
	SELECT coalesce(sum(bytes), 0)::bigint FROM sift.index_size_breakdown(rel) WHERE category = 'col_delta' $$;
CREATE FUNCTION same(a regclass, b regclass) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE x text; y text;
BEGIN
	EXECUTE format('SELECT string_agg(r::text, E''\n'' ORDER BY r::text) FROM %s r', a) INTO x;
	EXECUTE format('SELECT string_agg(r::text, E''\n'' ORDER BY r::text) FROM %s r', b) INTO y;
	RETURN x = y;
END $$;

-- a sift_col index: a patch of 6000 rows is 336,000 bytes of unbatched records of 56 bytes, and 8 bytes or less of a row in batches
CREATE TABLE c (id int NOT NULL, a int, b int, c3 int, s text, f float8, g int, h int, j int);
CREATE TABLE ch (LIKE c);
INSERT INTO c SELECT i, i % 100, i % 7, i, 'row ' || i % 50, i * 0.5, i % 3, i % 11, i FROM generate_series(1, 6000) i;
INSERT INTO ch SELECT * FROM c;
CREATE INDEX c_idx ON c USING sift_col (id, a, b, c3, s, f, g, h, j);
SELECT delta_bytes('c_idx') AS bytes_before;

UPDATE c SET a = a + 1;
UPDATE ch SET a = a + 1;
SELECT 'an integer leaf of every row is a few bytes' AS check, delta_bytes('c_idx') BETWEEN 1 AND 6000 * 8 AS ok;
SELECT 'the rows are those of the twin' AS check, same('c', 'ch') AS ok;

UPDATE c SET s = CASE WHEN id % 5 = 0 THEN 'shipped ' || id ELSE 'shipped' END;
UPDATE ch SET s = CASE WHEN id % 5 = 0 THEN 'shipped ' || id ELSE 'shipped' END;
SELECT 'the rows are those of the twin after the strings' AS check, same('c', 'ch') AS ok;

UPDATE c SET b = NULL;
UPDATE ch SET b = NULL;
SELECT 'a NULL of every row is a few bytes more' AS check, delta_bytes('c_idx') BETWEEN 1 AND 6000 * 16 AS ok;
SELECT 'the rows are those of the twin after the NULLs' AS check, same('c', 'ch') AS ok;
SELECT 'the index agrees with the twin' AS check, (SELECT count(*) FROM c WHERE b IS NULL AND a > 1 AND s LIKE 'shipped%') = (SELECT count(*) FROM ch WHERE b IS NULL AND a > 1 AND s LIKE 'shipped%') AS ok;
VACUUM c;
SELECT 'the rows are those of the twin after VACUUM' AS check, same('c', 'ch') AS ok;

-- a sift_table: a string, then a NULL, 6000 rows each
CREATE TABLE s (id int PRIMARY KEY, a int, b int, c3 int, t text, f float8, g int, h int, j int) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE sh (LIKE s INCLUDING ALL) USING heap;
INSERT INTO s SELECT i, i % 100, i % 7, i, 'row ' || i % 50, i * 0.5, i % 3, i % 11, i FROM generate_series(1, 6000) i;
INSERT INTO sh SELECT * FROM s;
SELECT sift.table_maintain('s') IS NOT NULL AS maintained;

UPDATE s SET t = 'shipped';
UPDATE sh SET t = 'shipped';
SELECT 'a string of every row is a few bytes' AS check, delta_bytes('s') BETWEEN 1 AND 6000 * 8 AS ok;
SELECT 'the rows are those of the twin' AS check, same('s', 'sh') AS ok;

UPDATE s SET g = NULL;
UPDATE sh SET g = NULL;
SELECT 'a NULL of every row is a few bytes more' AS check, delta_bytes('s') BETWEEN 1 AND 6000 * 16 AS ok;
SELECT 'the rows are those of the twin after the NULLs' AS check, same('s', 'sh') AS ok;

UPDATE s SET t = CASE WHEN id % 3 = 0 THEN NULL ELSE 'again ' || id % 4 END, a = a + id % 5 WHERE id <= 4000;
UPDATE sh SET t = CASE WHEN id % 3 = 0 THEN NULL ELSE 'again ' || id % 4 END, a = a + id % 5 WHERE id <= 4000;
SELECT 'the rows are those of the twin after a mixed update' AS check, same('s', 'sh') AS ok;
SELECT sift.table_maintain('s') IS NOT NULL AS maintained;
VACUUM s;
SELECT 'the rows are those of the twin after VACUUM' AS check, same('s', 'sh') AS ok;
SELECT 'sift.check is clean' AS check, count(*) = 0 AS ok FROM sift.check('s'::regclass);

DROP TABLE c, ch, s, sh;
DROP FUNCTION delta_bytes(regclass), same(regclass, regclass);
DROP EXTENSION pg_sift;
