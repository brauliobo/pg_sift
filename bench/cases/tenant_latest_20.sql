-- the latest 20 rows of a tenant: units cut by the tenant (key_cut), the order array of the date and the group order of the foreign key, the default index
-- label: `WHERE tenant_id = 5 ORDER BY event_date DESC LIMIT 20` over {rows} rows of 40 tenants (skewed) and 730 dates; the variant `btree` is a b-tree on (tenant_id, event_date)
-- ordered: yes
--@ setup
CREATE TABLE tl AS SELECT id, 1 + (((hashint8extended(id, 1) & 2147483647)::float8 / 2147483647) ^ 2 * 40)::int AS tenant_id, ((hashint8extended(id, 2) & 2147483647) % 3000)::int AS dim_a_id, date '2024-01-01' + ((hashint8extended(id, 3) & 2147483647) % 730)::int AS event_date, ((hashint8extended(id, 4) & 2147483647) % 5)::smallint AS status, (ARRAY['click', 'view', 'buy', 'share', 'save', 'rate', 'flag', 'skip'])[1 + ((hashint8extended(id, 5) & 2147483647) % 8)::int] AS kind, ((hashint8extended(id, 6) & 2147483647) % 100)::int AS counter_a, (((hashint8extended(id, 7) & 2147483647) % 10000) / 100.0)::numeric(10,2) AS value_num FROM orders;
CREATE INDEX tl_col ON tl USING sift_col (tenant_id, dim_a_id, event_date, status, kind, counter_a, value_num);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) tl;
DROP SCHEMA IF EXISTS c_tenant_latest_20__btree CASCADE;
CREATE SCHEMA c_tenant_latest_20__btree;
CREATE TABLE c_tenant_latest_20__btree.tl AS TABLE tl;
CREATE INDEX tl_bt ON c_tenant_latest_20__btree.tl (tenant_id, event_date);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_tenant_latest_20__btree.tl
--@ pg
SELECT id, event_date FROM tl WHERE tenant_id = 5 ORDER BY event_date DESC LIMIT 20
