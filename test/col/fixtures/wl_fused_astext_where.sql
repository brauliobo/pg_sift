-- the arms share one WHERE that keeps the cast off the scores that are no number: it is the scan's own, and the fused scan casts only what passes it
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
WITH b AS (SELECT r.id AS rid, i.category AS k, i.score AS n
		   FROM t_wl r, jsonb_to_recordset(coldiff.wl_txt(r.doc) -> 'opinions') AS i(category text, score text) WHERE r.deleted_at IS NULL)
SELECT k, avg(a), count(DISTINCT rid)
FROM (SELECT rid, k, avg(n::float8) AS a FROM b WHERE n ~ '^-?[0-9]+(\.[0-9]+)?$' GROUP BY rid, k
	  UNION ALL SELECT rid, NULL::text, avg(n::float8) FROM b WHERE n ~ '^-?[0-9]+(\.[0-9]+)?$' GROUP BY rid) x
GROUP BY k
ORDER BY k NULLS FIRST
