SELECT e.kind, count(*), round(avg(e.score)::numeric, 6) FROM docs d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(kind text, sub text, flag text, score float8) GROUP BY 1 ORDER BY 1
