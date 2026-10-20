-- a correlated EXISTS over the opinions of a document that is an index expression (the error `invalid attnum for relation` of the planner, then the push down)
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.entity_id % 5 AS g, count(*) AS n, sum(w.id) AS ids
FROM t_wl w
WHERE w.deleted_at IS NULL AND EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.wl_norm(w.doc) -> 'opinions') AS o(category text, polarity text) WHERE o.polarity = 'positive')
GROUP BY 1
ORDER BY 1
