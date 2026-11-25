-- the values of a group do not fit the histogram of a 64kB work_mem: they go through a sort, with the same result
-- shape: whole-percentiles-by-category
-- expect: agg
-- key: -
-- guc: work_mem = 64kB
SELECT i.category, percentile_cont(0.5) WITHIN GROUP (ORDER BY i.price + i.discount * 0.37) AS p50, percentile_cont(0.9) WITHIN GROUP (ORDER BY i.price + i.discount * 0.37) AS p90,
       percentile_disc(0.2) WITHIN GROUP (ORDER BY i.price + i.discount * 0.37 DESC) AS q20
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8, discount float8)
GROUP BY i.category
ORDER BY i.category
