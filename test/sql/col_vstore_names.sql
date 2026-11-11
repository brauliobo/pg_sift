-- Stores of tables and columns whose names are at the length limit, of the partitions of a partitioned table and of tables in other schemas: each gets its own
-- store, every shape answers as the heap, and the stores go with the tables.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;
SET sift.test_vstore_columns = '*';

CREATE SCHEMA vn_s;
CREATE TABLE vn_s.a_table_with_a_name_that_is_exactly_sixty_three_bytes_long_x (
	id int PRIMARY KEY, a_column_with_a_name_that_is_exactly_sixty_three_bytes_lon text);
CREATE TABLE vn_s.a_table_with_a_name_that_is_exactly_sixty_three_bytes_long_y (
	id int PRIMARY KEY, a_column_with_a_name_that_is_exactly_sixty_three_bytes_lon text);
INSERT INTO vn_s.a_table_with_a_name_that_is_exactly_sixty_three_bytes_long_x
	SELECT i, 'https://example.com/' || md5((i % 700)::text) FROM generate_series(1, 4000) i;
INSERT INTO vn_s.a_table_with_a_name_that_is_exactly_sixty_three_bytes_long_y
	SELECT i, 'https://example.com/' || md5((i % 300)::text) FROM generate_series(1, 4000) i;
CREATE INDEX vn_x1 ON vn_s.a_table_with_a_name_that_is_exactly_sixty_three_bytes_long_x USING sift_col (id, a_column_with_a_name_that_is_exactly_sixty_three_bytes_lon);
CREATE INDEX vn_x2 ON vn_s.a_table_with_a_name_that_is_exactly_sixty_three_bytes_long_x USING sift_col (a_column_with_a_name_that_is_exactly_sixty_three_bytes_lon, id);
CREATE INDEX vn_y1 ON vn_s.a_table_with_a_name_that_is_exactly_sixty_three_bytes_long_y USING sift_col (id, a_column_with_a_name_that_is_exactly_sixty_three_bytes_lon);
CREATE INDEX vn_y2 ON vn_s.a_table_with_a_name_that_is_exactly_sixty_three_bytes_long_y USING sift_col (a_column_with_a_name_that_is_exactly_sixty_three_bytes_lon, id);

SELECT (SELECT length(relname) FROM pg_class WHERE oid = store) <= 63 AS short_enough, indexes_using, table_name::text LIKE 'vn_s.%' AS in_schema
  FROM sift.col_value_store_status ORDER BY table_name::text;

CREATE FUNCTION vn_same(t regclass, col text, val text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE format('SELECT count(*) FROM %s WHERE %I = %L', t, col, val) INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT count(*) FROM %s WHERE %I = %L', t, col, val) INTO b;
	RETURN a = b AND a > 0;
END $$;
SELECT vn_same('vn_s.a_table_with_a_name_that_is_exactly_sixty_three_bytes_long_x', 'a_column_with_a_name_that_is_exactly_sixty_three_bytes_lon', 'https://example.com/' || md5('5')),
       vn_same('vn_s.a_table_with_a_name_that_is_exactly_sixty_three_bytes_long_y', 'a_column_with_a_name_that_is_exactly_sixty_three_bytes_lon', 'https://example.com/' || md5('5'));

-- the partitions of a partitioned table: a store for each
CREATE TABLE vn_p (id int, k int, url text) PARTITION BY RANGE (k);
CREATE TABLE vn_p1 PARTITION OF vn_p FOR VALUES FROM (0) TO (5);
CREATE TABLE vn_p2 PARTITION OF vn_p FOR VALUES FROM (5) TO (10);
INSERT INTO vn_p SELECT i, i % 10, 'https://example.com/' || md5((i % 200)::text) FROM generate_series(1, 6000) i;
ANALYZE vn_p;
CREATE INDEX vn_p1_a ON vn_p1 USING sift_col (id, k, url);
CREATE INDEX vn_p1_b ON vn_p1 USING sift_col (url, id);
CREATE INDEX vn_p2_a ON vn_p2 USING sift_col (id, k, url);
CREATE INDEX vn_p2_b ON vn_p2 USING sift_col (url, id);
SELECT table_name::text, indexes_using FROM sift.col_value_store_status WHERE table_name::text LIKE 'vn\_p%' ORDER BY 1;
SELECT vn_same('vn_p', 'url', 'https://example.com/' || md5('7'));
ALTER TABLE vn_p DETACH PARTITION vn_p2;
SELECT vn_same('vn_p', 'url', 'https://example.com/' || md5('3')), vn_same('vn_p2', 'url', 'https://example.com/' || md5('9'));
DROP TABLE vn_p;
SELECT table_name::text, indexes_using FROM sift.col_value_store_status WHERE table_name::text LIKE 'vn\_p%' ORDER BY 1;
DROP TABLE vn_p2;

DROP SCHEMA vn_s CASCADE;
SELECT count(*) AS stores_left FROM sift.col_value_store_status;
SELECT count(*) AS relations_left FROM pg_class WHERE relname LIKE '%sift\_values%';

DROP FUNCTION vn_same(regclass, text, text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
