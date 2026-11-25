-- ORDER BY a uuid, then a bytea and an integer key: the keys after the first are read for the rows that tie
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, u, b FROM t_misc ORDER BY u DESC, b, id LIMIT 20
