SELECT count(*), sum((d.attrs->>'mixed')::float8) FROM docs d WHERE jsonb_typeof(d.attrs->'mixed') = 'number'
