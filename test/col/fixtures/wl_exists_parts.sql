-- a correlated EXISTS over the parts of a document that is an index expression (the error `invalid attnum for relation` of the planner, then the push down)
-- shape: items-exists-same-element
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.dim_a_id % 5 AS g, count(*) AS n, sum(w.id) AS ids
FROM t_work w
WHERE w.deleted_at IS NULL AND EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(category text, dir_a text) WHERE o.dir_a = 'up')
GROUP BY 1
ORDER BY 1
