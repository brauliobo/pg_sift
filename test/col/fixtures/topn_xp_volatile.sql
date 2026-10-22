-- a volatile function is evaluated for every row by the plan's sort and is not repeatable: refused
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 BETWEEN -500 AND 500 ORDER BY i4 + (random() * 0)::int, id LIMIT 12
