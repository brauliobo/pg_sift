-- the reads of the same sift_table after its VACUUM: the units that kept a dead row are answered from pre-aggregates net of it
-- label: `SELECT country, count(*), sum(amount) FROM t GROUP BY country` over a `sift_table` after a `DELETE` of 4.5% of the rows, and its `VACUUM`
-- ordered: yes
--@ setup
CREATE TABLE trs USING sift_table WITH (autovacuum_enabled = false) AS SELECT id, country, amount, created_at FROM orders ORDER BY id;
VACUUM (FREEZE, ANALYZE) trs;
DELETE FROM trs WHERE id % 22 = 3;
VACUUM (ANALYZE) trs
--@ pg
SELECT country, count(*), sum(amount) FROM trs GROUP BY country
