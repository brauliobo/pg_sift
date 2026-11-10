-- ORDER BY a date with 730 values (1,400 equal keys in a row at 1,000,000 rows) with the layout the index chooses: the compact order is kept only for the chunks where it is smaller
-- label: `count(*) WHERE d >= '2024-03-01' AND d < '2024-06-01'` over a date with 730 values
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS olt;
CREATE TABLE olt AS SELECT g::int AS id, date '2024-01-01' + (hashint8extended(g, 9) & 1023)::int % 730 AS d FROM generate_series(1, {rows}) g ORDER BY md5((g * 31 + 7)::text);
CREATE INDEX olt_col ON olt USING sift_col (id, d) WITH (order_array_columns = 'd');
CREATE SCHEMA c_order_layout_ties_count__whole;
CREATE TABLE c_order_layout_ties_count__whole.olt AS TABLE olt;
CREATE INDEX olt_col ON c_order_layout_ties_count__whole.olt USING sift_col (id, d) WITH (order_array_columns = 'd', order_layout = 'whole');
CREATE SCHEMA c_order_layout_ties_count__compact;
CREATE TABLE c_order_layout_ties_count__compact.olt AS TABLE olt;
CREATE INDEX olt_col ON c_order_layout_ties_count__compact.olt USING sift_col (id, d) WITH (order_array_columns = 'd', order_layout = 'compact');
CREATE SCHEMA c_order_layout_ties_count__btree;
CREATE TABLE c_order_layout_ties_count__btree.olt AS TABLE olt;
CREATE INDEX olt_bt ON c_order_layout_ties_count__btree.olt (d);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) olt;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_order_layout_ties_count__whole.olt;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_order_layout_ties_count__compact.olt;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_order_layout_ties_count__btree.olt;
DO $$ DECLARE c float8 := (SELECT correlation FROM pg_stats WHERE schemaname = 'c_events' AND tablename = 'olt' AND attname = 'd'); BEGIN IF c IS NULL OR abs(c) >= 0.05 THEN RAISE EXCEPTION 'the key follows the heap: correlation %', c; END IF; END $$;
--@ pg
SELECT count(*) FROM olt WHERE d >= date '2024-03-01' AND d < date '2024-06-01'
