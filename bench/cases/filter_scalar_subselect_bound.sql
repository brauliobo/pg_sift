-- a condition against a scalar sub-select that gives one row (an average, a maximum, a row by its unique key): the scan asks it once when it starts and tests the rows against the value, as against a parameter
-- label: `WHERE amount > (SELECT avg(amount) FROM fact)` GROUP BY grp over 600,000 rows
-- ordered: yes
--@ setup
CREATE TABLE fs_fact AS SELECT g AS id, g % 53 AS grp, (g::bigint * 7919 % 1000)::int AS amount FROM generate_series(1, {rows}) g;
CREATE INDEX fs_fact_col ON fs_fact USING sift_col (id, grp, amount);
ANALYZE fs_fact
--@ pg
SELECT grp, count(*) AS n FROM fs_fact WHERE amount > (SELECT avg(amount) FROM fs_fact) GROUP BY grp ORDER BY grp
