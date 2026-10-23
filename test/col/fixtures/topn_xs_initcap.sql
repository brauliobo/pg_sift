-- initcap follows the collation's ctype
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY initcap(t), id LIMIT 12
