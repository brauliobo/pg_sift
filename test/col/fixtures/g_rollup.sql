-- generic aggregates under grouping sets: each set has its groups' states, the values of an ordered-set aggregate go through the one sort by group
-- shape: S3
-- expect: agg
-- key: -
SELECT lang, status, GROUPING(lang, status) AS gr, count(*) AS n, stddev_pop(total) AS sd, mode() WITHIN GROUP (ORDER BY country_code) AS mode_cc, bit_or(store_id) AS bo
FROM orders GROUP BY ROLLUP (lang, status)
ORDER BY lang NULLS LAST, status NULLS LAST, gr
