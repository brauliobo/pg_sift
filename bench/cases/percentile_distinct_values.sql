-- the values of an ordered set that are nearly all distinct are appended to an array of pairs, where they were counted in a hash that held a quarter as many and moved to a sort past that
-- label: `percentile_disc(0.5)` and `percentile_disc(0.95)` of a float8 column of distinct values by 12 groups (about 17,000 values a group)
-- ordered: yes
--@ setup
CREATE TABLE pc_fact AS SELECT g AS id, g % 12 AS grp, ((g::bigint * 7919 % 1000003) / 100.0)::float8 AS amount FROM generate_series(1, {rows}) g;
CREATE INDEX pc_fact_col ON pc_fact USING sift_col (id, grp, amount);
VACUUM (FREEZE, ANALYZE) pc_fact
--@ pg
SELECT grp, percentile_disc(0.5) WITHIN GROUP (ORDER BY amount) AS median, percentile_disc(0.95) WITHIN GROUP (ORDER BY amount) AS p95 FROM pc_fact GROUP BY grp ORDER BY grp
