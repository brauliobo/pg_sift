-- a key and comparisons of bytea: the empty value, NUL bytes, the shorter prefix first
-- shape: expr
-- expect: agg
-- key: -
SELECT encode(b, 'hex') AS h, count(*) AS n FROM t_misc GROUP BY b ORDER BY b
