-- an EXISTS over the opinions through a sub-select PostgreSQL pulls up: the sub-plan keeps the dead entry of the sub-select before the function
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.entity_id % 5 AS g, count(*) AS n, sum(w.id) AS ids
FROM t_wl w
WHERE w.deleted_at IS NULL AND EXISTS (SELECT * FROM LATERAL (SELECT category AS c, polarity, score FROM jsonb_to_recordset(coldiff.wl_norm(w.doc) -> 'opinions') AS o(category text, polarity text, score float8) WHERE o.score IS NOT NULL) o WHERE o.polarity = 'positive')
GROUP BY 1
ORDER BY 1
