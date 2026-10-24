-- a Top-N by a text column over the partitions: each partition has its own dictionary, the keys of the winners are compared as bytes
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, tag, qty FROM t_part ORDER BY tag DESC, qty NULLS FIRST, id LIMIT 20 OFFSET 3
