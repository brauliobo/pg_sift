-- ordered sets over row columns: percentile_disc of integers and dates, percentile_cont of integers, with NULL groups
-- shape: G7
-- expect: agg
-- key: -
-- scenario: yes
SELECT country_code, percentile_disc(0.25) WITHIN GROUP (ORDER BY store_id) AS s25, percentile_disc(0.9) WITHIN GROUP (ORDER BY placed_at DESC) AS d90,
       percentile_cont(0.5) WITHIN GROUP (ORDER BY store_id) AS c50
FROM orders
GROUP BY country_code
ORDER BY country_code NULLS FIRST
