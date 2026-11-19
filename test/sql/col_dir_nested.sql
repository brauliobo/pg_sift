-- Partitions recorded for keys of one, two and three components: a build records them with the bound each starts at, queries by a prefix of the key answer as the heap does, and
-- deletes, VACUUM, a rewrite and a merge keep the bounds true (col_verify); a key of three components whose value holds more rows than a partition is cut inside the value, at a row address
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.test_col_segment_kb = 1024;
SET sift.col_auto_merge_partition_kb = 512;
SET sift.test_col_net = off;
SET statement_timeout = '120s';

CREATE TABLE nd (id int PRIMARY KEY, a int NOT NULL, b int NOT NULL, c int NOT NULL, v int, w int) WITH (autovacuum_enabled = false);
INSERT INTO nd SELECT g, 1 + g % 3, 1 + (g::bigint * 7919 % 5)::int, (g::bigint * 2654435761 % 4294967296 / 65536)::int % 4000, g % 100, (g::bigint * 40503 % 1000003)::int
	FROM generate_series(1, 300000) g;

CREATE FUNCTION chk(OUT same_as_heap bool) LANGUAGE plpgsql AS $$
DECLARE a text; o text; q text;
BEGIN
	same_as_heap := true;
	FOREACH q IN ARRAY ARRAY[
		'SELECT a, count(*), sum(v) FROM nd GROUP BY 1 ORDER BY 1',
		'SELECT count(*), sum(w), min(id), max(id) FROM nd WHERE a = 2',
		'SELECT count(*), sum(w) FROM nd WHERE a = 2 AND b = 3',
		'SELECT count(*), sum(w) FROM nd WHERE a = 2 AND b = 3 AND c BETWEEN 1000 AND 1500',
		'SELECT id FROM nd WHERE a = 3 AND b = 1 AND c = 17 ORDER BY id']
	LOOP
		PERFORM set_config('sift.enable_agg', 'on', true);
		EXECUTE 'SELECT string_agg(r::text, '';'') FROM (' || q || ') r' INTO a;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE 'SELECT string_agg(r::text, '';'') FROM (' || q || ') r' INTO o;
		same_as_heap := same_as_heap AND a IS NOT DISTINCT FROM o;
	END LOOP;
END $$;

CREATE INDEX nd1 ON nd USING sift_col (id, a, b, c, v, w) WITH (key_cut = 'a', dead_rewrite_pct = 100);
CREATE INDEX nd2 ON nd USING sift_col (id, a, b, c, v, w) WITH (key_cut = 'a, b', dead_rewrite_pct = 100);
CREATE INDEX nd3 ON nd USING sift_col (id, a, b, c, v, w) WITH (key_cut = 'a, b, c', dead_rewrite_pct = 100);
VACUUM (FREEZE, ANALYZE) nd;

SELECT i, (SELECT count(*) FROM sift._test_dir_parts(i)) > 2 AS recorded, (SELECT bool_and(rows_from_lo AND rows_below_next) FROM sift._test_col_part_bounds(i)) AS rows_inside,
	sift.col_verify(i, true) FROM unnest(ARRAY['nd1', 'nd2', 'nd3']) i;
SELECT * FROM chk();

DELETE FROM nd WHERE id % 7 = 3;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, INDEX_CLEANUP ON) nd;
SELECT sift.col_verify(i, true) FROM unnest(ARRAY['nd1', 'nd2', 'nd3']) i;
SELECT * FROM chk();

SELECT sift.col_maintain(i) AS maintained FROM unnest(ARRAY['nd1', 'nd2', 'nd3']) i;
SELECT i, (SELECT bool_and(rows_from_lo AND rows_below_next) FROM sift._test_col_part_bounds(i)) AS rows_inside, sift.col_verify(i, true) FROM unnest(ARRAY['nd1', 'nd2', 'nd3']) i;
SELECT * FROM chk();

SELECT sift.col_merge(i) >= 0 AS merged FROM unnest(ARRAY['nd1', 'nd2', 'nd3']) i;
SELECT i, (SELECT count(*) FROM sift._test_dir_parts(i)) > 2 AS recorded, sift.col_verify(i, true) FROM unnest(ARRAY['nd1', 'nd2', 'nd3']) i;
SELECT * FROM chk();

-- one value of the key holds more rows than a partition: the cuts inside it are told by the row address
CREATE TABLE nh (id int PRIMARY KEY, a int NOT NULL, b int NOT NULL, c int NOT NULL, w int) WITH (autovacuum_enabled = false);
INSERT INTO nh SELECT g, CASE WHEN g % 10 = 0 THEN 2 ELSE 1 END, 1, 1, (g::bigint * 40503 % 1000003)::int FROM generate_series(1, 300000) g;
CREATE INDEX nh_idx ON nh USING sift_col (id, a, b, c, w) WITH (key_cut = 'a, b, c', dead_rewrite_pct = 100);
VACUUM (FREEZE, ANALYZE) nh;
SELECT (SELECT count(*) FROM sift._test_dir_parts('nh_idx')) > 2 AS recorded, (SELECT bool_and(rows_from_lo AND rows_below_next) FROM sift._test_col_part_bounds('nh_idx')) AS rows_inside,
	sift.col_verify('nh_idx', true);
DELETE FROM nh WHERE id % 3 = 0;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, INDEX_CLEANUP ON) nh;
SELECT sift.col_maintain('nh_idx') AS maintained, sift.col_verify('nh_idx', true);
SELECT (SELECT count(*) FROM nh WHERE a = 1 AND b = 1 AND c = 1) = (SELECT count(*) FROM nh WHERE a = 1);

SELECT count(DISTINCT lo) < count(*) AS cut_inside_a_value FROM sift._test_col_part_bounds('nh_idx');
-- a key of four components is past the depth a bound holds (three and the address): the bound keeps the first three, partitions that meet in them share it, and col_verify accepts that
CREATE TABLE nc (id int PRIMARY KEY, a int NOT NULL, b int NOT NULL, c int NOT NULL, d int NOT NULL, w int) WITH (autovacuum_enabled = false);
INSERT INTO nc SELECT g, 1 + g % 2, 1, 1, (g::bigint * 2654435761 % 4294967296 / 65536)::int % 100, (g::bigint * 40503 % 1000003)::int FROM generate_series(1, 300000) g;
CREATE INDEX nc_idx ON nc USING sift_col (id, a, b, c, d, w) WITH (key_cut = 'a, b, c, d', dead_rewrite_pct = 100);
VACUUM (FREEZE, ANALYZE) nc;
SELECT (SELECT count(*) FROM sift._test_dir_parts('nc_idx')) >= 2 AS recorded, sift.col_verify('nc_idx', true);
DELETE FROM nc WHERE id % 3 = 0;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, INDEX_CLEANUP ON) nc;
SELECT sift.col_maintain('nc_idx') AS maintained, (SELECT count(DISTINCT lo) < count(*) FROM sift._test_col_part_bounds('nc_idx')) AS shared_bounds, sift.col_verify('nc_idx', true);
SELECT sift.col_maintain('nc_idx') AS maintained_again, sift.col_verify('nc_idx', true);
CREATE FUNCTION chk_nc(OUT same_as_heap bool) LANGUAGE plpgsql AS $$
DECLARE a text; o text; q text := 'SELECT a, d / 10, count(*), sum(w) FROM nc WHERE b = 1 AND c = 1 GROUP BY 1, 2 ORDER BY 1, 2';
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE 'SELECT string_agg(r::text, '';'') FROM (' || q || ') r' INTO a;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE 'SELECT string_agg(r::text, '';'') FROM (' || q || ') r' INTO o;
	same_as_heap := a IS NOT DISTINCT FROM o;
END $$;
SELECT * FROM chk_nc();
INSERT INTO nc SELECT 300000 + g, 1 + g % 2, 1, 1, (g::bigint * 40503 % 100003)::int % 100, g FROM generate_series(1, 60000) g;
DELETE FROM nc WHERE id % 5 = 1;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, INDEX_CLEANUP ON) nc;
SELECT sift.col_maintain('nc_idx') AS maintained_over_shared_bounds, sift.col_maintain('nc_idx') AS and_again, sift.col_verify('nc_idx', true);
SELECT * FROM chk_nc();

DROP FUNCTION chk(), chk_nc();
DROP TABLE nd, nh, nc;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
