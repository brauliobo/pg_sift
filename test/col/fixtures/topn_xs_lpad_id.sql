-- a zero-padded id as text sorts as text
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, grp FROM t_coal ORDER BY lpad((id % 97)::text, 4, '0'), id LIMIT 12
