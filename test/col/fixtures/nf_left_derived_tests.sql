-- the conditions of the nullable input of a LEFT JOIN that test the arrays of its rows (an EXISTS, a jsonpath test) and the aggregate filter that reads the computed test
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(DISTINCT w.id) AS n, count(DISTINCT w.id) FILTER (WHERE w.managed IS TRUE) AS managed, count(*) FILTER (WHERE w.id IS NULL) AS empty_stores
FROM stores s LEFT JOIN (
  SELECT x.id, x.entity_id, jsonb_path_exists(x.comments, '$[*] ? (@.type == "manager")') AS managed
  FROM t_wl x
  WHERE x.deleted_at IS NULL
    AND EXISTS (SELECT * FROM LATERAL (SELECT category AS c, polarity, score FROM jsonb_to_recordset(coldiff.wl_norm(x.doc) -> 'opinions') AS o(category text, polarity text, score float8) WHERE o.score IS NOT NULL) o WHERE o.polarity = 'positive')
) w ON w.entity_id = s.id
WHERE s.id <= 40
GROUP BY 1
ORDER BY 1
