-- a computed test that is true for an absent input (a NULL document) is not a condition the scan can drop the guard of
-- shape: join
-- expect: plain
-- key: -
-- scenario: yes
SELECT s.kind, count(*) FILTER (WHERE w.no_comments IS TRUE) AS n
FROM stores s LEFT JOIN (SELECT x.entity_id, x.comments IS NULL AS no_comments FROM t_wl x) w ON w.entity_id = s.id
WHERE s.id <= 40
GROUP BY 1
ORDER BY 1
