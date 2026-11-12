-- The peers of a parallel key_cut build (each holds every key) are merged by the rewrite that ends the build, whatever share of the heap each participant took:
-- the participants take superblocks as fast as they can, so their ranges are of different lengths (one of them may end a superblock short of the other), and the
-- rewrite must not wait for ranges of the same length. Twelve builds of the same table, none of which may leave its peers.
CREATE EXTENSION pg_sift;

CREATE TABLE pu (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off);
INSERT INTO pu SELECT g, g % 500, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, 200000) g;
VACUUM (FREEZE, ANALYZE) pu;
UPDATE pu SET amount = amount + 1 WHERE id % 3 = 0;
VACUUM pu;
SET min_parallel_table_scan_size = 0;
SET max_parallel_maintenance_workers = 4;

CREATE FUNCTION pu_unmerged_builds(n int) RETURNS int LANGUAGE plpgsql AS $$
DECLARE
	unmerged int := 0;
BEGIN
	FOR i IN 1..n LOOP
		DROP INDEX IF EXISTS pu_i;
		CREATE INDEX pu_i ON pu USING sift_col (id, grp, amount, pad) WITH (key_cut = grp);
		IF (SELECT count(*) FROM sift.col_segments('pu_i') WHERE kind = 'immutable') > 1 THEN
			unmerged := unmerged + 1;
		END IF;
	END LOOP;
	RETURN unmerged;
END $$;

SELECT pu_unmerged_builds(12) AS builds_that_left_peers;

DROP FUNCTION pu_unmerged_builds(int);
DROP TABLE pu;
DROP EXTENSION pg_sift;
