-- a LATERAL top-1 for each entity with a filter on a text column of the rows (`kind = 'web'`) walks the grouped order array and tests the string on the heap row of each entry, not the unit of the group
-- label: `LEFT JOIN LATERAL (SELECT created_at FROM lt_ev WHERE dim_a_id = d.id AND kind = 'web' ORDER BY created_at DESC LIMIT 1)` for the 100 entities of one group, over rows/100 entities and rows events in the order of their time, `order_array_group = dim_a_id`; the variant `btree` is the same rows with a b-tree on (dim_a_id, created_at)
-- ordered: yes
--@ setup
CREATE TABLE lt_dim (id int PRIMARY KEY, dim_b_id int NOT NULL);
INSERT INTO lt_dim SELECT g, g % ({rows} / 10000) FROM generate_series(1, {rows} / 100) g;
CREATE TABLE lt_ev (id bigint PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int NOT NULL, created_at timestamp NOT NULL, kind text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO lt_ev SELECT i, t, t + 40 * ((hashint8extended(i, 2) & 2147483647) % ({rows} / 4000)), timestamp '2023-01-01' + (i::float8 / {rows}) * interval '730 days',
  (ARRAY['web','app','phone','mail','partner','kiosk','chat','other'])[1 + (hashint8extended(i, 5) & 2147483647) % 8]
FROM (SELECT i, 1 + (hashint8extended(i, 1) & 2147483647) % 40 AS t FROM generate_series(1, {rows}) i) x;
CREATE INDEX lt_col ON lt_ev USING sift_col (id, tenant_id, dim_a_id, created_at, kind) WITH (key_cut = tenant_id, order_columns = created_at, order_array_columns = created_at, order_array_group = dim_a_id);
VACUUM (FREEZE, ANALYZE) lt_dim, lt_ev;
DROP SCHEMA IF EXISTS c_lateral_top1_text_filter__btree CASCADE;
CREATE SCHEMA c_lateral_top1_text_filter__btree;
CREATE TABLE c_lateral_top1_text_filter__btree.lt_dim AS TABLE lt_dim;
CREATE TABLE c_lateral_top1_text_filter__btree.lt_ev AS TABLE lt_ev;
CREATE INDEX lt_bt ON c_lateral_top1_text_filter__btree.lt_ev (dim_a_id, created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_lateral_top1_text_filter__btree.lt_dim, c_lateral_top1_text_filter__btree.lt_ev
--@ pg
SELECT d.id, x.created_at FROM lt_dim d CROSS JOIN LATERAL (SELECT created_at FROM lt_ev e WHERE e.dim_a_id = d.id AND e.kind = 'web' ORDER BY created_at DESC LIMIT 1) x WHERE d.dim_b_id = 5
