-- a key of a numeric image: 1.0 and 1.00 are one group whose spelling PostgreSQL picks
-- shape: expr
-- expect: plain
-- key: -
SELECT count(*) AS groups, sum(n) AS rows FROM (SELECT nimg, count(*) AS n FROM t_wide GROUP BY nimg) g
