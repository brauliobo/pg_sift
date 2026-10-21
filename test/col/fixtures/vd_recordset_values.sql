-- numeric strings, a JSON null, NaN, Infinity and padded strings in the float8 column of jsonb_to_recordset: the rows are read again from the heap and give what the function gives
-- shape: S7
-- expect: agg
-- key: 1
SELECT d.grp, count(*) AS n, count(o.v) AS nv, sum(o.v) AS s, min(o.v) AS lo, max(o.v) AS hi, count(*) FILTER (WHERE o.v > 2) AS big
FROM t_vd d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> 'l') AS o(v float8)
WHERE d.grp IN (0, 1, 5, 7, 8, 10, 11)
GROUP BY d.grp ORDER BY d.grp
