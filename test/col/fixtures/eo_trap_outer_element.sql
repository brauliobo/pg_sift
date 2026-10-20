-- trap: the other side is an element of the array the query unnests, not a column of the row
-- shape: trap
-- expect: plain
-- key: -
SELECT e.grp, count(*) AS n
FROM t_eo e, jsonb_to_recordset(e.doc) AS a(q8 bigint)
WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q = a.q8)
GROUP BY 1
ORDER BY 1
