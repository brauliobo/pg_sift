-- trap: a row that holds a string that is no number reaches jsonb_to_recordset: the error of the heap plan, the same from both plans
-- shape: trap
-- expect: agg
-- key: -
-- error: yes
SELECT o.k, count(*) AS n, max(o.v) AS hi
FROM t_rc r CROSS JOIN LATERAL jsonb_to_recordset(r.doc -> 'l') AS o(k text, v float8, n int4, ok bool)
GROUP BY 1
ORDER BY 1 NULLS FIRST
