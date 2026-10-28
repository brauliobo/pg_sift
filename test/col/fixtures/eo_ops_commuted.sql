-- the column of the row on the left of the comparison: the operator is the same one seen from the element
-- shape: items-exists-same-element
-- expect: agg
-- key: -
-- scenario: yes
SELECT e.grp, count(*) AS n,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE e.r4 = i.q)) AS eq,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE e.r4 <> i.q)) AS ne,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE e.r4 < i.q)) AS lt,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE e.r4 <= i.q)) AS le,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE e.r4 > i.q)) AS gt,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE e.r4 >= i.q)) AS ge
FROM t_eo e
GROUP BY 1
ORDER BY 1
