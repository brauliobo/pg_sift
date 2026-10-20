-- trap: the arm's own WHERE keeps the cast off the scores that are no number, which a scan that casts every item cannot promise: not fused
-- (the regress test asserts the plan nodes), each arm is its own scan
-- shape: trap
-- expect: agg
-- key: -
WITH b AS (SELECT r.id AS rid, i.category AS k, i.score AS n
		   FROM t_work r, jsonb_to_recordset(coldiff.wl_txt(r.doc) -> 'parts') AS i(category text, score text) WHERE r.deleted_at IS NULL)
SELECT k, avg(a), count(DISTINCT rid)
FROM (SELECT rid, k, avg(n::float8) AS a FROM b WHERE n ~ '^-?[0-9]+(\.[0-9]+)?$' GROUP BY rid, k
	  UNION ALL SELECT rid, NULL::text, avg(n::float8) FROM b WHERE n ~ '^-?[0-9]+(\.[0-9]+)?$' GROUP BY rid) x
GROUP BY k
ORDER BY k NULLS FIRST
