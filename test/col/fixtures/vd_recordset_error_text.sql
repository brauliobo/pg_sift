-- trap: a text that is no number in the float8 column of jsonb_to_recordset raises the error of the function
-- shape: trap
-- expect: agg
-- error: yes
-- key: -
SELECT count(*) AS n, sum(o.v) AS s
FROM t_vd d CROSS JOIN LATERAL jsonb_to_recordset(d.doc -> 'l') AS o(v float8)
WHERE d.grp = 2
