-- a query that reads a unit with dead rows reads its liveness bits 64 at a time: a grouped sum over a 13-column index after a DELETE of 9% of the rows with no VACUUM since
-- label: `SELECT status, sum(amount), min(quantity), max(quantity) .. GROUP BY status` over a 13-column `sift_col` index with 9% of its rows dead
-- ordered: yes
-- vacuum: no
--@ setup
CREATE TABLE sdu WITH (autovacuum_enabled = false) AS SELECT id, customer_id, country, category, status, amount, quantity, created_at, (id % 97)::int AS a, (id % 1013)::int AS b, (id % 7)::smallint AS c, (id % 31)::int AS d, (id % 12 + 1)::int AS e FROM orders ORDER BY id;
CREATE INDEX sdu_col ON sdu USING sift_col (customer_id, country, category, status, amount, quantity, created_at, a, b, c, d, e);
VACUUM (FREEZE, ANALYZE) sdu;
DELETE FROM sdu WHERE id % 11 = 3;
ANALYZE sdu
--@ pg
SELECT status, sum(amount), min(quantity), max(quantity), count(*) FROM sdu GROUP BY status ORDER BY status
