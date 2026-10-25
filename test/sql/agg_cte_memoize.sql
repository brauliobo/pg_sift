-- PostgreSQL prices a Memoize node over the lateral lookups of a CTE's rows on the hits it expects, and for a key that is almost unique per row (a jsonb
-- document) it expects them in vain: the shared plan was compared to the scans at a price below what it costs. The shared plan of a CTE that several
-- aggregates read is made without Memoize, and the setting is what it was after, whatever happens while it is planned.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE kinds (id int PRIMARY KEY, enabled bool NOT NULL);
INSERT INTO kinds SELECT g, g <= 20 FROM generate_series(1, 3000) g;
CREATE TABLE events (id int PRIMARY KEY, set_id int NOT NULL, kind_id int NOT NULL, ts date NOT NULL, doc jsonb);
INSERT INTO events
SELECT g, 1 + g % 30, 1 + g % 20, date '2025-01-01' + (g * 7919 % 360),
       jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'c' || (g + j) % 4, 'score', (g * j) % 10)) FROM generate_series(1, 1 + g % 3) j))
FROM generate_series(1, 500) g;
CREATE INDEX events_col ON events USING sift_col (id, set_id, kind_id, ts, doc) WITH (paths = 'doc.parts[]{cat astext, score float8}');
VACUUM (FREEZE, ANALYZE) events;
ANALYZE kinds;

CREATE FUNCTION series_query(extra text DEFAULT '') RETURNS text LANGUAGE sql AS $f$
	SELECT $q$WITH key_sets AS MATERIALIZED (SELECT DISTINCT set_id FROM events WHERE set_id IN (SELECT 1 + g FROM generate_series(0, 29) g)),
	periods AS (SELECT * FROM (VALUES ('cur', daterange(DATE '2025-06-01', DATE '2025-12-31', '[]')), ('prev', daterange(DATE '2025-01-01', DATE '2025-05-31', '[]'))) p(period, range)),
	r AS (SELECT e.*, periods.period FROM events e INNER JOIN key_sets o ON o.set_id = e.set_id INNER JOIN kinds k ON k.id = e.kind_id AND k.enabled IS TRUE
	      INNER JOIN periods ON e.ts <@ periods.range),
	a AS (SELECT r.period, o.cat, avg(o.score) AS s, count(DISTINCT r.id) AS n FROM r, jsonb_to_recordset(r.doc['parts']) AS o(cat text, score float) GROUP BY r.period, o.cat),
	b AS (SELECT r.period, count(DISTINCT r.id) AS n, avg(o.score) AS s FROM r, jsonb_to_recordset(r.doc['parts']) AS o(cat text, score float) GROUP BY r.period),
	c AS (SELECT r.period, r.set_id, count(DISTINCT r.id) AS n, max(m.mx) AS mx FROM r
	      LEFT JOIN LATERAL (SELECT max(x.score) AS mx FROM jsonb_to_recordset(r.doc['parts']) AS x(cat text, score float)) m ON true GROUP BY r.period, r.set_id)
SELECT 'a' AS src, period, cat, s, n FROM a UNION ALL SELECT 'b', period, NULL, s, n FROM b UNION ALL SELECT 'c', period, set_id::text, mx, n FROM c $q$ || extra $f$;

CREATE FUNCTION memoize_nodes(q text, agg bool) RETURNS int LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	PERFORM set_config('sift.enable_agg', agg::text, true);
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN jsonb_array_length(jsonb_path_query_array(j, '$.** ? (@."Node Type" == "Memoize")'));
END $$;
CREATE FUNCTION plan_it(q text) RETURNS void LANGUAGE plpgsql AS $$ BEGIN EXECUTE 'EXPLAIN ' || q; END $$;

-- the plan PostgreSQL makes alone caches the lookups
SELECT memoize_nodes(series_query(), false) > 0 AS postgres_memoizes;
-- the plan with the shared CTE does not
SELECT memoize_nodes(series_query(), true) AS memoize_nodes;
-- the setting is what it was after a plan, and after a plan that fails
SHOW enable_memoize;
DO $$ BEGIN
	PERFORM plan_it(series_query('WHERE n > 1 / 0'));
EXCEPTION WHEN division_by_zero THEN RAISE NOTICE 'planning failed';
END $$;
SHOW enable_memoize;
DROP FUNCTION plan_it, memoize_nodes, series_query;
DROP TABLE events, kinds;
DROP EXTENSION pg_sift;
