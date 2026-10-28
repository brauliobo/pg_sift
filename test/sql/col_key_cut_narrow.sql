-- The sample of the default key_cut is a number of units (superblocks of 256 pages), not a number of rows: a table of narrow rows, with more than 12,500 rows a unit, is
-- sampled like one of wide rows (32 units at least) and decided by the default formula, instead of ending at "the units are too large for a sample" whatever its size.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;

CREATE TABLE kn (d date, a int, pad text) WITH (autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO kn SELECT '2020-01-01'::date + (random() * 1826)::int, (random() * 1000)::int, repeat('x', 112) FROM generate_series(1, 900000) i;
VACUUM (FREEZE, ANALYZE) kn;
SELECT pg_relation_size('kn') / 8192 / 256 >= 64 AS units_at_least_64,
       (SELECT reltuples FROM pg_class WHERE relname = 'kn') / (pg_relation_size('kn') / 8192 / 256) > 12500 AS rows_a_unit_over_12500;
CREATE INDEX kn_i ON kn USING sift_col (d, a);
SELECT leaf, sampled, decision, reason IS DISTINCT FROM 'the units are too large for a sample' AS was_sampled FROM sift.key_cut_decision('kn_i');
DROP TABLE kn;

-- a table of fewer than 32 units is sampled whole and the formula decides, with no minimum of units: 500,000 ranges of 56-byte rows (23 units) are cut by the start of the range,
-- as 1,000,000 and 3,000,000 are (@> 4.4 M instructions without the cut, 1.0 M with it, a GiST 0.4 M)
CREATE TABLE kr (id int, r int4range, pad text) WITH (autovacuum_enabled = false);
INSERT INTO kr SELECT i, int4range(x, x + 1 + (hashint8extended(i, 7) & 1023)::int), repeat('x', 40)
FROM (SELECT i, (hashint8extended(i, 3) & 33554431)::int AS x FROM generate_series(1, 500000) i) s;
VACUUM (FREEZE, ANALYZE) kr;
SELECT pg_relation_size('kr') / 8192 / 256 < 32 AS under_32_units;
CREATE INDEX kr_i ON kr USING sift_col (r);
SELECT leaf, decision, benefit_over_cost FROM (SELECT leaf, decision, reason LIKE '%benefit of the cut is at least its cost' AS benefit_over_cost FROM sift.key_cut_decision('kr_i')) d;
SELECT reloptions FROM pg_class WHERE relname = 'kr_i';
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM kr WHERE r @> 12345678;
SET enable_seqscan = on; SET enable_bitmapscan = on; SET enable_indexscan = off;
DROP INDEX kr_i;
SELECT count(*) FROM kr WHERE r @> 12345678;
DROP TABLE kr;

-- an empty table has nothing to sample
CREATE TABLE ke (id int, d date);
CREATE INDEX ke_i ON ke USING sift_col (id, d);
SELECT decision, reason FROM sift.key_cut_decision('ke_i');
DROP TABLE ke;
DROP EXTENSION pg_sift;
