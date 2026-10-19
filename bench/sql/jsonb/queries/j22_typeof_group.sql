SELECT jsonb_typeof(d.attrs->'mixed') AS ty, count(*), sum(d.grp) FROM docs d GROUP BY 1
