-- quals: IS NULL and IS NOT NULL as group keys and quals
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT (country_code IS NULL) AS no_country, (status IS NULL) AS no_status, (customer_id IS NULL) AS no_customer, count(*) AS n
FROM orders
WHERE deleted_at IS NULL OR deleted_at IS NOT NULL
GROUP BY 1, 2, 3
ORDER BY 1, 2, 3
