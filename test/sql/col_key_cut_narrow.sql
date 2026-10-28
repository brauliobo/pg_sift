-- The sample of the default key_cut is a number of units (superblocks of 256 pages), not a number of rows: a table of narrow rows, with more than 12,500 rows a unit, is
-- sampled like one of wide rows (32 units at least) and decided by its predicted size, instead of ending at "the units are too large for a sample" whatever its size.
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
DROP EXTENSION pg_sift;
