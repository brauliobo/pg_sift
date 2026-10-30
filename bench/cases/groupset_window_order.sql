-- the daily breakdown of an entity by a jsonb dimension, grouping sets with and without the dimension, and the change against the previous day by a window: the groups come out of the scan in the order of the window
-- label: 400,000 events over 600 days, 6 categories and 37,100 dimensions (a jsonb array built from two columns), about 300,000 groups, a lag over the days of each category and dimension
-- ordered: yes
--@ setup
DO $do$ BEGIN
	IF to_regclass('gw_events') IS NULL THEN
		CREATE TABLE gw_events AS SELECT i AS id, date '2024-01-01' + i % 600 AS day, i % 6 AS cat, i % 7 AS kind, i % 5300 AS code, (i % 11)::float AS v
			FROM generate_series(1, 400000) i;
		ALTER TABLE gw_events ADD PRIMARY KEY (id);
		CREATE INDEX gw_events_col ON gw_events USING sift_col (id, day, cat, kind, code, v);
	END IF;
END $do$;
VACUUM (FREEZE, ANALYZE) gw_events
--@ pg
WITH grouped AS (SELECT cat, day, jsonb_build_array(jsonb_build_object('kind', kind), jsonb_build_object('code', code)) AS dim, avg(v) AS value, count(*) AS n FROM gw_events
                 GROUP BY GROUPING SETS ((cat, day, jsonb_build_array(jsonb_build_object('kind', kind), jsonb_build_object('code', code))), (cat, day))),
 w AS (SELECT grouped.*, lag(value) OVER (PARTITION BY dim, cat ORDER BY day) AS previous FROM grouped)
SELECT cat, day, dim, round(value::numeric, 6) AS value, n, round(previous::numeric, 6) AS previous FROM w WHERE day >= date '2025-01-01' ORDER BY cat, day, dim
