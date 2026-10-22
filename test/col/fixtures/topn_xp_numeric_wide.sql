-- numeric of 19..38 digits is a two-lane leaf: the evaluator does not read it, the normal plan answers
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, n38 FROM t_wide ORDER BY n38 * 2, id LIMIT 12
