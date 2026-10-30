-- The groups of a bulk load are written by the workers of a parallel context when the load has more than one: a group is 8 units of 2 MB write
-- buffers (6500 rows of about 330 bytes each), the backend and its workers take a group each, and the segments, the rows and the TIDs are those of a load alone
CREATE EXTENSION pg_sift;
SET sift.table_write_buffer_limit = 2048;

CREATE TABLE twin AS SELECT g AS id, g % 7 AS grp, 'row ' || g AS v, lpad(g::text, 300, '0') AS pad FROM generate_series(1, 150000) g;
CREATE FUNCTION same_rows(a regclass, b regclass) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE r boolean;
BEGIN
	EXECUTE format('SELECT (SELECT md5(string_agg(a::text, $q$,$q$ ORDER BY id)) FROM %1$s a) = (SELECT md5(string_agg(b::text, $q$,$q$ ORDER BY id)) FROM %2$s b)', a, b) INTO r;
	RETURN r;
END $$;

SET max_parallel_maintenance_workers = 2;
SET sift.test_par_workers = 0;
CREATE TABLE par USING sift_table AS SELECT * FROM twin;
SELECT current_setting('sift.test_par_workers')::int AS workers;
SELECT count(*) AS segments FROM sift.table_segments('par') WHERE kind = 'columns';
SELECT same_rows('par', 'twin');

-- alone: the same segments, the same rows at the same TIDs
SET max_parallel_maintenance_workers = 0;
SET sift.test_par_workers = 0;
CREATE TABLE alone USING sift_table AS SELECT * FROM twin;
SELECT current_setting('sift.test_par_workers')::int AS workers;
SELECT (SELECT array_agg(ctid ORDER BY id) FROM par) = (SELECT array_agg(ctid ORDER BY id) FROM alone) AS same_tids;
SELECT (SELECT count(*) FROM sift.table_segments('par') WHERE kind = 'columns') = (SELECT count(*) FROM sift.table_segments('alone') WHERE kind = 'columns') AS same_segments;

-- a load that rolls back leaves nothing, and the refresh of a materialized view takes the same path
SET max_parallel_maintenance_workers = 2;
BEGIN;
CREATE TABLE gone USING sift_table AS SELECT * FROM twin;
ROLLBACK;
SELECT to_regclass('gone') IS NULL AS rolled_back;
SET sift.test_par_workers = 0;
CREATE MATERIALIZED VIEW mv USING sift_table AS SELECT * FROM twin;
REFRESH MATERIALIZED VIEW mv;
SELECT current_setting('sift.test_par_workers')::int AS workers;
SELECT same_rows('mv', 'twin');

DROP MATERIALIZED VIEW mv;
DROP TABLE twin, par, alone;
DROP FUNCTION same_rows(regclass, regclass);
DROP EXTENSION pg_sift;
