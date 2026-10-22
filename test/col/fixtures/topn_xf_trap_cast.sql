-- a cast of the column
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY i4::bigint, id LIMIT 12
