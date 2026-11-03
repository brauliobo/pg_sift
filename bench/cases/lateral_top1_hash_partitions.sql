-- a LATERAL top-1 over a table partitioned by hash of the key runs the one partition that holds the key, where PostgreSQL scanned it and sorted for each outer row; the milliseconds are of whole statements, planning included
-- label: `LEFT JOIN LATERAL (SELECT created_at FROM lh_ev WHERE dim_a_id = d.id ORDER BY created_at DESC LIMIT 1)` for 100 entities, over rows events in 16 hash partitions, `order_array_group = dim_a_id`; the variant `btree` is the same rows with a b-tree on (dim_a_id, created_at)
-- ordered: yes
--@ setup
CREATE TABLE lh_dim (id int PRIMARY KEY);
INSERT INTO lh_dim SELECT g FROM generate_series(1, 100) g;
CREATE TABLE lh_ev (id bigint NOT NULL, dim_a_id int NOT NULL, created_at timestamp NOT NULL, kind text NOT NULL) PARTITION BY HASH (dim_a_id);
SELECT format('CREATE TABLE lh_ev_%s PARTITION OF lh_ev FOR VALUES WITH (MODULUS 16, REMAINDER %s)', r, r) FROM generate_series(0, 15) r \gexec
INSERT INTO lh_ev SELECT i, 1 + (hashint8extended(i, 1) & 2147483647) % 100, timestamp '2025-01-01' + (hashint8extended(i, 2) & 2147483647) % 31536000 * interval '1 second', (ARRAY['web','app','phone','mail'])[1 + (hashint8extended(i, 5) & 2147483647) % 4] FROM generate_series(1, {rows}) i;
CREATE INDEX lh_col ON lh_ev USING sift_col (id, dim_a_id, created_at, kind) WITH (order_columns = created_at, order_array_columns = created_at, order_array_group = dim_a_id);
VACUUM (FREEZE, ANALYZE) lh_dim, lh_ev;
DROP SCHEMA IF EXISTS c_lateral_top1_hash_partitions__btree CASCADE;
CREATE SCHEMA c_lateral_top1_hash_partitions__btree;
CREATE TABLE c_lateral_top1_hash_partitions__btree.lh_dim AS TABLE lh_dim;
CREATE TABLE c_lateral_top1_hash_partitions__btree.lh_ev (LIKE lh_ev) PARTITION BY HASH (dim_a_id);
SELECT format('CREATE TABLE c_lateral_top1_hash_partitions__btree.lh_ev_%s PARTITION OF c_lateral_top1_hash_partitions__btree.lh_ev FOR VALUES WITH (MODULUS 16, REMAINDER %s)', r, r) FROM generate_series(0, 15) r \gexec
INSERT INTO c_lateral_top1_hash_partitions__btree.lh_ev SELECT * FROM lh_ev;
CREATE INDEX lh_bt ON c_lateral_top1_hash_partitions__btree.lh_ev (dim_a_id, created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_lateral_top1_hash_partitions__btree.lh_dim, c_lateral_top1_hash_partitions__btree.lh_ev
--@ pg
SELECT d.id, x.created_at FROM lh_dim d LEFT JOIN LATERAL (SELECT created_at FROM lh_ev e WHERE e.dim_a_id = d.id ORDER BY created_at DESC LIMIT 1) x ON true
