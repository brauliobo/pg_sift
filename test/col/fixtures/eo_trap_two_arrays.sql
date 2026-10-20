-- trap: a second array in the sub-query pairs the elements of both, whatever the comparison with the column is
-- shape: trap
-- expect: plain
-- key: -
SELECT e.grp, count(*) AS n
FROM t_eo e
WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int), jsonb_to_recordset(e.doc) AS j(q8 bigint) WHERE i.q = e.r4 AND j.q8 > i.q)
GROUP BY 1
ORDER BY 1
