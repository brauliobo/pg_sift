-- bytea ordering and equality in a FILTER, and count(DISTINCT)
-- shape: expr
-- expect: agg
-- key: -
SELECT count(*) FILTER (WHERE b > '\x00ff') AS gt, count(*) FILTER (WHERE b = '\x') AS empty, count(DISTINCT b) AS d FROM t_misc
