-- the distribution of spend per customer: an aggregate of an aggregate, 200,000 customers at 1,000,000 orders
-- label: `GROUP BY width_bucket(spend, ..)` over `SELECT customer_id, sum(amount) .. GROUP BY customer_id`
-- ordered: yes
--@ pg
SELECT width_bucket(spend, 0, 2000, 10) AS bucket, count(*) AS customers, sum(spend) AS revenue FROM (SELECT customer_id, sum(amount) AS spend FROM orders GROUP BY customer_id) t GROUP BY 1 ORDER BY 1
