-- window: a rank by count(DISTINCT) and a moving frame over the groups
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT store_id, count(DISTINCT customer_id) AS customers, dense_rank() OVER (ORDER BY count(DISTINCT customer_id) DESC) AS rnk,
       sum(count(*)) OVER (ORDER BY store_id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS moving
FROM orders
WHERE store_id < 60
GROUP BY store_id
ORDER BY store_id
