-- an aggregate with no GROUP BY over a lateral function of the row (one row for each row of `x`) is read by the scan as the grouping of the elements by the key of `x`
-- label: `LEFT JOIN LATERAL (SELECT avg(score) FROM jsonb_to_recordset(doc['parts'])) s ON true`, `count`, `avg` by `kind`
-- ordered: yes
--@ pg
SELECT r.kind, count(r.id) AS n, avg(r.sv) AS sv FROM (SELECT x.id, x.kind, x.deleted_at, s.sv FROM events x
LEFT JOIN LATERAL (SELECT avg(o.score) AS sv FROM jsonb_to_recordset(x.doc['parts']) o(score float8) WHERE o.score IS NOT NULL) s ON true) r
WHERE r.deleted_at IS NULL GROUP BY r.kind ORDER BY 1
