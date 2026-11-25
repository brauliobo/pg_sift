-- the rows that would overflow are filtered by the WHERE, which PostgreSQL evaluates first: no error
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 < 2147483647 ORDER BY i4 + 1, id LIMIT 12
