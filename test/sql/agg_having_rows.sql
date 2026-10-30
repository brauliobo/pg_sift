-- The rows a SiftAgg node reports are PostgreSQL's estimate of the groups that pass the HAVING clause, not all the groups: the scan applies the clause itself, and
-- a plan above that joins the groups (an anti join against the keys with the most rows, NOT IN) is counted from them. Reported as all 50 groups, PostgreSQL 19
-- counted that anti join as one row, and a shared CTE plan was priced for it (agg_cte_hoist).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int PRIMARY KEY, parent_id int NOT NULL, score float8 NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, 1 + g % 50, g % 17 FROM generate_series(1, 30000) g;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, score);
VACUUM (FREEZE, ANALYZE) events;

CREATE FUNCTION top_rows(q text, scan bool) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	EXECUTE format('SET sift.enable_agg = %s', CASE WHEN scan THEN 'on' ELSE 'off' END);
	FOR l IN EXECUTE 'EXPLAIN ' || q LOOP
		RETURN substring(l from ' rows=(\d+) ')::bigint;
	END LOOP;
END $$;
CREATE FUNCTION uses_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	SET sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE TABLE having_cases (q text);
INSERT INTO having_cases VALUES
	('SELECT parent_id FROM events GROUP BY parent_id HAVING min(id) < 25'),
	('SELECT parent_id, sum(score) FROM events GROUP BY parent_id HAVING sum(score) > 4000'),
	('SELECT parent_id, count(*) FROM events GROUP BY parent_id HAVING count(*) > 100 AND max(id) < 20000'),
	('SELECT parent_id FROM events GROUP BY parent_id');
-- the scan is used and reports the rows PostgreSQL's own aggregate does
SELECT uses_scan(q) AS scan, top_rows(q, true) = top_rows(q, false) AS same_rows FROM having_cases;
DROP FUNCTION top_rows(text, bool), uses_scan(text);
DROP TABLE having_cases, events;
DROP EXTENSION pg_sift;
