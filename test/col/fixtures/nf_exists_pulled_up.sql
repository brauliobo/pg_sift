-- an EXISTS over the parts through a sub-select PostgreSQL pulls up: the sub-plan keeps the dead entry of the sub-select before the function
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.entity_id % 5 AS g, count(*) AS n, sum(w.id) AS ids
FROM t_load w
WHERE w.deleted_at IS NULL AND EXISTS (SELECT * FROM LATERAL (SELECT category AS c, polarity, score FROM jsonb_to_recordset(coldiff.load_norm(w.doc) -> 'parts') AS o(category text, polarity text, score float8) WHERE o.score IS NOT NULL) o WHERE o.polarity = 'positive')
GROUP BY 1
ORDER BY 1
