-- a bound that a stable function gives (zero days added to the date): PostgreSQL prunes when the statement starts, so does the scan
-- shape: part
-- expect: agg
-- key: -
SELECT tag, count(*) AS n, sum(amount) AS revenue
FROM t_part WHERE d >= DATE '2024-05-10' + (current_date - current_date) AND d < DATE '2024-08-20' + (current_date - current_date) GROUP BY tag ORDER BY tag
