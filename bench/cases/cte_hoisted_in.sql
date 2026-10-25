-- a common table expression that two aggregates read is planned inline, and the aggregating `IN` sub-select of its body (the keys with the most rows) is a common table expression of the same WITH, computed once, not once for each copy of the body
-- label: `part_rows` read by two arms, `parent_id IN (SELECT parent_id FROM events GROUP BY parent_id HAVING min(id) < 25)`, over `jsonb_to_recordset(doc['parts'])`
-- ordered: yes
--@ pg
WITH periods AS (SELECT * FROM (VALUES ('cur', daterange(DATE '2025-03-01', DATE '2025-12-31', '[]')), ('prev', daterange(DATE '2024-03-01', DATE '2024-12-31', '[]'))) p(period, range)),
	part_rows AS (SELECT e.id, periods.period AS bucket_period, o.kind, o.sub, o.score
	              FROM events e JOIN periods ON e.ts <@ periods.range
	              CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(kind text, sub text, score float8)
	              WHERE o.score IS NOT NULL AND e.parent_id IN (SELECT parent_id FROM events GROUP BY parent_id HAVING min(id) < 250)),
	per_cat AS (
	  SELECT bucket_period, kind, sub, avg(score) AS score, count(*) AS n FROM part_rows GROUP BY bucket_period, kind, sub
	  UNION ALL
	  SELECT bucket_period, NULL, NULL, avg(score), count(*) FROM part_rows GROUP BY bucket_period)
SELECT p.period, coalesce(x.kind, 'mhead') AS kind, x.sub, round(x.score::numeric, 6) AS value, x.n
FROM periods p LEFT JOIN per_cat x ON x.bucket_period = p.period ORDER BY 1, 2, 3
