-- the same with a condition on the inner average (a cell of the arm that fails it is no group) and a filtered outer aggregate (its bound is no
-- average of one-decimal scores, whose last bit depends on the order of the additions)
-- shape: item-summary-rollup
-- expect: agg
-- key: -
-- scenario: yes
WITH b AS (SELECT r.id AS rid, i.category AS k, CASE WHEN i.score ~ '^-?[0-9]+(\.[0-9]+)?$' THEN i.score::float8 END AS n
		   FROM t_work r, jsonb_to_recordset(coldiff.wl_txt(r.doc) -> 'parts') AS i(category text, score text) WHERE r.deleted_at IS NULL)
SELECT k, avg(a) FILTER (WHERE a >= 5.03) AS high, count(DISTINCT rid) AS items
FROM (SELECT rid, k, avg(n) AS a FROM b GROUP BY rid, k HAVING avg(n) > 1
	  UNION ALL SELECT rid, NULL::text, avg(n) FROM b GROUP BY rid) x
GROUP BY k
ORDER BY k NULLS FIRST
