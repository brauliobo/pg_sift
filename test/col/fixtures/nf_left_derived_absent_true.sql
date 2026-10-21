-- a computed test that is true for an absent input (a NULL document) is not a condition the scan can drop the guard of
-- shape: join
-- expect: plain
-- key: -
-- scenario: yes
SELECT s.kind, count(*) FILTER (WHERE w.no_comments IS TRUE) AS n
FROM tenants s LEFT JOIN (SELECT x.dim_a_id, x.tags_a IS NULL AS no_comments FROM t_work x) w ON w.dim_a_id = s.id
WHERE s.id <= 40
GROUP BY 1
ORDER BY 1
