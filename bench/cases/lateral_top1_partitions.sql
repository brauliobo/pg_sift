-- a LATERAL top-1 over a table partitioned by month reads the newest partition first and leaves the others unexecuted, where PostgreSQL read every partition and sorted for each outer row
-- label: `LEFT JOIN LATERAL (SELECT created_at FROM lp_ev WHERE dim_a_id = d.id ORDER BY created_at DESC LIMIT 1)` for 100 entities, over rows events in 12 partitions of a month, `order_array_group = dim_a_id`; the variant `btree` is the same rows with a b-tree on (dim_a_id, created_at)
-- ordered: yes
--@ setup
CREATE TABLE lp_dim (id int PRIMARY KEY);
INSERT INTO lp_dim SELECT g FROM generate_series(1, 100) g;
CREATE TABLE lp_ev (id bigint NOT NULL, dim_a_id int NOT NULL, created_at timestamp NOT NULL, kind text NOT NULL) PARTITION BY RANGE (created_at);
SELECT format('CREATE TABLE lp_ev_%s PARTITION OF lp_ev FOR VALUES FROM (%L) TO (%L)', to_char(m, 'YYMM'), m, m + interval '1 month') FROM generate_series(timestamp '2025-01-01', timestamp '2025-12-01', interval '1 month') m \gexec
INSERT INTO lp_ev SELECT i, 1 + (hashint8extended(i, 1) & 2147483647) % 100, timestamp '2025-01-01' + (hashint8extended(i, 2) & 2147483647) % 31536000 * interval '1 second', (ARRAY['web','app','phone','mail'])[1 + (hashint8extended(i, 5) & 2147483647) % 4] FROM generate_series(1, {rows}) i;
CREATE INDEX lp_col ON lp_ev USING sift_col (id, dim_a_id, created_at, kind) WITH (order_columns = created_at, order_array_columns = created_at, order_array_group = dim_a_id);
VACUUM (FREEZE, ANALYZE) lp_dim, lp_ev;
DROP SCHEMA IF EXISTS c_lateral_top1_partitions__btree CASCADE;
CREATE SCHEMA c_lateral_top1_partitions__btree;
CREATE TABLE c_lateral_top1_partitions__btree.lp_dim AS TABLE lp_dim;
CREATE TABLE c_lateral_top1_partitions__btree.lp_ev (LIKE lp_ev) PARTITION BY RANGE (created_at);
SELECT format('CREATE TABLE c_lateral_top1_partitions__btree.lp_ev_%s PARTITION OF c_lateral_top1_partitions__btree.lp_ev FOR VALUES FROM (%L) TO (%L)', to_char(m, 'YYMM'), m, m + interval '1 month') FROM generate_series(timestamp '2025-01-01', timestamp '2025-12-01', interval '1 month') m \gexec
INSERT INTO c_lateral_top1_partitions__btree.lp_ev SELECT * FROM lp_ev;
CREATE INDEX lp_bt ON c_lateral_top1_partitions__btree.lp_ev (dim_a_id, created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_lateral_top1_partitions__btree.lp_dim, c_lateral_top1_partitions__btree.lp_ev
--@ pg
SELECT d.id, x.created_at FROM lp_dim d LEFT JOIN LATERAL (SELECT created_at FROM lp_ev e WHERE e.dim_a_id = d.id ORDER BY created_at DESC LIMIT 1) x ON true
