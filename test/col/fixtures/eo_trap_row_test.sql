-- trap: a test of the column alone in the sub-query is not a comparison of the element
-- shape: trap
-- expect: plain
-- key: -
SELECT e.grp, count(*) AS n
FROM t_eo e
WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q IS NULL AND e.r4 > 3)
GROUP BY 1
ORDER BY 1
