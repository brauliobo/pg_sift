-- trap: OFFSET in the sub-query skips the first matching element
-- shape: trap
-- expect: plain
-- key: -
SELECT e.grp, count(*) AS n
FROM t_eo e
WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q >= e.r4 OFFSET 1)
GROUP BY 1
ORDER BY 1
