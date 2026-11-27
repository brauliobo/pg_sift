-- REINDEX INDEX CONCURRENTLY of a sift_col index with a secondary projection and a tier policy: the index built again under a new OID takes the rows of sift.projection_defs and
-- sift.tier_policies of the one it replaces, builds its projection, and its tiering marks the objects of the index that went as orphans.
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SELECT current_setting('data_directory') || '/sift_tier_ric' AS tierdir \gset
\set mk 'cat >/dev/null && rm -rf ' :tierdir ' && mkdir -p ' :tierdir
COPY (SELECT 1) TO PROGRAM :'mk';
CREATE TABLE evr (id int8, ts timestamp NOT NULL, tenant int, v int, pad text);
INSERT INTO evr SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 7, i % 100, md5(i::text) FROM generate_series(1, 5000) i;
CREATE INDEX evr_col ON evr USING sift_col (id, ts, tenant, v, pad);
VACUUM evr;
INSERT INTO evr SELECT 5000 + i, localtimestamp - i * interval '1 minute', i % 7, i % 100, md5((5000 + i)::text) FROM generate_series(1, 5000) i;
VACUUM evr;
SELECT sift.set_tier_policy('evr_col', 'file://' || :'tierdir', '30 days');
SELECT sift.create_projection('evr_col', 'by_tenant', 'GROUP BY tenant MEASURES count(*), sum(v)');
VACUUM evr;
SELECT count(*) > 0 AS parts_in_the_store FROM sift.tier_status WHERE relation = 'evr_col'::regclass;
SELECT 'evr_col'::regclass::oid AS old_oid, lpad(to_hex(scope_id), 16, '0') AS scope FROM sift.tier_policies \gset

REINDEX INDEX CONCURRENTLY evr_col;
SELECT 'evr_col'::regclass::oid <> :old_oid AS rebuilt_under_a_new_oid;
SELECT count(*) = 1 AND bool_and(rel::oid = 'evr_col'::regclass::oid) AS projection_follows FROM sift.projection_defs;
SELECT count(*) = 1 AND bool_and(relid = 'evr_col'::regclass) AS policy_follows FROM sift.tier_policies;
SELECT (SELECT count(*) FROM sift.projection_defs d WHERE NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = d.rel::oid))
	 + (SELECT count(*) FROM sift.tier_policies t WHERE NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = t.relid::oid)) = 0 AS no_orphan_rows;

-- the VACUUM builds the projection, and the tiering of the new index marks the objects of the old one for the sweep
VACUUM evr;
SELECT name, state FROM sift.projection_list() WHERE relation = 'evr_col'::regclass;
SELECT count(*) > 0 AS old_objects_marked FROM pg_ls_dir(:'tierdir' || '/r' || :'scope') f WHERE f LIKE '%.orphan-%';
SET enable_seqscan = off;
SELECT (SELECT md5(string_agg(tenant || ':' || n || ':' || s, ',' ORDER BY tenant)) FROM (SELECT tenant, count(*) n, sum(v) s FROM evr GROUP BY tenant) q) AS through_index \gset
RESET enable_seqscan;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT (SELECT md5(string_agg(tenant || ':' || n || ':' || s, ',' ORDER BY tenant)) FROM (SELECT tenant, count(*) n, sum(v) s FROM evr GROUP BY tenant) q) = :'through_index' AS same_as_heap;
RESET enable_indexscan;
RESET enable_bitmapscan;
SELECT count(*) AS problems FROM sift.check('evr_col');

-- REINDEX TABLE CONCURRENTLY rebuilds the index too, and the rows follow it again
SELECT 'evr_col'::regclass::oid AS old_oid \gset
REINDEX TABLE CONCURRENTLY evr;
SELECT 'evr_col'::regclass::oid <> :old_oid AS rebuilt_again;
SELECT (SELECT count(*) FROM sift.projection_defs WHERE rel::oid = 'evr_col'::regclass::oid) = 1 AND (SELECT count(*) FROM sift.tier_policies WHERE relid = 'evr_col'::regclass) = 1
	AS rows_follow_the_table_reindex;

DROP TABLE evr;
SELECT count(*) AS rows_left FROM sift.tier_policies;
\set rm 'cat >/dev/null && rm -rf ' :tierdir
COPY (SELECT 1) TO PROGRAM :'rm';
DROP EXTENSION pg_sift;
