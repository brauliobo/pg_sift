-- the number of distinct values: count over a SELECT DISTINCT sub-select
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS customers FROM (SELECT DISTINCT customer_id FROM orders WHERE customer_id IS NOT NULL AND status <> 'cancelled') d
