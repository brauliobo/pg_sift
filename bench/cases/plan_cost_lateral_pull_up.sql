-- a page of events with lateral sub-selects that can be pulled up into the join (the elements of a document, a looked-up row) and no aggregate: the hook plans the statement as written and pulled up, and the time is that planning
-- label: `INNER JOIN LATERAL (jsonb_each .. LEFT JOIN parents)`, a lateral last fact, `IN (40 literals)`, `ORDER BY id LIMIT 20`, no aggregate
-- ordered: yes
--@ pg
SELECT e.id, s.key, s.name, f.role, f.val FROM events e INNER JOIN LATERAL (SELECT r.key, pa.name FROM jsonb_each(e.doc -> 'metrics') r LEFT JOIN parents pa ON pa.id = e.parent_id) s ON true LEFT JOIN LATERAL (SELECT fa.role, fa.val FROM facts fa WHERE fa.parent_id = e.parent_id ORDER BY fa.ts DESC, fa.id LIMIT 1) f ON true WHERE e.parent_id IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40) AND e.ts BETWEEN '2030-01-01' AND '2030-12-31' AND e.deleted_at IS NULL ORDER BY e.id LIMIT 20
