SELECT count(*) FROM docs d WHERE EXISTS (SELECT 1 FROM jsonb_array_elements(d.items) e WHERE e->>'kind' = 'c3' AND e->>'flag' = 'fa')
