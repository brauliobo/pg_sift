-- the query unnests the array itself and tests an element of it against a column of the row: the elements of the sub-query and of the query are the same leaves
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT e.grp, count(*) AS elements, sum(a.q8) AS q8_sum, count(*) FILTER (WHERE a.q8 < e.r4) AS below_column
FROM t_eo e, jsonb_to_recordset(e.doc) AS a(q8 bigint)
WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q >= e.r4)
GROUP BY 1
ORDER BY 1
