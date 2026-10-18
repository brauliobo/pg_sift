-- generic aggregates: the variance family, bit and boolean aggregates and min/max of text run PostgreSQL's own transition functions on the decoded values (numeric accumulation is exact, so the results are)
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT source, count(*) AS n, stddev_samp(total) AS sd, var_pop(total) AS vp, variance(discount) AS vd, stddev(store_id) AS sds,
       bool_and(total > 10) AS all_big, bool_or(status = 'refunded') AS any_refund, every(deleted_at IS NULL) AS all_live,
       bit_and(store_id) AS ba, bit_or(store_id) AS bo, min(status) AS st_lo, max(status) AS st_hi, min(lang) AS l_lo, max(country_code) AS cc_hi
FROM orders GROUP BY source ORDER BY source
