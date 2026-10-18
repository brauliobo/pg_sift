-- trap: avg(float8) keeps a sum of squares: the metrics of 1e300 overflow it (22003) in every plan, although their sum does not
-- shape: trap
-- expect: any
-- error: yes
-- key: -
SELECT avg((metrics->>'huge')::float8) AS a FROM orders_odd WHERE metrics ? 'huge'
