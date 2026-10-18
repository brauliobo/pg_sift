-- generic aggregates with a combine function run in the workers and merge their partial states in the leader (internal states through their serial functions)
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT source, bool_and(total > 10) AS ba, bit_or(tenant_id) AS bo, bit_xor(id) AS bx, stddev_samp(total) AS sd, var_pop(tenant_id) AS vp, regr_count(total::float8, tenant_id::float8) AS rc,
       round(corr(total::float8, tenant_id::float8)::numeric, 9) AS co, coldiff.sum_sq_par(total::float8) AS sq, every(status IS NOT NULL) AS ev, count(*) AS n,
       cardinality(array_agg(tenant_id)) AS ids, length(string_agg(country_code, ',')) AS ccs
FROM orders GROUP BY source ORDER BY source
