-- The writer cuts a granule at a quarter of the largest allocation (SIFT_GRANULE_BYTES_MAX) of the strings of its rows, the strings of the elements of their arrays of objects included:
-- 3,000 rows of two 50 kB elements (286 MB) are two granules, not one that the encoders of its strings could not allocate.
CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
CREATE UNLOGGED TABLE ge (id int8, items jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ge SELECT i, jsonb_build_array(jsonb_build_object('k', repeat(chr(65 + i % 26), 50000) || i), jsonb_build_object('k', repeat(chr(97 + i % 26), 50000) || i))
FROM generate_series(1, 3000) i;
SET maintenance_work_mem = '1GB';
CREATE INDEX ge_col ON ge USING sift_col (id, items) WITH (paths = 'items[]{k text}');
SELECT count(*) AS parts, sum(rows) AS rows, min(granules) >= 2 AS cut_by_element_bytes FROM sift.parts('ge_col');
DROP TABLE ge;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
