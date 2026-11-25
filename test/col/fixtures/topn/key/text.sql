-- ORDER BY a text column: mixed case, accents, an empty string, strings longer than a dictionary entry and NULLs, under the collation of the column
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY t, id LIMIT 12 OFFSET 2
