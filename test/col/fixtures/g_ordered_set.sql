-- generic ordered-set aggregates: the array form of the fractions, an interval, text and date arguments and mode(); a group's values go through one tuplesort, then PostgreSQL's own functions
-- shape: G7
-- expect: agg
-- key: -
-- scenario: yes
SELECT store_id % 5 AS g, count(*) AS n, percentile_cont(ARRAY[0.1, 0.5, 0.9]) WITHIN GROUP (ORDER BY total::float8) AS arr,
       percentile_cont(0.5) WITHIN GROUP (ORDER BY (placed_at - DATE '2022-01-01') * INTERVAL '1 hour') AS med_iv,
       percentile_disc(0.5) WITHIN GROUP (ORDER BY source) AS med_src, percentile_disc(ARRAY[0, 0.5, 1]::float8[]) WITHIN GROUP (ORDER BY placed_at) AS days,
       mode() WITHIN GROUP (ORDER BY country_code) AS mode_cc, mode() WITHIN GROUP (ORDER BY lang) FILTER (WHERE status = 'paid') AS mode_paid_lang,
       percentile_disc(0.9) WITHIN GROUP (ORDER BY total DESC) AS p90_total
FROM orders GROUP BY store_id % 5 ORDER BY 1
