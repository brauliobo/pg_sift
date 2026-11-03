-- the two arms of a UNION ALL (the groups of a table and their totals) read by a window: each arm emits its groups in the window's order and the parent merges them, no Sort
-- label: 400,000 events over 600 days, 20 categories and 6 kinds, `count(*), avg(v)` by (category, kind, day) and by (category, day), `lag` over the days of each category and kind
-- ordered: yes
--@ setup
DO $do$ BEGIN
	IF to_regclass('ua_events') IS NULL THEN
		CREATE TABLE ua_events AS SELECT i AS id, date '2024-01-01' + i % 600 AS day, i % 20 AS cat, i % 6 AS kind, (i % 11)::float AS v FROM generate_series(1, 400000) i;
		ALTER TABLE ua_events ADD PRIMARY KEY (id);
		CREATE INDEX ua_events_col ON ua_events USING sift_col (id, day, cat, kind, v);
	END IF;
END $do$;
VACUUM (FREEZE, ANALYZE) ua_events
--@ pg
WITH g AS (SELECT cat, kind, day, avg(v) AS value, count(*) AS n FROM ua_events GROUP BY cat, kind, day
           UNION ALL SELECT cat, NULL::int, day, avg(v), count(*) FROM ua_events GROUP BY cat, day)
SELECT cat, kind, day, round(value::numeric, 6) AS value, n, round(lag(value) OVER (PARTITION BY cat, kind ORDER BY day)::numeric, 6) AS previous FROM g ORDER BY cat, kind, day
