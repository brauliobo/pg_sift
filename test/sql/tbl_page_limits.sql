-- The page checks of a sift_table must accept every page the writers can leave, at the edge of its capacity: a tombstone page that fills up
-- (the reader resumes inside the page it read before, then follows the chain) and a write buffer whose header lists its largest page count.
CREATE EXTENSION pg_sift;
CREATE TABLE tomb (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO tomb SELECT g, 'v' || g FROM generate_series(1, 1500) g;
-- every delete is read back at once: the tombstone log is synced with 1, 2, ... entries on its page, at its capacity and across it
DO $$
BEGIN
	FOR i IN 1 .. 700 LOOP
		DELETE FROM tomb WHERE id = i;
		IF (SELECT count(*) FROM tomb) <> 1500 - i THEN
			RAISE EXCEPTION 'delete % is not seen', i;
		END IF;
	END LOOP;
END $$;
SELECT count(*), min(id), max(id) FROM tomb;
-- a session that did not write them reads the whole chain at once
\c -
SELECT count(*), min(id), max(id) FROM tomb;
-- the largest write buffer: 1024 pages of 8 kB, the page count of its header at the limit
SET sift.table_write_buffer_limit = 8192;
CREATE TABLE wide (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO wide SELECT g, repeat('x', 1000) || g FROM generate_series(1, 12000) g;
SELECT count(*), sum(length(v)) FROM wide;
SELECT count(*) AS wrong FROM wide WHERE v <> repeat('x', 1000) || id;
DROP TABLE tomb, wide;
DROP EXTENSION pg_sift;
