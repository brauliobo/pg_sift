-- a sum of a numeric column times a value the statement asks once (a sub-select that gives one row, a parameter): the scan sums the column and the product is made once for each group above it
-- label: `SELECT grp, sum(amount * (SELECT count(*) FROM fk_kinds)) FROM fk_fact GROUP BY grp` over 600,000 rows
-- ordered: yes
--@ setup
CREATE TABLE fk_kinds AS SELECT g AS id FROM generate_series(1, 30) g;
CREATE TABLE fk_fact AS SELECT g AS id, g % 53 AS grp, ((g::bigint * 7919 % 100000) / 100.0)::numeric(10,2) AS amount FROM generate_series(1, {rows}) g;
CREATE INDEX fk_fact_col ON fk_fact USING sift_col (id, grp, amount);
ANALYZE fk_fact;
ANALYZE fk_kinds
--@ pg
SELECT grp, sum(amount * (SELECT count(*) FROM fk_kinds)) AS s FROM fk_fact GROUP BY grp ORDER BY grp
