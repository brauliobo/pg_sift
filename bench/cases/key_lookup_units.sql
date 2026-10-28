-- one key of 20,001 over 1,000,000 rows, in an index cut by (key, date) into 15,638 units (the main lane), 4,008 (variant `u4008`) and 500 (variant `u500`): the scan skips the morsels of units the zones clear, so the units of the index do not add to the lookup
-- label: `SELECT count(*), sum(amount) WHERE k = <one key> AND d` over two years, `key_cut = 'k, d'` with `key_cut_rows` 60, 250 and 2000; the variants are the same rows cut into fewer units
-- ordered: no
--@ setup
DROP SCHEMA IF EXISTS key_lookup_u4008 CASCADE;
DROP SCHEMA IF EXISTS key_lookup_u500 CASCADE;
CREATE SCHEMA key_lookup_u4008;
CREATE SCHEMA key_lookup_u500;
CREATE TABLE key_rows AS SELECT i AS id, (hashint8extended(i, 5) & 1048575)::int % 20001 AS k, date '2022-01-01' + ((hashint8extended(i, 9) & 1048575)::int % 1461) AS d, (hashint8extended(i, 11) & 1023)::int AS amount FROM generate_series(1, 1000000) i;
CREATE TABLE key_lookup_u4008.key_rows AS TABLE key_rows;
CREATE TABLE key_lookup_u500.key_rows AS TABLE key_rows;
CREATE INDEX key_rows_col ON key_rows USING sift_col (id, k, d, amount) WITH (key_cut = 'k, d', key_cut_rows = 60);
CREATE INDEX key_rows_col ON key_lookup_u4008.key_rows USING sift_col (id, k, d, amount) WITH (key_cut = 'k, d', key_cut_rows = 250);
CREATE INDEX key_rows_col ON key_lookup_u500.key_rows USING sift_col (id, k, d, amount) WITH (key_cut = 'k, d', key_cut_rows = 2000);
VACUUM (FREEZE, ANALYZE) key_rows;
VACUUM (FREEZE, ANALYZE) key_lookup_u4008.key_rows;
VACUUM (FREEZE, ANALYZE) key_lookup_u500.key_rows;
--@ pg
SELECT count(*) AS n, sum(amount) AS total FROM key_rows WHERE k = 4242 AND d >= date '2024-01-01' AND d < date '2026-01-01'
