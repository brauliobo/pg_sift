-- a common table expression that three aggregates read, built from a table with a column index joined to a filtered dimension and to a list of ranges by containment: the shared plan is made without Memoize and its scan is priced from the rows the body can build, so the scans that replace it win the comparison
-- label: `WITH r AS (events JOIN owners JOIN dims JOIN periods)` read by three aggregates over `jsonb_to_recordset(doc['parts'])`, `count(DISTINCT id)`
-- ordered: yes
--@ pg
WITH key_sets AS MATERIALIZED (SELECT DISTINCT parent_id FROM events WHERE parent_id IN (SELECT 1 + g FROM generate_series(0, 299) g)),
	periods AS (SELECT * FROM (VALUES ('cur', daterange(DATE '2025-01-01', DATE '2025-12-31', '[]')), ('prev', daterange(DATE '2024-01-01', DATE '2024-12-31', '[]'))) p(period, range)),
	r AS (SELECT e.*, periods.period FROM events e INNER JOIN key_sets o ON o.parent_id = e.parent_id INNER JOIN dims k ON k.id = e.parent_id AND k.active IS TRUE
	      INNER JOIN periods ON e.ts <@ periods.range),
	a AS (SELECT r.period, o.kind, avg(o.score) AS s, count(DISTINCT r.id) AS n FROM r, jsonb_to_recordset(r.doc['parts']) AS o(kind text, score float) GROUP BY r.period, o.kind),
	b AS (SELECT r.period, count(DISTINCT r.id) AS n, avg(o.score) AS s FROM r, jsonb_to_recordset(r.doc['parts']) AS o(kind text, score float) GROUP BY r.period),
	c AS (SELECT r.period, r.parent_id, count(DISTINCT r.id) AS n FROM r GROUP BY r.period, r.parent_id)
SELECT 'a' AS src, period, kind, round(s::numeric, 6) AS s, n FROM a UNION ALL SELECT 'b', period, NULL, round(s::numeric, 6), n FROM b UNION ALL SELECT 'c', period, parent_id::text, NULL, n FROM c
ORDER BY 1, 2, 3, 5
