-- NOT EXISTS: a NULL on either side is unknown, so the element does not match and an empty or absent array has no element at all
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT e.grp, count(*) AS n,
       count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q = e.r4)) AS none_eq,
       count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q <> e.r4)) AS none_ne,
       count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q IS NULL OR i.q = e.r4)) AS none_null_or_eq
FROM t_eo e
GROUP BY 1
ORDER BY 1
