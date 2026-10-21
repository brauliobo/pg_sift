-- a test on the elements and on the row, with the rows that hold a value the leaves cannot store among those tested
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT r.grp, count(*) AS n, sum(o.n) AS sn, max(o.v) AS hi
FROM t_rc r CROSS JOIN LATERAL jsonb_to_recordset(r.doc -> 'l') AS o(k text, v float8, n int4, ok bool)
WHERE r.grp <> 6 AND o.v > 1.5 AND o.k <> 'k0'
GROUP BY 1
ORDER BY 1
