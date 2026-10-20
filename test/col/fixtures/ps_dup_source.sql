-- a key of the dimension with two rows: both rows meet each order (the orders are repeated), a source without a row drops its orders, and a row without orders stays
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT d.region, count(*) AS rows, count(o.id) AS orders, sum(o.total) AS revenue
FROM dim_source d LEFT JOIN orders o ON o.source = d.source AND o.country_code = 'US'
GROUP BY d.region ORDER BY d.region
