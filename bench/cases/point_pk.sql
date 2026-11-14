-- a statement with no aggregate, group, window or top-N shape, a lookup of one row by its primary key, is planned as PostgreSQL plans it: the planner hooks leave without reading the indexes of its table or walking its expressions
-- label: `SELECT * FROM pk_fact WHERE id = 300000` over 600,000 rows with a column index
-- ordered: no
--@ setup
CREATE TABLE pk_fact AS SELECT g AS id, 1 + g % 2000 AS grp, ((g::bigint * 7919 % 100000) / 100.0)::float8 AS amount FROM generate_series(1, {rows} * 3) g;
ALTER TABLE pk_fact ADD PRIMARY KEY (id);
CREATE INDEX pk_fact_col ON pk_fact USING sift_col (id, grp, amount) WITH (key_cut = grp);
VACUUM ANALYZE pk_fact
--@ pg
SELECT * FROM pk_fact WHERE id = 300000
