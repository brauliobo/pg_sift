-- The choice between a shared CTE and its inline plan (cte.c) is kept for the statements of a backend under the statement without its constants, the size
-- class of each table it reads and the settings that move a plan, and a hit plans the chosen variant only. A stale verdict is the plan of the other variant,
-- never other rows. The counters are sift.stats().
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE kinds (id int PRIMARY KEY, enabled bool NOT NULL);
INSERT INTO kinds SELECT g, g <= 20 FROM generate_series(1, 3000) g;
CREATE TABLE events (id int PRIMARY KEY, set_id int NOT NULL, kind_id int NOT NULL, ts date NOT NULL, doc jsonb);
CREATE FUNCTION fill(from_id int, to_id int) RETURNS void LANGUAGE sql AS $$
	INSERT INTO events
	SELECT g, 1 + g % 30, 1 + g % 20, date '2025-01-01' + (g * 7919 % 360),
	       jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'c' || (g + j) % 4, 'score', (g * j) % 10)) FROM generate_series(1, 1 + g % 3) j))
	FROM generate_series(from_id, to_id) g $$;
SELECT fill(1, 1500);
CREATE INDEX events_col ON events USING sift_col (id, set_id, kind_id, ts, doc) WITH (paths = 'doc.parts[]{cat astext, score float8}');
VACUUM (FREEZE, ANALYZE) events;
ANALYZE kinds;

-- a shared CTE read by three aggregates; owners is the constant that changes between the statements of a dashboard
CREATE FUNCTION series_query(key_sets int) RETURNS text LANGUAGE sql AS $f$
	SELECT format($q$WITH r AS (SELECT e.* FROM events e INNER JOIN kinds k ON k.id = e.kind_id AND k.enabled IS TRUE WHERE e.set_id <= %s),
	a AS (SELECT r.ts, o.cat, avg(o.score) AS s, count(DISTINCT r.id) AS n FROM r, jsonb_to_recordset(r.doc['parts']) AS o(cat text, score float) GROUP BY r.ts, o.cat),
	b AS (SELECT r.ts, count(DISTINCT r.id) AS n, avg(o.score) AS s FROM r, jsonb_to_recordset(r.doc['parts']) AS o(cat text, score float) GROUP BY r.ts),
	c AS (SELECT r.ts, r.set_id, count(DISTINCT r.id) AS n FROM r GROUP BY r.ts, r.set_id)
SELECT 'a' AS src, ts, cat, s, n FROM a UNION ALL SELECT 'b', ts, NULL, s, n FROM b UNION ALL SELECT 'c', ts, set_id::text, NULL, n FROM c$q$, key_sets) $f$;

CREATE FUNCTION plan_it(q text) RETURNS void LANGUAGE plpgsql AS $$ BEGIN EXECUTE 'EXPLAIN ' || q; END $$;
CREATE FUNCTION arms(q text) RETURNS text LANGUAGE sql AS
$$ SELECT string_agg(arm || ':' || used, ' ' ORDER BY arm) FROM sift.explain_why(q) WHERE arm IN ('cte_inline', 'verdict_cached') $$;
CREATE FUNCTION counts() RETURNS text LANGUAGE sql AS
$$ SELECT string_agg(replace(name, 'verdict_cache_', '') || '=' || value, ' ' ORDER BY name) FROM sift.stats() WHERE name LIKE 'verdict_cache_%' AND name <> 'verdict_cache_shared_hits' $$;
CREATE FUNCTION shared_hits() RETURNS bigint LANGUAGE sql AS
$$ SELECT value FROM sift.stats() WHERE name = 'verdict_cache_shared_hits' $$;

-- the first statement is planned in full, the next with other constants from its verdict while its request is of about the same size (agg_cte_verdict_sizes
-- has the others)
SELECT sift.stats_reset();
SELECT plan_it(series_query(10));
SELECT plan_it(series_query(11));
SELECT plan_it(series_query(12));
SELECT counts();
-- explain_why says so: the verdict of the table at this size is the shared CTE
SELECT arms(series_query(15));

-- the table grows past its size class: planned in full, and the scans take the CTE now
SELECT sift.stats_reset();
SELECT fill(1501, 20000);
SELECT plan_it(series_query(10));
SELECT counts();
SELECT arms(series_query(10));
SELECT arms(series_query(10));
SELECT counts();

-- ANALYZE and a change of a setting that moves a plan drop the verdicts
SELECT sift.stats_reset();
ANALYZE events;
SELECT plan_it(series_query(10));
SET enable_hashagg = off;
SELECT plan_it(series_query(10));
RESET enable_hashagg;
SELECT plan_it(series_query(10));
SELECT counts();

-- an entry is checked again every eighth hit, and each check that confirms it doubles the hits to the next (a new key: other work_mem): plans in full at the
-- 1st, 9th and 25th statement of the 41
SET work_mem = '65MB';
SELECT sift.stats_reset();
SELECT count(plan_it(series_query(10 + i % 3))) FROM generate_series(0, 40) i;
SELECT counts();
RESET work_mem;

-- the choice between the flattened form of a statement (a lateral aggregate as a table grouped by its tie) and the form as written is kept the same way: the
-- second statement is planned in the form that won, and gives the rows of the plan without the scans
CREATE FUNCTION lateral_query(key_sets int) RETURNS text LANGUAGE sql AS
$$ SELECT format($q$WITH r AS (SELECT * FROM events WHERE set_id <= %s)
SELECT k.id, x.s, y.n FROM kinds k
	LEFT JOIN LATERAL (SELECT sum(r.set_id) AS s FROM r WHERE r.kind_id = k.id) x ON true
	LEFT JOIN LATERAL (SELECT count(*) AS n FROM r WHERE r.kind_id = k.id) y ON true$q$, key_sets) $$;
SELECT sift.stats_reset();
SELECT plan_it(lateral_query(10));
SELECT plan_it(lateral_query(11));
SELECT counts();
DROP FUNCTION lateral_query;

-- the verdicts are kept for the other backends of the database too (sift.verdict_cache_kb): a backend that connects after the first one plans the statement
-- from its verdict (a shared hit), and the key carries the files of the column index, so that a REINDEX is not served the verdict of the old one
SELECT plan_it(series_query(10));
\c
SET max_parallel_workers_per_gather = 0;
SELECT sift.stats_reset();
SELECT plan_it(series_query(10));
SELECT counts(), shared_hits();
REINDEX INDEX events_col;
\c
SET max_parallel_workers_per_gather = 0;
SELECT sift.stats_reset();
SELECT plan_it(series_query(10));
SELECT counts(), shared_hits();
\c
SET max_parallel_workers_per_gather = 0;
SELECT sift.stats_reset();
SELECT plan_it(series_query(10));
SELECT counts(), shared_hits();

-- the table is a cache: where a backend cannot have it (no room in /dev/shm, no more segments) it plans in full and no statement
-- fails; the setting at 0 is the clean switch, for the backends that set it
\c
SET max_parallel_workers_per_gather = 0;
SET sift.test_verdict_shm_fail = on;
SELECT sift.stats_reset();
SELECT plan_it(series_query(10));
SELECT plan_it(series_query(10));
SELECT counts(), shared_hits();
\c
SET max_parallel_workers_per_gather = 0;
SET sift.verdict_cache_kb = 0;
SELECT sift.stats_reset();
SELECT plan_it(series_query(10));
SELECT counts(), shared_hits();

-- the statement of another constant planned from the verdict cached for this one gives the rows of the plan without the scans
CREATE FUNCTION digest(q text, agg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE d text;
BEGIN
	PERFORM set_config('sift.enable_agg', agg::text, true);
	EXECUTE 'SELECT md5(string_agg(r, ''|'' ORDER BY r)) FROM (SELECT concat_ws('','', src, ts, cat, round(s::numeric, 6), n) AS r FROM (' || q || ') t) u' INTO d;
	RETURN d;
END $$;
SELECT sift.stats_reset();
SELECT plan_it(series_query(10));
SELECT digest(series_query(25), true) = digest(series_query(25), false) AS same_rows;
SELECT counts();
DROP FUNCTION digest;
DROP FUNCTION counts, shared_hits, arms, plan_it, series_query, fill;
DROP TABLE events, kinds;
DROP EXTENSION pg_sift;
