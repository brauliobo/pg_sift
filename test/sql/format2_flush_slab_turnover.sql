-- Small transactions that insert and delete rows of a sift_col index grow its open slab of heap references to the rows a flush is asked at without sealing a page: the next small write turns the
-- slab over into a part instead of reading the open pages as pages of batches (and quarantining the index as damaged).
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE hs (id int PRIMARY KEY, n int) WITH (autovacuum_enabled = false);
INSERT INTO hs SELECT i, i FROM generate_series(1, 6000) i;
CREATE INDEX hs_idx ON hs USING sift_col (id, n);

CREATE PROCEDURE hs_churn(k int) LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..k LOOP
		INSERT INTO hs VALUES (10000 + i, i);
		DELETE FROM hs WHERE id = i;
		COMMIT;
	END LOOP;
END $$;

CALL hs_churn(4300);
INSERT INTO hs VALUES (20000, 1);
SELECT count(*) AS live_rows FROM hs;
SELECT count(*) AS problems FROM sift.check('hs_idx', heapallindexed => true);

DROP TABLE hs;
DROP PROCEDURE hs_churn;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
