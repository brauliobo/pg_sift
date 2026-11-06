-- the first three rows of each key with row_number() ... WHERE rn <= 3: a probe of the table for each key (a top-3 of the key) instead of the sort of every row
-- label: `SELECT * FROM (SELECT id, grp, amount, row_number() OVER (PARTITION BY grp ORDER BY amount DESC NULLS LAST, id) rn FROM wn_fact WHERE grp <= 200) t WHERE rn <= 3` over 600,000 rows and 2,000 keys
-- ordered: no
--@ setup
CREATE TABLE wn_fact AS SELECT g AS id, 1 + g % 2000 AS grp, CASE WHEN g % 20 = 0 THEN NULL ELSE ((g::bigint * 7919 % 100000) / 100.0)::float8 END AS amount FROM generate_series(1, {rows} * 3) g;
ALTER TABLE wn_fact ALTER COLUMN grp SET NOT NULL;
ALTER TABLE wn_fact ADD PRIMARY KEY (id);
CREATE INDEX wn_fact_col ON wn_fact USING sift_col (id, grp, amount) WITH (key_cut = grp);
VACUUM ANALYZE wn_fact
--@ pg
SELECT * FROM (SELECT id, grp, amount, row_number() OVER (PARTITION BY grp ORDER BY amount DESC NULLS LAST, id) AS rn FROM wn_fact WHERE grp <= 200) t WHERE rn <= 3
