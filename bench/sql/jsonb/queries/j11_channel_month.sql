SELECT d.attrs->>'channel' AS ch, date_trunc('month', d.day) AS m, count(*) FROM docs d GROUP BY 1, 2 ORDER BY 1, 2
