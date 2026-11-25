-- sift.test_limits lowers the leaves a sift_table or a sift_col index holds, so that the guards run on a few columns: CREATE TABLE and the
-- ALTER TABLE that would pass the limit are refused alike, DROP COLUMN is not checked
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
\set VERBOSITY terse
SET sift.test_limits = 'col_leaves=6';
CREATE TABLE five (a int, b int, c int, d int, e int) USING sift_table;
CREATE TABLE six (a int, b int, c int, d int, e int, f int) USING sift_table;
INSERT INTO five VALUES (1, 2, 3, 4, 5);
ALTER TABLE five ADD COLUMN f int;
ALTER TABLE five DROP COLUMN e;
ALTER TABLE five ADD COLUMN f int;
ALTER TABLE five ALTER COLUMN a TYPE bigint;
INSERT INTO five VALUES (6, 7, 8, 9, 10);
SELECT count(*), sum(a) FROM five;

CREATE TABLE items (a int, b int, c int, d int, e int, f int, g int);
CREATE INDEX items_6 ON items USING sift_col (a, b, c, d, e, f);
CREATE INDEX items_7 ON items USING sift_col (a, b, c, d, e, f, g);

RESET sift.test_limits;
ALTER TABLE five ADD COLUMN g int;
DROP TABLE five, items;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
