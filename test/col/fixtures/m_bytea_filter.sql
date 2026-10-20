-- bytea ordering, min and max
-- shape: expr
-- expect: agg
-- key: -
SELECT count(*) FILTER (WHERE b > '\x00ff') AS gt, count(*) FILTER (WHERE b = '\x') AS empty, encode(min(b), 'hex') AS mn, encode(max(b), 'hex') AS mx, count(DISTINCT b) AS d FROM t_misc
