-- a jsonb result: refused
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, grp FROM t_coal ORDER BY to_jsonb(grp), id LIMIT 12
