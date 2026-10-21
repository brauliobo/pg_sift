-- documents that are an array, a string, a number or a JSON null and values the leaves cannot store (a numeric string, a number where a text is declared): the
-- rows are read again from the heap and give what jsonb_to_recordset gives
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.k, count(*) AS n, count(o.v) AS nv, sum(o.n) AS sn, min(o.v) AS lo, max(o.v) AS hi, avg(o.v) AS av, count(*) FILTER (WHERE o.ok) AS oks
FROM t_rc r CROSS JOIN LATERAL jsonb_to_recordset(r.doc -> 'l') AS o(k text, v float8, n int4, ok bool)
WHERE r.grp <> 6
GROUP BY 1
ORDER BY 1 NULLS FIRST
