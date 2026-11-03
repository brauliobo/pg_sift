-- count(*) of one entity from the entries of the second order of the grouped order array: 5 columns, an entity of 3,000 values, the default index
-- label: `count(*) WHERE entity = 17` over {rows} rows, the default index (grouped order array on the entity); the variant `btree` is a b-tree on (entity, created_at), the variant `ungrouped` the same index with `order_array_group = 'off'` (the plain order array of the entity, no second order)
-- ordered: yes
--@ setup
CREATE TABLE ge AS SELECT id, 1 + ((hashint8extended(id, 5) & 2147483647) % 3000)::int AS entity, timestamptz '2022-01-01' + id * interval '2 minutes' AS created_at, 1 + (id % 5)::int AS score, (ARRAY['en', 'pt', 'es', 'fr', 'de', 'it', 'ja', 'ko'])[1 + (id % 8)::int] AS lang FROM orders;
CREATE INDEX ge_col ON ge USING sift_col (id, entity, created_at, score, lang);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ge;
DROP SCHEMA IF EXISTS c_group_count_entity__btree CASCADE;
CREATE SCHEMA c_group_count_entity__btree;
CREATE TABLE c_group_count_entity__btree.ge AS TABLE ge;
CREATE INDEX ge_bt ON c_group_count_entity__btree.ge (entity, created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_group_count_entity__btree.ge;
DROP SCHEMA IF EXISTS c_group_count_entity__ungrouped CASCADE;
CREATE SCHEMA c_group_count_entity__ungrouped;
CREATE TABLE c_group_count_entity__ungrouped.ge AS TABLE ge;
CREATE INDEX ge_col ON c_group_count_entity__ungrouped.ge USING sift_col (id, entity, created_at, score, lang) WITH (order_array_group = 'off');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_group_count_entity__ungrouped.ge
--@ pg
SELECT count(*) FROM ge WHERE entity = 17
