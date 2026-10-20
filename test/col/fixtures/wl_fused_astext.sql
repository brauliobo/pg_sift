-- per-item arms over a text leaf with the cast in the aggregate: the arms are fused, the cast guarded for the scores that are no number
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
WITH b AS (SELECT r.id AS rid, i.category AS k, CASE WHEN i.score ~ '^-?[0-9]+(\.[0-9]+)?$' THEN i.score::float8 END AS n
		   FROM t_wl r, jsonb_to_recordset(coldiff.wl_txt(r.doc) -> 'opinions') AS i(category text, score text) WHERE r.deleted_at IS NULL)
SELECT k, avg(a) AS value, count(DISTINCT rid) AS items, sum(s) AS total
FROM (SELECT rid, k, avg(n) AS a, sum(n) AS s FROM b GROUP BY rid, k
	  UNION ALL SELECT rid, NULL::text, avg(n), sum(n) FROM b GROUP BY rid) x
GROUP BY k
ORDER BY k NULLS FIRST
