SELECT count(*) FROM docs d WHERE jsonb_typeof(d.attrs->'mixed') IS NULL OR jsonb_typeof(d.attrs->'mixed') = 'string'
