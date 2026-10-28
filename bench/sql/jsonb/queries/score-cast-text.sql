SELECT e.v->>'kind' AS kind, sum((e.v->>'score')::float8) FROM docs d CROSS JOIN LATERAL jsonb_array_elements(d.items) AS e(v) WHERE e.v->>'score' ~ '^[0-9.]+$' GROUP BY 1 ORDER BY 1
