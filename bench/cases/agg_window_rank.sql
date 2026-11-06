-- the rows of rank <= 3 of each key, rank() ... WHERE rn <= 3: the scan keeps the three best rows of each key and every row that ties with the third instead of the sort of every row
-- label: `SELECT * FROM (SELECT id, grp, amount, rank() OVER (PARTITION BY grp ORDER BY amount DESC NULLS LAST) rn FROM rk_fact WHERE grp <= 200) t WHERE rn <= 3` over 600,000 rows, 2,000 keys and 100 values of amount
-- ordered: no
--@ setup
CREATE TABLE rk_fact AS SELECT g AS id, 1 + g % 2000 AS grp, CASE WHEN g % 20 = 0 THEN NULL ELSE ((g / 2000) * 37 + g) % 100 END AS amount FROM generate_series(1, {rows} * 3) g;
ALTER TABLE rk_fact ALTER COLUMN grp SET NOT NULL;
ALTER TABLE rk_fact ADD PRIMARY KEY (id);
CREATE INDEX rk_fact_col ON rk_fact USING sift_col (id, grp, amount) WITH (key_cut = grp);
VACUUM ANALYZE rk_fact
--@ pg
SELECT * FROM (SELECT id, grp, amount, rank() OVER (PARTITION BY grp ORDER BY amount DESC NULLS LAST) AS rn FROM rk_fact WHERE grp <= 200) t WHERE rn <= 3
