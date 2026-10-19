SELECT count(*), sum(d.grp) FROM docs d WHERE jsonb_typeof(d.attrs->'mixed') = 'null'
