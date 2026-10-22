-- a stable function in a CASE branch is refused
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, tz FROM t_coal ORDER BY CASE WHEN tz > now() THEN 1 ELSE 0 END, id LIMIT 12
