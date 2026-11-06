-- the correlation and the regression slope of two columns of a few groups are added up by a kernel over the chunk (an int4 column converted to float8 included)
-- label: `SELECT g, corr(amount, qty), regr_slope(amount, score) FROM sc_fact GROUP BY g`, 12 groups
-- ordered: yes
-- set: sift.agg_force = on
--@ setup
CREATE TABLE sc_fact AS SELECT g AS id, g % 12 AS grp, ((g::bigint * 7919 % 100000) / 100.0)::float8 AS amount, (g::bigint * 104729 % 1000)::int AS qty, (g % 311)::int AS score FROM generate_series(1, {rows}) g;
CREATE INDEX sc_fact_col ON sc_fact USING sift_col (id, grp, amount, qty, score);
VACUUM (FREEZE, ANALYZE) sc_fact
--@ pg
SELECT grp, corr(amount, qty) AS c, regr_slope(amount, score) AS s FROM sc_fact GROUP BY grp ORDER BY grp
