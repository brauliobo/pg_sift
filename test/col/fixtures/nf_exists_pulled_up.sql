-- an EXISTS over the parts through a sub-select PostgreSQL pulls up: the sub-plan keeps the dead entry of the sub-select before the function
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.dim_a_id % 5 AS g, count(*) AS n, sum(w.id) AS ids
FROM t_work w
WHERE w.deleted_at IS NULL AND EXISTS (SELECT * FROM LATERAL (SELECT category AS c, dir_a, score FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(category text, dir_a text, score float8) WHERE o.score IS NOT NULL) o WHERE o.dir_a = 'up')
GROUP BY 1
ORDER BY 1
