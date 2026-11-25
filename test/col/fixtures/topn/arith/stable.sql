-- a stable function (it depends on the session) is refused
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, tz FROM t_coal ORDER BY tz - now() DESC, id LIMIT 12
