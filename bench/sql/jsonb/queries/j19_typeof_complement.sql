SELECT count(*) FROM docs d WHERE jsonb_typeof(d.attrs->'mixed') <> 'number'
