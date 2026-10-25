-- A CTE of the keys a request names (a literal list, kept to the keys a sub-select allows) is estimated at the rows of two selectivities multiplied: a
-- hundred keys joined to a hundred come out as one, and the plan that keeps the CTE is priced for one key whatever the keys own. The CTE is estimated at
-- the keys its list is worth for the table with a column index it is joined to (the statistics of the key column), never fewer rows than PostgreSQL's own.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE sites (id int PRIMARY KEY);
INSERT INTO sites SELECT g FROM generate_series(1, 2000) g;
CREATE TABLE grants (site_id int NOT NULL);
INSERT INTO grants SELECT g FROM generate_series(1, 2000) g;
CREATE TABLE events (id int PRIMARY KEY, site_id int NOT NULL REFERENCES sites, ts date NOT NULL);
-- site 1 owns 30% of the events, site 2 one in a thousand, the others are spread evenly
INSERT INTO events SELECT g, CASE WHEN g % 100 < 30 THEN 1 WHEN g % 1000 = 999 THEN 2 ELSE 3 + g % 1997 END, date '2025-01-01' + g % 360 FROM generate_series(1, 20000) g;
CREATE INDEX events_col ON events USING sift_col (id, site_id, ts);
VACUUM (FREEZE, ANALYZE) events;
ANALYZE sites, grants;
-- the rows the plan expects of the CTE of a list of site ids that two sub-selects read
CREATE FUNCTION key_rows(ids text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN WITH keys AS MATERIALIZED (SELECT DISTINCT id FROM sites WHERE id IN (' || ids || ') AND id IN (SELECT site_id FROM grants))
		SELECT (SELECT count(*) FROM events e JOIN keys k ON k.id = e.site_id), (SELECT max(ts) FROM events e JOIN keys k ON k.id = e.site_id)' LOOP
		IF l ~ 'CTE Scan on keys' THEN n := substring(l from 'rows=(\d+)')::int; END IF;
	END LOOP;
	RETURN n;
END $$;
-- the key with a third of the events is worth hundreds of keys, a key of the mean is one, a hundred keys of the mean are a hundred
SELECT key_rows('1') > 100 AS hot, key_rows('1500') AS cold, key_rows((SELECT string_agg(g::text, ',') FROM generate_series(1000, 1099) g)) AS hundred;
DROP FUNCTION key_rows;
DROP TABLE events, grants, sites;
DROP EXTENSION pg_sift;
