-- the groups of a bigint column whose values lie in a dense range far from zero are numbered by pages of ids, as those of an int column are
-- label: `SELECT k, count(*), sum(amount) FROM gb_fact GROUP BY k`, a third of the rows' count in groups, values from 4,000,000,000
-- ordered: yes
--@ setup
CREATE TABLE gb_fact AS SELECT g AS id, 4000000000 + (g::bigint * 7919 % ({rows} / 3)) AS k, g % 97 AS amount FROM generate_series(1, {rows}) g;
CREATE INDEX gb_fact_col ON gb_fact USING sift_col (id, k, amount);
VACUUM (FREEZE, ANALYZE) gb_fact
--@ pg
SELECT k, count(*) AS n, sum(amount) AS s FROM gb_fact GROUP BY k ORDER BY k
