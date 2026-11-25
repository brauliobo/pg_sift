-- SELECT DISTINCT of one dictionary column, NULL included
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT status FROM orders ORDER BY status NULLS FIRST
