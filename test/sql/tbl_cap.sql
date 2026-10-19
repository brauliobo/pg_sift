-- The directory of a table holds a limited number of segments. A load in many transactions closes a write buffer about every 2 MB and
-- promotes it into a column segment: the writers merge those as they go, so the load does not stop at the limit (it was 126 segments, VACUUM
-- the only merge). The limit is lowered here (a testing setting) to reach it with a few dozen segments.
CREATE EXTENSION pg_sift;
SET sift.table_max_segments = 24;
SET sift.table_write_buffer_limit = 2048;
CREATE TABLE cap (id int, v text) USING sift_table;
CREATE PROCEDURE load_cap(t regclass, batches int) LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1 .. batches LOOP
		EXECUTE format('INSERT INTO %s SELECT g, repeat(''x'', 1000) || g FROM generate_series(%s, %s) g', t, (i - 1) * 2200 + 1, i * 2200);
		COMMIT;
	END LOOP;
END $$;
CALL load_cap('cap', 60);
SELECT count(*), sum(length(v)) FROM cap;
SELECT count(*) < 24 AS within_the_limit, count(*) FILTER (WHERE kind = 'columns') > 0 AS promoted FROM sift.table_segments('cap');
-- the rows are the loaded ones
SELECT count(*) AS wrong FROM cap WHERE v <> repeat('x', 1000) || id;
-- one transaction does not stop at the limit either: the write buffers that one command wrote become column groups that carry their
-- writer while it runs (tbl_bigtx)
CREATE TABLE cap_one (id int, v text) USING sift_table;
INSERT INTO cap_one SELECT g, repeat('x', 1000) || g FROM generate_series(1, 60 * 2200) g;
SELECT count(*), sum(length(v)) FROM cap_one;
-- the rows of many commands wait for the transaction in groups that carry the version of each row: the loop writes all of it
CREATE TABLE cap_cmds (id int, v text) USING sift_table;
DO $$
BEGIN
	FOR i IN 1 .. 60 * 2200 LOOP
		INSERT INTO cap_cmds VALUES (i, repeat('x', 1000) || i);
	END LOOP;
END $$;
SELECT count(*), sum(id) FROM cap_cmds;
DROP TABLE cap, cap_one, cap_cmds;
DROP PROCEDURE load_cap;
DROP EXTENSION pg_sift;
