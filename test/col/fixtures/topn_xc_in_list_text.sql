-- IN of strings under the collation of the column
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY t IN ('a', 'B', 'Z') DESC, id LIMIT 12
