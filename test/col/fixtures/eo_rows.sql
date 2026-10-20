-- the rows that match, not only their number: an element equal to the column of another row (the neighbour holds the value of the next one) must not match
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT e.grp, count(*) AS n, sum(e.id) AS ids, min(e.id) AS first_id, max(e.id) AS last_id, count(DISTINCT e.id) AS distinct_ids
FROM t_eo e
WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q = e.r4)
GROUP BY 1
ORDER BY 1
