SELECT t, count(*) FROM docs d CROSS JOIN LATERAL jsonb_array_elements_text(d.attrs->'tags') AS t GROUP BY 1 ORDER BY 1
