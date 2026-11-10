-- a LATERAL top-1 for each of 200 entities finds the rows of its entity in a write buffer of 5,000 rows from the buffer put in groups once, not by reading all of them for each entity
-- label: `LEFT JOIN LATERAL (SELECT created_at FROM lw_ev WHERE dim_a_id = d.id ORDER BY created_at DESC LIMIT 1)` for 200 entities, over rows/100 entities and rows events in the order of their time, `order_array_group = dim_a_id`, with 5,000 more events still in the write buffer; the variant `btree` is the same rows with a b-tree on (dim_a_id, created_at)
-- ordered: yes
--@ setup
CREATE TABLE lw_dim (id int PRIMARY KEY, dim_b_id int NOT NULL);
INSERT INTO lw_dim SELECT g, g % ({rows} / 10000) FROM generate_series(1, {rows} / 100) g;
CREATE TABLE lw_ev (id bigint PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int NOT NULL, created_at timestamp NOT NULL, kind text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO lw_ev SELECT i, t, t + 40 * ((hashint8extended(i, 2) & 2147483647) % ({rows} / 4000)), timestamp '2023-01-01' + (i::float8 / {rows}) * interval '730 days',
  (ARRAY['web','app','phone','mail','partner','kiosk','chat','other'])[1 + (hashint8extended(i, 5) & 2147483647) % 8]
FROM (SELECT i, 1 + (hashint8extended(i, 1) & 2147483647) % 40 AS t FROM generate_series(1, {rows}) i) x;
CREATE INDEX lw_col ON lw_ev USING sift_col (id, tenant_id, dim_a_id, created_at, kind) WITH (key_cut = tenant_id, order_array_columns = created_at, order_array_group = dim_a_id);
VACUUM (FREEZE, ANALYZE) lw_dim, lw_ev;
INSERT INTO lw_ev SELECT i, t, t + 40 * ((hashint8extended(i, 2) & 2147483647) % ({rows} / 4000)), timestamp '2023-01-01' + (i::float8 / ({rows} + 5000)) * interval '730 days',
  (ARRAY['web','app','phone','mail','partner','kiosk','chat','other'])[1 + (hashint8extended(i, 5) & 2147483647) % 8]
FROM (SELECT i, 1 + (hashint8extended(i, 1) & 2147483647) % 40 AS t FROM generate_series({rows} + 1, {rows} + 5000) i) x;
ANALYZE lw_ev;
DROP SCHEMA IF EXISTS c_lateral_top1_wbuf_groups__btree CASCADE;
CREATE SCHEMA c_lateral_top1_wbuf_groups__btree;
CREATE TABLE c_lateral_top1_wbuf_groups__btree.lw_dim AS TABLE lw_dim;
CREATE TABLE c_lateral_top1_wbuf_groups__btree.lw_ev AS TABLE lw_ev;
CREATE INDEX lw_bt ON c_lateral_top1_wbuf_groups__btree.lw_ev (dim_a_id, created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_lateral_top1_wbuf_groups__btree.lw_dim, c_lateral_top1_wbuf_groups__btree.lw_ev
--@ pg
SELECT d.id, x.created_at FROM lw_dim d LEFT JOIN LATERAL (SELECT created_at FROM lw_ev e WHERE e.dim_a_id = d.id ORDER BY created_at DESC LIMIT 1) x ON true WHERE d.id <= 200
