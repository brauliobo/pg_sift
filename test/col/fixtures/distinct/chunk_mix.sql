-- count(DISTINCT) from workers with chunks of both kinds in one blob: dense ones (ORed by the leader) and ones under 32 bits (pairs)
-- shape: distinct
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(DISTINCT v) AS dense, count(DISTINCT v) FILTER (WHERE id % 97 = 0) AS sparse, count(DISTINCT x) FILTER (WHERE id % 5 = 0) AS mid,
	   count(DISTINCT w) AS wide
FROM t_dist GROUP BY grp ORDER BY grp
