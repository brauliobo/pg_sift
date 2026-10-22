-- :n rows of `facts` for the rollup of the rows of a unit (docs/COL-PREAGG.md section 2): scalar columns of known cardinality, assigned independently of each other.
--   kind 8 values (text), sub 12, flag 5 (low); mid_a 400, mid_b 300 (high); mon 24 months in the order of the rows (a time bucket: a unit holds one or two).
-- psql db -v n=200000 -v pad=200 -f gen.sql      (pad: bytes of a PLAIN filler, it fixes the rows per heap page and with it the rows per unit)
CREATE EXTENSION IF NOT EXISTS pg_sift;
DROP TABLE IF EXISTS facts;
CREATE TABLE facts (id int PRIMARY KEY, kind text, sub int, flag int, mid_a int, mid_b int, mon date, amount numeric(10,2), qty int, score float8, pad text)
	WITH (autovacuum_enabled = false);
ALTER TABLE facts ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO facts
SELECT i, 'k' || (hashint4(i + 1) & 7), (hashint4(i + 2) & 2147483647) % 12, (hashint4(i + 3) & 2147483647) % 5,
       (hashint4(i + 4) & 2147483647) % 400, (hashint4(i + 5) & 2147483647) % 300,
       date '2024-01-01' + ((i::bigint * 24 / :n)::int) * 30,
       round((10 + (hashint4(i + 11) & 65535) / 100.0)::numeric, 2), 1 + (hashint4(i + 6) & 2147483647) % 20,
       ((hashint4(i + 17) & 65535) / 655.35)::float8, repeat('x', :pad)
FROM generate_series(1, :n) i;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) facts;
