-- trap: the arms have a WHERE of their own that keeps the cast off the scores that are no number: a scan that cast every item would raise
-- where the heap plan does not; each arm is its own scan (or the normal plan), the rows are the same
-- shape: trap
-- expect: any
-- key: -
-- scenario: yes
WITH b AS (SELECT r.id AS rid, i.category AS k, i.score AS n
		   FROM t_work r, jsonb_to_recordset(coldiff.wl_txt(r.doc) -> 'parts') AS i(category text, score text) WHERE r.deleted_at IS NULL)
SELECT k, avg(a), count(DISTINCT rid)
FROM (SELECT rid, k, avg(n::float8) AS a FROM b WHERE n ~ '^-?[0-9]+(\.[0-9]+)?$' GROUP BY rid, k
	  UNION ALL SELECT rid, NULL::text, avg(n::float8) FROM b WHERE n ~ '^-?[0-9]+(\.[0-9]+)?$' AND k <> 'misc' GROUP BY rid) x
GROUP BY k
ORDER BY k NULLS FIRST
