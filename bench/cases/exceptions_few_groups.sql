-- a soft-delete filter over units that hold few groups, from the exceptions of the units against a scan of every unit with a stamped row
-- label: `SELECT status, count(*), sum(qty) FROM soft WHERE stamp IS NULL GROUP BY 1` on 400,000 rows cut by `tenant` into 31 units (1 row in 101 stamped), `sift_col` after VACUUM; the variant `scan` reads the units (`sift.agg_preagg = off`), as the units were read when the exceptions of a small cube were dropped
--@ setup
CREATE TABLE soft AS SELECT i AS id, i / 6500 AS tenant, i % 3 AS status, i * 10 AS qty, (i / 2.0)::float8 AS price, CASE WHEN i % 101 = 0 THEN i END::bigint AS stamp, jsonb_build_object('items', jsonb_build_array(jsonb_build_object('category', 'c' || i % 5, 'qty', i % 7))) AS doc FROM generate_series(1, 400000) i;
SET sift.test_col_unit_rows = 13000;
CREATE INDEX soft_col ON soft USING sift_col (id, tenant, status, qty, price, stamp, doc) WITH (key_cut = 'tenant', paths = 'doc.items[]{category text, qty int4}', preagg = 'cube(tenant: qty, price; status: qty) rollup(doc.items[].category: doc.items[].qty)', superunit = 0);
VACUUM (FREEZE, ANALYZE) soft;
RESET sift.test_col_unit_rows;
--@ pg
SELECT status, count(*), sum(qty) FROM soft WHERE stamp IS NULL GROUP BY 1
