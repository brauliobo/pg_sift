-- flat: count(DISTINCT) of dictionary and integer columns without a group
-- shape: flat
-- expect: agg
-- key: -
SELECT count(DISTINCT country_code) AS countries, count(DISTINCT source) AS sources, count(DISTINCT store_id) AS stores,
       count(DISTINCT status) AS statuses, count(DISTINCT lang) AS langs, count(DISTINCT customer_id) AS customers
FROM orders
