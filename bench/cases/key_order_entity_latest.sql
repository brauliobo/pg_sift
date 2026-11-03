-- the latest row of each of 100 entities of 1,000 over a table in arrival order (the date grows with the heap): the default layout of an index of an entity and a date, against the same index with key_order = 'off' and a b-tree on (entity, date)
-- label: `LATERAL (SELECT event_date WHERE tenant_id = p.id ORDER BY event_date DESC LIMIT 1)` for 100 entities of 1,000, 300,000 rows; the variants `off` and `btree` are the same rows
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS ke;
CREATE TABLE ke AS SELECT g::int AS id, 1 + least(999, floor(1000 * power((hashint4(g) & 2147483647) / 2147483647.0, 1.5)))::int AS tenant_id, date '2022-01-01' + (g * 730 / {rows} + (hashint4(g + 7) & 2147483647) % 3) AS event_date, (hashint4(g + 13) & 2147483647) % 1000 AS amount, (1 + (hashint4(g + 11) & 2147483647) % 5)::int2 AS status FROM generate_series(1, {rows}) g;
CREATE TABLE ke_probe AS SELECT tenant_id AS id FROM ke GROUP BY 1 ORDER BY md5(tenant_id::text) LIMIT 100;
CREATE INDEX ke_col ON ke USING sift_col (tenant_id, event_date, amount, status);
CREATE SCHEMA c_key_order_entity_latest__off;
CREATE TABLE c_key_order_entity_latest__off.ke AS TABLE ke;
CREATE TABLE c_key_order_entity_latest__off.ke_probe AS TABLE ke_probe;
CREATE INDEX ke_col ON c_key_order_entity_latest__off.ke USING sift_col (tenant_id, event_date, amount, status) WITH (key_order = 'off');
CREATE SCHEMA c_key_order_entity_latest__btree;
CREATE TABLE c_key_order_entity_latest__btree.ke AS TABLE ke;
CREATE TABLE c_key_order_entity_latest__btree.ke_probe AS TABLE ke_probe;
CREATE INDEX ke_bt ON c_key_order_entity_latest__btree.ke (tenant_id, event_date);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ke;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ke_probe;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_key_order_entity_latest__off.ke;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_key_order_entity_latest__off.ke_probe;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_key_order_entity_latest__btree.ke;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_key_order_entity_latest__btree.ke_probe
--@ pg
SELECT p.id, l.event_date FROM ke_probe p, LATERAL (SELECT event_date FROM ke e WHERE e.tenant_id = p.id ORDER BY event_date DESC LIMIT 1) l ORDER BY p.id
