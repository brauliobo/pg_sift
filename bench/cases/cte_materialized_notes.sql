-- the notes of events (jsonb_to_recordset, mapped through a table), written once as a MATERIALIZED CTE and read by three aggregates, two periods against each other: the CTE is planned inline and the aggregates are scans
-- label: 500,000 events of 200 entities, two notes each, 100 entities picked by a CTE of ids, two months against two months
-- ordered: yes
--@ setup
DO $do$ BEGIN
	IF to_regclass('nm_events') IS NULL THEN
		CREATE TABLE nm_events AS SELECT i AS id, i % 200 AS ent, date '2024-01-01' + i % 900 AS day,
			CASE WHEN i % 13 = 0 THEN NULL ELSE jsonb_build_object('notes', jsonb_build_array(
				jsonb_build_object('category', 'c' || i % 4, 'value_num', 's' || i % 3, 'tone', CASE i % 3 WHEN 0 THEN 'up' WHEN 1 THEN 'down' ELSE 'flat' END, 'score', i % 7),
				jsonb_build_object('category', 'c' || (i + 1) % 4, 'value_num', 's' || i % 2, 'tone', 'flat', 'score', i % 5))) END AS ann
			FROM generate_series(1, 500000) i;
		ALTER TABLE nm_events ADD PRIMARY KEY (id);
		CREATE INDEX nm_events_col ON nm_events USING sift_col (id, ent, day, ann) WITH (paths = 'ann.notes[]{category astext, value_num astext, tone astext, score float8}');
		CREATE TABLE nm_maps AS SELECT 'c' || a AS category, 's' || b AS value_num, 'm' || (a + b) % 3 AS flag_a FROM generate_series(0, 3) a, generate_series(0, 2) b;
		ALTER TABLE nm_maps ADD PRIMARY KEY (category, value_num);
	END IF;
END $do$;
VACUUM (FREEZE, ANALYZE) nm_events, nm_maps
--@ pg
WITH ids AS MATERIALIZED (SELECT DISTINCT ent FROM nm_events WHERE ent < 100),
 periods(period, lo, hi) AS (VALUES ('current', DATE '2026-02-01', DATE '2026-03-31'), ('previous', DATE '2025-02-01', DATE '2025-03-31')),
 notes AS MATERIALIZED (SELECT e.id, e.ent, p.period, m.flag_a AS category, n.value_num, n.tone, n.score
             FROM nm_events e JOIN ids USING (ent) JOIN periods p ON e.day >= p.lo AND e.day < p.hi
             CROSS JOIN LATERAL jsonb_to_recordset(e.ann -> 'notes') AS n(category text, value_num text, tone text, score float8)
             LEFT JOIN LATERAL (SELECT flag_a FROM nm_maps WHERE nm_maps.category = n.category AND nm_maps.value_num = n.value_num LIMIT 1) m ON true),
 per_event AS (SELECT id, ent, period, category, value_num, avg(score) AS score, count(*) FILTER (WHERE tone = 'up') AS pos, count(*) AS notes FROM notes GROUP BY id, ent, period, category, value_num
               UNION ALL SELECT id, ent, period, category, NULL, avg(score), count(*) FILTER (WHERE tone = 'up'), count(*) FROM notes GROUP BY id, ent, period, category
               UNION ALL SELECT id, ent, period, NULL, NULL, avg(score), count(*) FILTER (WHERE tone = 'up'), count(*) FROM notes GROUP BY id, ent, period)
SELECT category, value_num, dperiod, count(id) AS events, round(avg(score)::numeric, 6) AS score, sum(pos) AS pos, sum(notes) AS notes, count(DISTINCT ent) AS ents
FROM (SELECT d.period AS dperiod, e.id, e.ent, e.category, e.value_num, e.score, e.pos, e.notes FROM periods d LEFT JOIN per_event e ON e.period = d.period) x
GROUP BY GROUPING SETS ((category, value_num, dperiod)) ORDER BY 1, 2, 3
