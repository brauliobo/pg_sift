-- timestamptz minus an interval depends on the session time zone (stable): refused
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, tz FROM t_coal ORDER BY tz - interval '3 hours' DESC NULLS LAST, id LIMIT 12
