-- trap: a LIMIT that is not a constant stays in the sub-query (a constant LIMIT of an EXISTS is dropped by PostgreSQL: it changes nothing)
-- shape: trap
-- expect: plain
-- key: -
SELECT e.grp, count(*) AS n
FROM t_eo e
WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q = e.r4 LIMIT (SELECT 1))
GROUP BY 1
ORDER BY 1
