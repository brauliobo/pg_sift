-- the percentiles of an array of fractions, where PostgreSQL's transition function sorts every value of the group in a tuplesort
-- label: `percentile_cont(array[0.25, 0.5, 0.75])` of a float8 column by 12 groups (25,000 values a group at the default size)
-- ordered: no
--@ setup
CREATE TABLE pq_fact AS SELECT g AS id, g % 12 AS grp, ((g::bigint * 7919 % 1000003) / 100.0)::float8 AS amount FROM generate_series(1, {rows}) g;
CREATE INDEX pq_fact_col ON pq_fact USING sift_col (id, grp, amount);
VACUUM (FREEZE, ANALYZE) pq_fact
--@ pg
SELECT grp, percentile_cont(array[0.25, 0.5, 0.75]) WITHIN GROUP (ORDER BY amount) FROM pq_fact GROUP BY grp
