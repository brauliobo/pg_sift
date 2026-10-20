-- a correlated EXISTS over the parts of a document that is an index expression (the error `invalid attnum for relation` of the planner, then the push down)
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.dim_a_id % 5 AS g, count(*) AS n, sum(w.id) AS ids
FROM t_work w
WHERE w.deleted_at IS NULL AND EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(category text, tone text) WHERE o.tone = 'up')
GROUP BY 1
ORDER BY 1
