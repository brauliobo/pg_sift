SELECT s.key, count(*), avg(s.value::float8) FROM docs d CROSS JOIN LATERAL jsonb_each_text(d.scores) AS s(key, value) WHERE s.value ~ '^[0-9.]+$' GROUP BY 1 ORDER BY 1
