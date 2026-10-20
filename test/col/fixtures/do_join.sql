-- DISTINCT ON over an inner join to a dimension table that filters the rows: the select list and the sort keys are columns of the fact table
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (o.store_id) o.store_id, o.id, o.placed_at, o.total
FROM orders o JOIN stores s ON s.id = o.store_id AND s.kind <> 'outlet' ORDER BY o.store_id, o.placed_at DESC, o.id
