-- A lookup joined through a btree index (the events of the entities of a list) is priced by the rows the index gives the loader, before the table's other filters, and
-- by a heap page visit for each: a list of entities that PostgreSQL's statistics cannot tell from a typical one (no longer in the most common values) owns many times
-- the events of a typical one, and the scan driven by the entities was priced at a twentieth of the normal plan though it ran as long. The scan must be driven by
-- the events (the entities are the lookup) for each list below, the hot and the cold ones; the rows are those of the normal plan.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dim_a (id int PRIMARY KEY, name text NOT NULL);
INSERT INTO dim_a SELECT g, 'entity ' || g FROM generate_series(1, 1000) g;
-- the events of an entity: a power law of its number, with no random draw (the statistics read every row at the target below)
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, event_date date NOT NULL, deleted_at timestamptz, score float8 NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, 1 + floor(1000 * power(((g::bigint * 2654435761) % 4294967296) / 4294967296.0, 2.6))::int, date '2026-01-01' + g * 7 % 300,
       CASE WHEN g % 50 = 0 THEN timestamptz '2026-02-01' END, g % 10 FROM generate_series(1, 200000) g;
CREATE INDEX events_entity ON events (dim_a_id);
CREATE INDEX entities_col ON dim_a USING sift_col (id, name) WITH (key_columns = 'auto: id', order_array_columns = 'auto: id');
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, event_date, deleted_at, score) WITH (key_cut = dim_a_id, key_columns = 'auto: dim_a_id');
ALTER TABLE events ALTER dim_a_id SET STATISTICS 10000;
VACUUM (FREEZE, ANALYZE) events, dim_a;

-- the entities ranked by their events from `skip`, `n` of them
CREATE FUNCTION kpi_query(skip int, n int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE ids text;
BEGIN
	SELECT string_agg(dim_a_id::text, ',') INTO ids FROM (SELECT dim_a_id FROM events GROUP BY dim_a_id ORDER BY count(*) DESC, dim_a_id OFFSET skip LIMIT n) t;
	RETURN format($q$SELECT en.id, en.name, count(*) FILTER (WHERE e.event_date >= '2026-09-29') AS cur, count(*) FILTER (WHERE e.event_date < '2026-09-29') AS prev, avg(e.score) AS score
		FROM events e JOIN dim_a en ON en.id = e.dim_a_id WHERE e.deleted_at IS NULL AND e.dim_a_id IN (%s) AND e.event_date >= '2026-02-28'
		GROUP BY en.id, en.name ORDER BY cur DESC, en.id LIMIT 20$q$, ids);
END $$;
-- the index the scan is driven by
CREATE FUNCTION driver_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Index: ' THEN RETURN trim(substring(l from 'Index: (\S+)')); END IF;
	END LOOP;
	RETURN 'normal plan';
END $$;
CREATE FUNCTION same_rows(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE 'SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (' || q || ') x' INTO a;
	SET sift.enable_agg = off;
	EXECUTE 'SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (' || q || ') x' INTO b;
	RESET sift.enable_agg;
	RETURN a = b;
END $$;

SELECT skip, n, driver_of(kpi_query(skip, n)) AS driver, same_rows(kpi_query(skip, n)) AS same
FROM (VALUES (0, 40), (100, 40), (200, 40), (400, 40), (900, 40), (900, 3)) v(skip, n) ORDER BY skip, n DESC;

DROP FUNCTION same_rows(text);
DROP FUNCTION driver_of(text);
DROP FUNCTION kpi_query(int, int);
DROP TABLE events, dim_a;
DROP EXTENSION pg_sift;
