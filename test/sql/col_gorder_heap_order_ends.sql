-- A timestamp in heap order has the second order by the entity and, beside it, the ends of its plain order. The units are cut by another column (each holds the whole range of the
-- timestamp), so ORDER BY ts LIMIT n over the table read the units of the newest rows; the ends answer it with n entries of each segment. A range in the middle and an OFFSET past the
-- ends stay with the units, and a promotion and a merge keep the answers.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
\set VERBOSITY terse

CREATE FUNCTION eo_chk(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.topn_check_ordered_rows(q) $$;

CREATE TABLE eo (id int, tenant_id int, ent_id int, created timestamp, updated timestamp, pad text);
INSERT INTO eo SELECT i, i % 40, abs(hashint4(i)) % 3000, timestamp '2024-01-01' + i * interval '30 seconds', timestamp '2024-02-01' + i * interval '45 seconds',
	   md5(i::text) || md5(i::text || 'x') || md5(i::text || 'y')
FROM generate_series(1, 120000) i;
ANALYZE eo;
CREATE INDEX eo_i ON eo USING sift_col (id, tenant_id, ent_id, created, updated, pad) WITH (key_cut = 'tenant_id');
SELECT regexp_replace(reloption, '^(order_array_[a-z]+)=.*$', '\1') AS option, regexp_replace(reloption, '^[a-z_]+=', '') AS value
FROM pg_class, unnest(reloptions) reloption WHERE relname = 'eo_i' AND reloption LIKE 'order_array_%' ORDER BY 1;
SELECT sift.col_verify('eo_i');

-- what the NOTICE of the build and index_choices say each column has
SELECT value FROM sift.index_choices('eo_i') WHERE choice = 'order_array_columns';

-- the page of the whole table, in both directions and as a later page, reads the ends
SELECT col, d, result, count(*) FROM (
	SELECT col, d, eo_chk(format('SELECT id, %s FROM eo ORDER BY %s %s, id LIMIT %s OFFSET %s', col, col, d, l, o)) AS result
	FROM unnest(ARRAY['created', 'updated']) col, unnest(ARRAY['ASC', 'DESC']) d, unnest(ARRAY[1, 20, 500]) l, unnest(ARRAY[0, 30, 3000]) o) r
GROUP BY 1, 2, 3 ORDER BY 1, 2, 3;

-- what the ends do not hold stays with the units: an offset past them, and a range in the middle of the column
SELECT eo_chk('SELECT id, created FROM eo ORDER BY created DESC, id LIMIT 20 OFFSET 6000') AS past_the_ends,
	   eo_chk('SELECT id, created FROM eo WHERE created >= timestamp ''2024-02-10'' AND created < timestamp ''2024-02-11'' ORDER BY created, id LIMIT 20') AS middle_range;
SELECT eo_chk('SELECT id, created FROM eo WHERE tenant_id = 7 ORDER BY created DESC, id LIMIT 20') AS tenant_page,
	   eo_chk('SELECT id, created FROM eo WHERE ent_id = 7 ORDER BY created DESC, id LIMIT 20') AS entity_page;

-- rows written after the build, a promotion and a merge keep the answers
INSERT INTO eo SELECT 200000 + i, i % 40, i % 3000, timestamp '2024-06-01' + i * interval '1 second', timestamp '2024-07-01' + i * interval '1 second', 'w' FROM generate_series(1, 3000) i;
UPDATE eo SET created = timestamp '2023-12-31' WHERE id IN (5, 6, 7);
DELETE FROM eo WHERE id % 211 = 0;
SELECT eo_chk('SELECT id, created FROM eo ORDER BY created DESC, id LIMIT 20') AS buffers_desc, eo_chk('SELECT id, created FROM eo ORDER BY created, id LIMIT 20') AS buffers_asc;
SELECT sift.col_promote('eo_i'::regclass) >= 0 AS promoted;
SELECT eo_chk('SELECT id, created FROM eo ORDER BY created DESC, id LIMIT 20') AS promoted_desc, eo_chk('SELECT id, updated FROM eo ORDER BY updated, id LIMIT 20') AS promoted_asc;
SELECT sift.col_merge('eo_i'::regclass, true) >= 0 AS merged;
SELECT eo_chk('SELECT id, created FROM eo ORDER BY created DESC, id LIMIT 20') AS merged_desc, eo_chk('SELECT id, updated FROM eo ORDER BY updated, id LIMIT 20') AS merged_asc;
SELECT sift.col_verify('eo_i');

DROP TABLE eo;
DROP FUNCTION eo_chk(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
