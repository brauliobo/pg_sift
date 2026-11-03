-- the inner side of a nested loop on an equality: 1,000 keys of an outer table each find one row of the events by the point directory of a sift_col index, and the index scan starts over for each key
-- label: `JOIN lt_ev e ON e.ref = p.ref` of 1,000 probe keys against rows events of unique `ref`, `point_tid_columns = 'ref'`, an Index Scan for each key (no hash join, no merge join, no index-only scan); the variant `btree` is the same rows with a b-tree on `ref`
-- ordered: yes
-- set: enable_hashjoin = off; enable_mergejoin = off; enable_indexonlyscan = off
--@ setup
CREATE TABLE lt_ev (id bigint PRIMARY KEY, ref int NOT NULL, tenant_id int NOT NULL, status int2 NOT NULL, pad text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO lt_ev SELECT i, i, 1 + (hashint8extended(i, 2) & 2147483647) % 40, (i % 5)::int2, repeat('x', 40) FROM generate_series(1, {rows}) i;
CREATE TABLE lt_probe AS SELECT row_number() OVER (ORDER BY hashint8extended(id, 3))::int AS n, ref FROM (SELECT id, ref FROM lt_ev ORDER BY hashint8extended(id, 4) LIMIT 1000) s;
CREATE INDEX lt_col ON lt_ev USING sift_col (ref, tenant_id, status, id) WITH (point_tid_columns = 'ref');
VACUUM (FREEZE, ANALYZE) lt_ev, lt_probe;
DROP SCHEMA IF EXISTS c_join_eq_nl_probe__btree CASCADE;
CREATE SCHEMA c_join_eq_nl_probe__btree;
CREATE TABLE c_join_eq_nl_probe__btree.lt_ev AS TABLE lt_ev;
CREATE TABLE c_join_eq_nl_probe__btree.lt_probe AS TABLE lt_probe;
CREATE INDEX lt_bt ON c_join_eq_nl_probe__btree.lt_ev (ref);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_join_eq_nl_probe__btree.lt_ev, c_join_eq_nl_probe__btree.lt_probe
--@ pg
SELECT p.n, e.tenant_id, e.status FROM lt_probe p JOIN lt_ev e ON e.ref = p.ref ORDER BY p.n
