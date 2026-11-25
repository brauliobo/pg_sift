-- a Top-N whose first key has many ties across the partitions: the next key and the id decide
-- shape: topn
-- expect: any
-- key: -
-- scenario: yes
SELECT id, grp, qty FROM t_part ORDER BY grp DESC, qty NULLS FIRST, id LIMIT 30
