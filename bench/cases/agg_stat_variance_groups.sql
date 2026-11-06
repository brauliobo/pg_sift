-- the variance and the standard deviation of a float8 column and of an int4 column of a few groups are added up by a kernel over the chunk, where the transition function ran for each row
-- label: `SELECT g, stddev(amount), variance(score) FROM st_fact GROUP BY g`, 12 groups
-- ordered: yes
-- set: sift.agg_force = on
--@ setup
CREATE TABLE st_fact AS SELECT g AS id, g % 12 AS grp, ((g::bigint * 7919 % 100000) / 100.0)::float8 AS amount, (g::bigint * 104729 % 1000)::int AS qty, (g % 311)::int AS score FROM generate_series(1, {rows}) g;
CREATE INDEX st_fact_col ON st_fact USING sift_col (id, grp, amount, qty, score);
VACUUM (FREEZE, ANALYZE) st_fact
--@ pg
SELECT grp, stddev(amount) AS sd, variance(score) AS v FROM st_fact GROUP BY grp ORDER BY grp
