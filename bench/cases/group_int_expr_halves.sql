-- the groups of an integer expression of a column with many values (one group for two rows) are numbered from the value by pages of ids while the values lie in a dense range, not by two hash tables
-- label: `SELECT id / 2 AS g, count(*) FROM gi_fact GROUP BY 1`, one group for two rows, the rows in no order of id
-- ordered: yes
--@ setup
CREATE TABLE gi_fact AS SELECT g::bigint AS id, g % 97 AS amount FROM generate_series(1, {rows}) g ORDER BY hashint8(g);
CREATE INDEX gi_fact_col ON gi_fact USING sift_col (id, amount);
VACUUM (FREEZE, ANALYZE) gi_fact
--@ pg
SELECT id / 2 AS g, count(*) AS n FROM gi_fact GROUP BY 1 ORDER BY 1
