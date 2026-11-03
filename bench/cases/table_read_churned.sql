-- the reads of a sift_table with 4.5% of its rows deleted and no VACUUM since: the aggregate scan reads the rows of each unit that holds a dead row
-- label: `SELECT country, count(*), sum(amount) FROM t GROUP BY country ORDER BY country` over a `sift_table` after a `DELETE` of 4.5% of the rows, before the `VACUUM`
-- ordered: yes
-- vacuum: no
--@ setup
CREATE TABLE trc USING sift_table WITH (autovacuum_enabled = false) AS SELECT id, country, amount, created_at FROM orders ORDER BY id;
VACUUM (FREEZE, ANALYZE) trc;
DELETE FROM trc WHERE id % 22 = 3;
ANALYZE trc
--@ pg
SELECT country, count(*), sum(amount) FROM trc GROUP BY country ORDER BY country
