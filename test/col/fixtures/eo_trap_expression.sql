-- trap: the column is inside an expression, which is not the value of a leaf
-- shape: trap
-- expect: plain
-- key: -
SELECT e.grp, count(*) AS n
FROM t_eo e
WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q = e.r4 + 1)
GROUP BY 1
ORDER BY 1
