SELECT count(*) FILTER (WHERE jsonb_typeof(d.attrs->'mixed') = 'string' OR d.grp < 10) AS a, count(*) FILTER (WHERE jsonb_typeof(d.attrs->'mixed') = 'null') AS b FROM docs d
