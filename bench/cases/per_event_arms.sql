-- a grouping that regroups per-event groups (the average of each event's scores, then by category and sub-category): the cells of the arms
-- label: 400,000 events of 0 to 5 elements of a jsonb list, `avg` of each event's element scores by (event, category, sub), then `avg`, `sum` and `count(DISTINCT event)` by (category, sub)
-- ordered: yes
--@ setup
DO $do$ BEGIN
	IF to_regclass('pe_events') IS NULL THEN
		CREATE TABLE pe_events AS
		SELECT i AS id, i % 200 AS dim_a_id, date '2024-01-01' + i % 600 AS day,
		       (SELECT jsonb_agg(jsonb_build_object('category', 'c' || (hashint4(i * 8 + j + 21) & 2147483647) % 12, 'sub', 's' || (hashint4(i * 8 + j + 23) & 2147483647) % 6,
		                                            'score', ((hashint4(i * 8 + j + 31) & 2147483647) % 100) / 10.0))
		          FROM generate_series(1, (hashint4(i + 37) & 2147483647) % 6) j) AS parts
		FROM generate_series(1, 400000) i;
		ALTER TABLE pe_events ADD PRIMARY KEY (id);
		CREATE INDEX pe_events_col ON pe_events USING sift_col (id, dim_a_id, day, parts) WITH (key_cut = 'dim_a_id, day', paths = 'parts[]{category text, sub text, score float8}');
	END IF;
END $do$;
VACUUM (FREEZE, ANALYZE) pe_events
--@ pg
WITH rows AS (SELECT e.id, p.category, p.sub, p.score FROM pe_events e CROSS JOIN LATERAL jsonb_to_recordset(e.parts) p(category text, sub text, score float8)
              WHERE e.dim_a_id < 100 AND e.day >= date '2024-01-01' AND e.day < date '2025-01-01' AND p.score IS NOT NULL),
     per_event AS (SELECT id, category, sub, avg(score) AS score, count(*) AS n FROM rows GROUP BY id, category, sub)
SELECT category, sub, round(avg(score)::numeric, 6) AS value, sum(n) AS elements, count(DISTINCT id) AS events FROM per_event GROUP BY category, sub ORDER BY category, sub
