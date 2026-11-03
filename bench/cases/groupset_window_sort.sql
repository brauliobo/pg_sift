-- the rows of one day of an entity's groups with the change against the previous day by a window, ordered by a flag of one category first: the scan emits the order of the Sort above the window
-- label: 400,000 events over 600 days, 7 kinds and 5,300 codes, about 400,000 groups, a lag over the days of each kind and code, the rows of one day ordered by (kind = 3) DESC, code, kind
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
WITH grouped AS (SELECT kind, code, day, avg(v) AS value, count(*) AS n FROM gw_events GROUP BY kind, code, day),
 w AS (SELECT grouped.*, lag(value) OVER (PARTITION BY kind, code ORDER BY day DESC) AS previous FROM grouped)
SELECT kind, code, day, round(value::numeric, 6) AS value, n, round(previous::numeric, 6) AS previous FROM w WHERE day = date '2025-01-01' ORDER BY (kind = 3) DESC, code, kind
