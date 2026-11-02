-- the latest row of each of 200 entities by the grouped order array: a LATERAL top-1 per entity
-- label: `LATERAL (.. WHERE entity = e ORDER BY created_at DESC LIMIT 1)` for 200 entities over {rows} rows, the default index (5 columns, an entity of 3,000); the variant `btree` is a b-tree on (entity, created_at), the variant `ungrouped` the same index with `order_array_group = 'off'` (the plain order array of the entity, no second order)
-- ordered: yes
--@ setup
CREATE TABLE gl AS SELECT id, 1 + ((hashint8extended(id, 5) & 2147483647) % 3000)::int AS entity, timestamptz '2022-01-01' + id * interval '2 minutes' AS created_at, 1 + (id % 5)::int AS score, (ARRAY['en', 'pt', 'es', 'fr', 'de', 'it', 'ja', 'ko'])[1 + (id % 8)::int] AS lang FROM orders;
CREATE INDEX gl_col ON gl USING sift_col (id, entity, created_at, score, lang);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) gl;
DROP SCHEMA IF EXISTS c_group_latest_entity__btree CASCADE;
CREATE SCHEMA c_group_latest_entity__btree;
CREATE TABLE c_group_latest_entity__btree.gl AS TABLE gl;
CREATE INDEX gl_bt ON c_group_latest_entity__btree.gl (entity, created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_group_latest_entity__btree.gl;
DROP SCHEMA IF EXISTS c_group_latest_entity__ungrouped CASCADE;
CREATE SCHEMA c_group_latest_entity__ungrouped;
CREATE TABLE c_group_latest_entity__ungrouped.gl AS TABLE gl;
CREATE INDEX gl_col ON c_group_latest_entity__ungrouped.gl USING sift_col (id, entity, created_at, score, lang) WITH (order_array_group = 'off');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_group_latest_entity__ungrouped.gl
--@ pg
SELECT e, r.created_at FROM generate_series(1, 200) e CROSS JOIN LATERAL (SELECT created_at FROM gl WHERE entity = e ORDER BY created_at DESC LIMIT 1) r
