-- the check against the heap of the rows a scan selects on pages that are not all-visible (update and delete churn, no VACUUM since) visits the selected rows only
-- label: `SELECT count(*) WHERE bucket = 7` (1% of the rows) after an `UPDATE` of 5% and a `DELETE` of 5% of the rows with no `VACUUM` since
-- ordered: yes
-- vacuum: no
--@ setup
CREATE TABLE ccn WITH (autovacuum_enabled = false) AS SELECT id, (hashint4(id) & 2147483647) % 100 AS bucket, amount, created_at FROM orders ORDER BY id;
CREATE INDEX ccn_col ON ccn USING sift_col (id, bucket, amount, created_at);
VACUUM (FREEZE, ANALYZE) ccn;
UPDATE ccn SET amount = amount + 1 WHERE id % 20 = 1;
DELETE FROM ccn WHERE id % 20 = 2;
ANALYZE ccn
--@ pg
SELECT count(*) FROM ccn WHERE bucket = 7
