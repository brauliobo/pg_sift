-- a list page tied on a date, deep OFFSET, after churn: `order_array_columns = 'auto'` weighs a date of fewer than 1,000 distinct values and chooses it, and the rows of the tie group that holds the first row to keep are read from the units by address, not fetched from the heap (the plan holds no more than 100 heap fetches)
-- label: `SELECT id, d ORDER BY d DESC, id LIMIT 20 OFFSET 10000` over {rows} rows of 365 dates in the order of a hash, the default index of 7 columns (no order option), after an `UPDATE` of 3% of v, 2% of k and 2% of deleted_at, a `DELETE` of 1% and a `VACUUM`; the variant `btree` is the same rows with a b-tree on (d DESC, id)
-- ordered: yes
--@ setup
CREATE TABLE oda (id int PRIMARY KEY, k int NOT NULL, d date NOT NULL, t text NOT NULL, v float8 NOT NULL, grp int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
INSERT INTO oda SELECT i, ((i::bigint * 7919) % {rows})::int + 1, DATE '2025-01-01' + ((hashint4(i) & 16777215) % 365), 't' || lpad(((i::bigint * 104729) % {rows})::text, 8, '0'), (hashint4(i + 1) & 16777215) % 1000 / 10.0, (hashint4(i + 2) & 16777215) % 1000, NULL FROM generate_series(1, {rows}) i;
CREATE INDEX oda_col ON oda USING sift_col (id, k, d, t, v, grp, deleted_at);
VACUUM (FREEZE, ANALYZE) oda;
UPDATE oda SET v = v + 1 WHERE id % 33 = 1;
UPDATE oda SET k = k + 1 WHERE id % 50 = 2;
UPDATE oda SET deleted_at = now() WHERE id % 50 = 3;
DELETE FROM oda WHERE id % 100 = 4;
VACUUM (ANALYZE) oda;
DROP SCHEMA IF EXISTS c_topn_offset_dates_auto__btree CASCADE;
CREATE SCHEMA c_topn_offset_dates_auto__btree;
CREATE TABLE c_topn_offset_dates_auto__btree.oda AS TABLE oda;
CREATE INDEX oda_bt ON c_topn_offset_dates_auto__btree.oda (d DESC, id);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_topn_offset_dates_auto__btree.oda
--@ pg
SELECT id, d FROM oda ORDER BY d DESC, id LIMIT 20 OFFSET 10000
