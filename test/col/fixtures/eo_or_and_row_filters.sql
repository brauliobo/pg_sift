-- the comparison with the column among the other conjuncts and disjuncts of the element and of the row
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT e.grp, count(*) AS n
FROM t_eo e
WHERE e.id % 3 <> 0 AND e.rf IS NOT NULL
  AND EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int, q8 bigint) WHERE i.q = e.r4 OR i.q8 = 5 OR i.q IS NULL)
  AND NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q > e.r4)
GROUP BY 1
ORDER BY 1
