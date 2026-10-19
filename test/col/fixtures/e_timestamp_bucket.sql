-- calendar keys of a timestamp (a nullable column): each is evaluated once per day, hour or minute of the data, with NULLs, aggregates and a distinct count next to it
-- shape: expr
-- expect: agg
-- key: -
-- scenario: yes
SELECT date_trunc('day', deleted_at) AS d, date_trunc('hour', deleted_at)::date AS h, extract(dow FROM deleted_at) AS w, date_trunc('minute', deleted_at) AS m,
	count(*) AS n, sum(total) AS s, count(DISTINCT tenant_id) AS c
FROM orders GROUP BY 1, 2, 3, 4 ORDER BY 1 NULLS FIRST, 2 NULLS FIRST, 3 NULLS FIRST, 4 NULLS FIRST
