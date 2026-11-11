-- The codes of a shared value store (docs/COL-VSTORE.md) that no segment of any index refers to any more are garbage: sift.col_value_store_usage reads them
-- from the segments of the members, and sift.col_value_store_status reports the codes the store holds, those in use and the share of the others.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET sift.test_vstore_columns = 'url';

CREATE TABLE vg (id int PRIMARY KEY, tenant int, url text);
INSERT INTO vg SELECT i, i % 5, 'https://example.com/p/' || md5(i::text) FROM generate_series(1, 4000) i;
ANALYZE vg;
CREATE INDEX vg_a ON vg USING sift_col (id, tenant, url);
CREATE INDEX vg_b ON vg USING sift_col (id, url);

-- every code is in use: both members name all of them, the union is the store
SELECT epoch, member, segments, codes, total_codes FROM sift.col_value_store_usage('vg_url_sift_values') ORDER BY member NULLS LAST;
SELECT indexes_using, total_codes, live_codes, garbage_pct FROM sift.col_value_store_status WHERE table_name = 'vg'::regclass;

-- a thousand values replaced by new ones: the store has them all, the segments name the new ones and the unchanged ones
UPDATE vg SET url = 'https://example.com/q/' || md5(id::text) WHERE id <= 1000;
SELECT sift.col_promote('vg_a') > 0 AS promoted_a, sift.col_promote('vg_b') > 0 AS promoted_b;
VACUUM vg;
SELECT sift.col_merge('vg_a', true) >= 0 AS merged_a, sift.col_merge('vg_b', true) >= 0 AS merged_b;
SELECT member, codes, total_codes FROM sift.col_value_store_usage('vg_url_sift_values') ORDER BY member NULLS LAST;
SELECT indexes_using, total_codes, live_codes, garbage_pct FROM sift.col_value_store_status WHERE table_name = 'vg'::regclass;

-- the answers are the heap's, and an index that goes takes its names with it only: the other still holds what it names
SELECT count(*), count(DISTINCT url) FROM vg WHERE url LIKE 'https://example.com/q/%';
DROP INDEX vg_b;
SELECT member, codes, total_codes FROM sift.col_value_store_usage('vg_url_sift_values') ORDER BY member NULLS LAST;

-- a store of another table, a table that is not a store, a role that cannot read the table
SELECT * FROM sift.col_value_store_usage('vg'::regclass);
CREATE ROLE vg_nobody NOLOGIN;
SET ROLE vg_nobody;
SELECT * FROM sift.col_value_store_usage('vg_url_sift_values');
SELECT store, total_codes, live_codes, garbage_pct FROM sift.col_value_store_status;
RESET ROLE;
DROP ROLE vg_nobody;
DROP TABLE vg;
DROP EXTENSION pg_sift;
