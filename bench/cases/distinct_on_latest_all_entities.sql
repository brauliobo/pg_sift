-- DISTINCT ON (entity) over every entity: the latest row of an entity is chosen among the rows of a chunk before the values of the row are copied, so a chunk that holds several rows of an entity copies one
-- label: `SELECT DISTINCT ON (dim_a_id) dim_a_id, id, created_at .. ORDER BY dim_a_id, created_at DESC`, over rows/100 entities and rows events in the order of their time; the variant `btree` is the same rows with a b-tree on (dim_a_id, created_at)
-- ordered: yes
--@ setup
CREATE TABLE lt_dim (id int PRIMARY KEY, dim_b_id int NOT NULL);
INSERT INTO lt_dim SELECT g, g % ({rows} / 10000) FROM generate_series(1, {rows} / 100) g;
CREATE TABLE lt_ev (id bigint PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int NOT NULL, created_at timestamp NOT NULL, kind text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO lt_ev SELECT i, t, t + 40 * ((hashint8extended(i, 2) & 2147483647) % ({rows} / 4000)), timestamp '2023-01-01' + (i::float8 / {rows}) * interval '730 days',
  (ARRAY['web','app','phone','mail','partner','kiosk','chat','other'])[1 + (hashint8extended(i, 5) & 2147483647) % 8]
FROM (SELECT i, 1 + (hashint8extended(i, 1) & 2147483647) % 40 AS t FROM generate_series(1, {rows}) i) x;
CREATE INDEX lt_col ON lt_ev USING sift_col (id, tenant_id, dim_a_id, created_at, kind) WITH (key_cut = tenant_id, order_array_columns = created_at, order_array_group = dim_a_id);
VACUUM (FREEZE, ANALYZE) lt_dim, lt_ev;
DROP SCHEMA IF EXISTS c_distinct_on_latest_all_entities__btree CASCADE;
CREATE SCHEMA c_distinct_on_latest_all_entities__btree;
CREATE TABLE c_distinct_on_latest_all_entities__btree.lt_dim AS TABLE lt_dim;
CREATE TABLE c_distinct_on_latest_all_entities__btree.lt_ev AS TABLE lt_ev;
CREATE INDEX lt_bt ON c_distinct_on_latest_all_entities__btree.lt_ev (dim_a_id, created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_distinct_on_latest_all_entities__btree.lt_dim, c_distinct_on_latest_all_entities__btree.lt_ev
--@ pg
SELECT DISTINCT ON (e.dim_a_id) e.dim_a_id, e.id, e.created_at FROM lt_ev e ORDER BY e.dim_a_id, e.created_at DESC
