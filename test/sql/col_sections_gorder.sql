-- sift.col_sections lists the order array of a timestamp sorted by an entity column (order_array_group, the second slot of the leaf, next to the plain order) with the
-- order arrays, not with the point directories: the two indexes keep the same point directories, and the one with the group has the arrays of both orders.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
CREATE TABLE gs (id int, ent_id int, ts timestamp, pad text) WITH (autovacuum_enabled = false);
ALTER TABLE gs ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO gs SELECT i, abs(hashint4(i)) % 400, timestamp '2024-01-01' + (abs(hashint4(i + 7)) % 20000000) * interval '1 second', md5(i::text) || md5(i::text || 'x')
FROM generate_series(1, 120000) i;
ANALYZE gs;
CREATE INDEX gs_a ON gs USING sift_col (id, ent_id, ts, pad) WITH (order_array_group = 'off', key_cut = 'off');
CREATE INDEX gs_b ON gs USING sift_col (id, ent_id, ts, pad) WITH (key_cut = 'off');
CREATE FUNCTION gs_bytes(idx regclass, s text) RETURNS bigint LANGUAGE sql AS $$ SELECT coalesce(sum(bytes), 0) FROM sift.col_sections(idx) WHERE section = s $$;

SELECT gs_bytes('gs_a', 'point_dir') = gs_bytes('gs_b', 'point_dir') AS same_point_dirs;
SELECT gs_bytes('gs_b', 'order_arrays') > gs_bytes('gs_a', 'order_arrays') * 3 / 2 AS grouped_order_listed;

DROP FUNCTION gs_bytes(regclass, text);
DROP TABLE gs;
DROP EXTENSION pg_sift;
