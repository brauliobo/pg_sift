-- a sum of a numeric column times a value of a table joined by its unique key: the amounts are added up for each key and the product is made once for each group of the sub-select
-- label: `SELECT t.region, sum(f.amount * k.rate) FROM fl_fact f JOIN fl_tenants t .. JOIN fl_kinds k ON k.id = f.kind_id GROUP BY t.region` over 600,000 rows
-- ordered: yes
--@ setup
CREATE TABLE fl_tenants AS SELECT g AS id, 1 + g % 6 AS region FROM generate_series(1, 2000) g;
ALTER TABLE fl_tenants ADD PRIMARY KEY (id);
CREATE TABLE fl_kinds AS SELECT g AS id, (g / 8.0)::numeric(8,3) AS rate FROM generate_series(1, 30) g;
ALTER TABLE fl_kinds ADD PRIMARY KEY (id);
CREATE TABLE fl_fact AS SELECT g AS id, 1 + (g::bigint * 7919 % 2000)::int AS tenant_id, 1 + g % 30 AS kind_id, ((g::bigint * 7919 % 100000) / 100.0)::numeric(10,2) AS amount FROM generate_series(1, {rows}) g;
CREATE INDEX fl_fact_col ON fl_fact USING sift_col (id, tenant_id, kind_id, amount);
ANALYZE fl_tenants;
ANALYZE fl_kinds;
ANALYZE fl_fact
--@ pg
SELECT t.region, sum(f.amount * k.rate) AS s FROM fl_fact f JOIN fl_tenants t ON t.id = f.tenant_id JOIN fl_kinds k ON k.id = f.kind_id GROUP BY t.region ORDER BY t.region
