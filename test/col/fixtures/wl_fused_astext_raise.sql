-- trap: the plain cast raises for a score that is no number, as the heap plan does: the same error from both plans
-- shape: trap
-- expect: agg
-- key: -
-- error: yes
WITH b AS (SELECT r.id AS rid, i.category AS k, i.score AS n
		   FROM t_wl r, jsonb_to_recordset(coldiff.wl_txt(r.doc) -> 'opinions') AS i(category text, score text) WHERE r.deleted_at IS NULL)
SELECT k, avg(a), count(DISTINCT rid)
FROM (SELECT rid, k, avg(n::float8) AS a FROM b GROUP BY rid, k
	  UNION ALL SELECT rid, NULL::text, avg(n::float8) FROM b GROUP BY rid) x
GROUP BY k
ORDER BY k NULLS FIRST
