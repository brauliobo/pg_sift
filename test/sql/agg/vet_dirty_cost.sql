-- The rows on heap pages that are not all-visible are checked in the heap, and the units a declared key cuts (the index cuts by a column of few values by
-- itself) each span the whole heap, so a set of rows reaches the dirty pages of the whole table, not of its zone. The check ran 4 to 16 times longer than its
-- price when 80% of the pages or more were dirty: the cost of the scan must grow with the dirty pages as the time does (a month of a set: 7 times from
-- clean to all dirty). Below, the cost of the scan with every page dirty is 3 to 15 times that of the clean table, and it grows with the dirt.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int PRIMARY KEY, set_id int NOT NULL, kind_id int NOT NULL, ts date NOT NULL, metric_a float8 NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, 1 + g % 8, 1 + (g::bigint * 7919) % 12, date '2024-01-01' + (g * 37 % 700), g * 31 % 1000 FROM generate_series(1, 300000) g;
CREATE INDEX events_col ON events USING sift_col (set_id, kind_id, ts, metric_a);
VACUUM (FREEZE, ANALYZE) events;

CREATE FUNCTION scan_cost(q text) RETURNS float8 LANGUAGE sql AS $$ SELECT scan_cost FROM sift.explain_why(q) $$;
CREATE TABLE shapes (name text, q text);
INSERT INTO shapes VALUES
	('month of a set', $$SELECT kind_id, count(*), sum(metric_a) FROM events WHERE set_id = 3 AND ts >= '2025-03-01' AND ts < '2025-04-01' GROUP BY kind_id$$),
	('half year of a set', $$SELECT kind_id, count(*), sum(metric_a) FROM events WHERE set_id = 3 AND ts >= '2025-01-01' AND ts < '2025-07-01' GROUP BY kind_id$$),
	('month of all', $$SELECT kind_id, count(*), sum(metric_a) FROM events WHERE ts >= '2025-03-01' AND ts < '2025-04-01' GROUP BY kind_id$$);
CREATE TABLE clean AS SELECT name, scan_cost(q) AS cost FROM shapes;

-- the first row of every page of the first half, then of the second half: the update clears the page's bit of the visibility map
UPDATE events SET metric_a = metric_a WHERE (ctid::text::point)[0]::int % 100 < 50 AND (ctid::text::point)[1]::int = 1;
CREATE TABLE half AS SELECT name, scan_cost(q) AS cost FROM shapes;
UPDATE events SET metric_a = metric_a WHERE (ctid::text::point)[0]::int % 100 >= 50 AND (ctid::text::point)[1]::int = 1;
CREATE TABLE dirty AS SELECT name, scan_cost(q) AS cost FROM shapes;

SELECT name, h.cost > c.cost AND d.cost > h.cost AS grows, d.cost / c.cost BETWEEN 3 AND 15 AS dirty_in_range
FROM clean c JOIN half h USING (name) JOIN dirty d USING (name) ORDER BY name;

DROP TABLE clean, half, dirty, shapes;
DROP FUNCTION scan_cost(text);
DROP TABLE events;
DROP EXTENSION pg_sift;
