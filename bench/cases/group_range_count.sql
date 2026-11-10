-- count(*) of one entity over a week (WHERE dim_a_id = x AND created_at BETWEEN a AND b) reads the entries of the group between the two keys of the second order, where it read the units that hold the entity
-- label: `SELECT count(*) FROM gr_ev WHERE dim_a_id = 17 AND created_at BETWEEN a AND b` over rows/100 entities and rows events in the order of their time, `order_array_group = dim_a_id`; the variant `btree` is the same rows with a b-tree on (dim_a_id, created_at)
-- ordered: no
--@ setup
CREATE TABLE gr_ev (id bigint PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamp NOT NULL, kind text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO gr_ev SELECT i, 1 + (hashint8extended(i, 1) & 2147483647) % ({rows} / 100), timestamp '2023-01-01' + (i::float8 / {rows}) * interval '730 days',
  (ARRAY['web','app','phone','mail','partner','kiosk','chat','other'])[1 + (hashint8extended(i, 5) & 2147483647) % 8]
FROM generate_series(1, {rows}) i;
CREATE INDEX gr_col ON gr_ev USING sift_col (id, dim_a_id, created_at, kind) WITH (key_cut = off, order_array_columns = created_at, order_array_group = dim_a_id);
VACUUM (FREEZE, ANALYZE) gr_ev;
DROP SCHEMA IF EXISTS c_group_range_count__btree CASCADE;
CREATE SCHEMA c_group_range_count__btree;
CREATE TABLE c_group_range_count__btree.gr_ev AS TABLE gr_ev;
CREATE INDEX gr_bt ON c_group_range_count__btree.gr_ev (dim_a_id, created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_group_range_count__btree.gr_ev
--@ pg
SELECT count(*) FROM gr_ev WHERE dim_a_id = 17 AND created_at BETWEEN timestamp '2024-01-01' AND timestamp '2024-04-01'
