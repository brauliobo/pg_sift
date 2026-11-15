-- a condition that wraps a column in coalesce(col, c) is tested as the column: the comparison kernels run it instead of the expression for every row
-- label: `SELECT grp, count(*), sum(amount) FROM pn_fact WHERE coalesce(amount, 0) > 990 GROUP BY grp` over 600,000 rows, 5% of the amounts NULL
-- ordered: yes
--@ setup
CREATE TABLE pn_fact AS SELECT g AS id, g % 53 AS grp,
       CASE WHEN g % 20 = 0 THEN NULL ELSE ((g::bigint * 7919 % 100000) / 100.0)::float8 END AS amount FROM generate_series(1, {rows} * 3) g;
CREATE INDEX pn_fact_col ON pn_fact USING sift_col (id, grp, amount);
ANALYZE pn_fact
--@ pg
SELECT grp, count(*) AS n, sum(amount) AS total FROM pn_fact WHERE coalesce(amount, 0) > 990 GROUP BY grp ORDER BY grp
