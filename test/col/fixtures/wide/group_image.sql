-- a key of a numeric image: 1.0 and 1.00 are one group; only the groups are counted, the spelling of a key is the first row's
-- shape: expr
-- expect: agg
-- key: -
SELECT count(*) AS groups, sum(n) AS rows FROM (SELECT nimg, count(*) AS n FROM t_wide GROUP BY nimg) g
