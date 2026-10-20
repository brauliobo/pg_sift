-- DISTINCT ON under a top-level ORDER BY ... LIMIT of the groups, with an OFFSET
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (store_id) store_id, id, placed_at FROM orders ORDER BY store_id, placed_at DESC, id LIMIT 7 OFFSET 2
