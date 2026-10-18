-- a percentile fraction that is a stable expression (evaluated when the scan starts); a per-group column-valued one stays on the normal plan (o_percentile_array)
-- shape: G7
-- expect: agg
-- key: -
SELECT source, percentile_cont(least(1.0, extract(month FROM now())::float8 / 12.0)) WITHIN GROUP (ORDER BY total::float8) AS p,
       percentile_disc(ARRAY[0.25, least(1.0, extract(month FROM now())::float8 / 12.0)]) WITHIN GROUP (ORDER BY placed_at) AS d
FROM orders GROUP BY source ORDER BY source
