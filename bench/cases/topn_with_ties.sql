-- FETCH FIRST n ROWS WITH TIES: the n best rows and every row that ties with the n-th, as a top-N of the scan
-- label: `ORDER BY dim_a_id DESC FETCH FIRST 10 ROWS WITH TIES` over {rows} rows, 1,000 values of the key (about {rows}/1000 rows tie)
-- ordered: no
-- set: sift.agg_force = on
--@ setup
DROP TABLE IF EXISTS tw_fact;
CREATE TABLE tw_fact (id int, dim_a_id int, amount float8) WITH (autovacuum_enabled = false);
INSERT INTO tw_fact SELECT g, (g::bigint * 7919 % 1000)::int, (g % 977) / 10.0 FROM generate_series(1, {rows}) g;
CREATE INDEX tw_fact_col ON tw_fact USING sift_col (id, dim_a_id, amount);
VACUUM (ANALYZE) tw_fact
--@ pg
SELECT id, dim_a_id FROM tw_fact ORDER BY dim_a_id DESC FETCH FIRST 10 ROWS WITH TIES
