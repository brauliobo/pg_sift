SELECT jsonb_array_length(d.items) AS n, count(*), avg(d.grp) FROM docs d GROUP BY 1 ORDER BY 1
