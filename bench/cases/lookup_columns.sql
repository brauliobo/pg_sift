-- a join to a table of 200,000 rows whose lookup table the query loads (no cached copy of an earlier query), from the leaves of a sift_col index of the table, where it was read from the heap
-- label: `count(*), sum(amount)` of 1,000,000 orders joined to a table of 200,000 rows by customer id, `grp < 10`, `GROUP BY` its 100 kinds, the lookup table loaded by the query (`sift.agg_lookup_cache_kb = 0`, `sift.agg_lookup_shared_kb = 0`)
-- ordered: yes
-- set: sift.agg_force = on; sift.agg_lookup_cache_kb = 0; sift.agg_lookup_shared_kb = 0
--@ setup
CREATE TABLE seg AS SELECT g AS id, (g % 50)::int AS grp, 'kind' || (g % 100) AS kind, date '2026-01-01' + g % 300 AS d FROM generate_series(1, 200000) g;
ALTER TABLE seg ADD PRIMARY KEY (id);
CREATE INDEX seg_col ON seg USING sift_col (id, grp, kind, d);
VACUUM (FREEZE, ANALYZE) seg
--@ pg
SELECT s.kind, count(*), sum(o.amount) FROM orders o JOIN seg s ON s.id = o.customer_id AND s.grp < 10 GROUP BY s.kind ORDER BY s.kind
