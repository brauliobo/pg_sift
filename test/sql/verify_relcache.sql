-- sift.col_verify's heap check must not use the index's cached state across tuples: a session that updates the index's statistics (VACUUM, ANALYZE)
-- sends an invalidation that reloads the index's relcache entry and frees that state. sift.test_col_verify_inval does it at every tuple, and
-- first overwrites the cached state, so a check that kept it reads garbage.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE TABLE cv (id int PRIMARY KEY, g int, v float8, t text);
INSERT INTO cv SELECT i, i % 7, i / 3.0, 'row ' || i FROM generate_series(1, 2000) i;
CREATE INDEX cv_col ON cv USING sift_col (id, g, v, t);
SELECT sift.col_verify('cv_col', true);
SET sift.test_col_verify_inval = on;
SELECT sift.col_verify('cv_col', true);
SELECT sift.col_verify('cv_col', true);
RESET sift.test_col_verify_inval;
SELECT sift.col_verify('cv_col', true);
DROP TABLE cv;
DROP EXTENSION pg_sift_test;
