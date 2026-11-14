-- avg, min, max and count of a CASE with an ELSE that is a value: the aggregates of the value with a filter on the condition and the rows that do not hold it
-- label: `avg`, `min` and `count` of `CASE WHEN status = 'paid' THEN qty ELSE 0 END` by tenant over {rows} rows
-- ordered: no
-- set: sift.agg_force = on
--@ setup
DROP TABLE IF EXISTS ce_fact;
CREATE TABLE ce_fact (id int, tenant int, status text, qty int) WITH (autovacuum_enabled = false);
INSERT INTO ce_fact SELECT g, g % 50, (array['paid', 'open', 'void', 'late'])[1 + g % 4], (g * 7919) % 200 - 20 FROM generate_series(1, {rows}) g;
CREATE INDEX ce_fact_col ON ce_fact USING sift_col (id, tenant, status, qty);
VACUUM (ANALYZE) ce_fact
--@ pg
SELECT tenant, avg(CASE WHEN status = 'paid' THEN qty ELSE 0 END), min(CASE WHEN status = 'paid' THEN qty ELSE 0 END), count(CASE WHEN status = 'paid' THEN qty ELSE 0 END) FROM ce_fact GROUP BY tenant
