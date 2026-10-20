-- DISTINCT ON: expressions of the row's columns in the select list, a column that is also a sort key, a constant
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (store_id) store_id, id, total * 2 AS dbl, coalesce(status, '-') AS st, upper(lang) AS l, placed_at, total IS NULL AS no_total, 'x' AS k
FROM orders WHERE store_id < 40 ORDER BY store_id, placed_at DESC, id
