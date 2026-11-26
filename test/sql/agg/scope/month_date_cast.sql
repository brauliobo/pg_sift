-- date_trunc('month', ...) of a date time leaf groups by the months of the summary only when the date reaches date_trunc as PostgreSQL casts it to a timestamp or a timestamptz: another
-- function of one date argument (here one that moves every date by 40 days) makes other months, so the statement runs the join and gives the rows of the heap.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET TimeZone = 'UTC';
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
CREATE TABLE md_pr (entity_id int NOT NULL, parent_id int NOT NULL, PRIMARY KEY (entity_id, parent_id)) WITH (autovacuum_enabled = false);
INSERT INTO md_pr SELECT e, 1 + e % 5 FROM generate_series(1, 40) e;
CREATE TABLE md_ev (id int PRIMARY KEY, entity_id int NOT NULL, at date NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO md_ev SELECT g, 1 + g % 40, DATE '2025-01-01' + g / 20, g % 97 FROM generate_series(1, 6000) g;
CREATE INDEX md_scope ON md_ev USING sift_col (id, entity_id, at, amount) WITH (scope = 'parent_id: entity_id -> md_pr(entity_id, parent_id)', track_changes = 'ctid');
VACUUM (ANALYZE) md_ev, md_pr;
CREATE FUNCTION md_later(d date) RETURNS timestamp LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RETURN d + 40; END $$;

CREATE FUNCTION md_check(q text, OUT summary bool, OUT same bool) LANGUAGE plpgsql AS $$
DECLARE l text; a text; b text;
BEGIN
	summary := false;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		summary := summary OR l ~ 'Scope Read: summary';
	END LOOP;
	EXECUTE 'SELECT string_agg(x::text, ''|'' ORDER BY x::text) FROM (' || q || ') x' INTO a;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE 'SELECT string_agg(x::text, ''|'' ORDER BY x::text) FROM (' || q || ') x' INTO b;
	PERFORM set_config('sift.enable_agg', 'on', true);
	same := a IS NOT DISTINCT FROM b;
END $$;

SELECT f AS month_of, (md_check(format('SELECT date_trunc(''month'', %s), sum(e.amount) FROM md_ev e JOIN md_pr p ON p.entity_id = e.entity_id WHERE p.parent_id = 2 GROUP BY 1', f))).*
FROM unnest(ARRAY['e.at::timestamp', 'e.at::timestamptz', 'md_later(e.at)']) f ORDER BY f;

DROP TABLE md_ev, md_pr;
DROP FUNCTION md_check(text), md_later(date);
DROP EXTENSION pg_sift;
