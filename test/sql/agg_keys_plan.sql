-- A table that a semi, anti or inner join by one int4 column answers from a byte table of its keys (`k IN (SELECT ..)`, `EXISTS`, a join) is read for its keys
-- alone: the planner prices its load and its probes as that, and chooses the scan where it had kept PostgreSQL's hash join (3.6.0 priced them as a full lookup
-- table and a hash probe: the normal plan at 200,000 rows of events and a table of 100,000 rows of which 90,000 are loaded). Each statement is planned as it is (no force, no parallel workers), and
-- its rows are compared with sift.enable_agg off: with the lookup cache on, and off (the first statement of a backend).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE
	AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;
CREATE TABLE items AS SELECT i AS id FROM generate_series(1, 100000) i;
CREATE TABLE ev AS
SELECT g::bigint AS id, 1 + floor(100000 * pg_temp.u(g, 1))::int AS item_id, (pg_temp.u(g, 2) * 29)::int2 AS kind, round((pg_temp.u(g, 3) * 10)::numeric, 1)::float8 AS score,
	   CASE WHEN pg_temp.u(g, 4) < 0.015 THEN now() END AS deleted_at
FROM generate_series(1, 200000) g;
CREATE INDEX ev_col ON ev USING sift_col (item_id, kind, score, deleted_at);
CREATE TABLE keyset (n int, id int);
INSERT INTO keyset SELECT CASE WHEN pg_temp.u(id, 12) < 0.9 THEN 90000 ELSE 256 END, id FROM items;
CREATE TABLE dim_b AS SELECT n AS group_id, id AS item_id, NULL::timestamptz AS deleted_at FROM keyset;
VACUUM (FREEZE, ANALYZE) ev, keyset, dim_b;

CREATE FUNCTION differences(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	DROP TABLE r_on, r_off;
	RETURN d;
END $$;
CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE line text; res text := 'normal';
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF line ~ 'Custom Scan \(SiftAgg\)' THEN res := 'scan'; END IF;
	END LOOP;
	RETURN res;
END $$;

CREATE TABLE queries (name text, q text);
INSERT INTO queries VALUES
	('in',     'SELECT kind, count(*), round(avg(score)::numeric, 3) FROM ev e WHERE e.deleted_at IS NULL AND e.item_id IN (SELECT id FROM keyset WHERE n = 90000) GROUP BY 1'),
	('exists', 'SELECT kind, count(*), round(avg(score)::numeric, 3) FROM ev e WHERE e.deleted_at IS NULL AND EXISTS (SELECT 1 FROM dim_b m WHERE m.item_id = e.item_id AND m.group_id = 90000 AND m.deleted_at IS NULL) GROUP BY 1'),
	('join',   'SELECT kind, count(*), round(avg(score)::numeric, 3) FROM ev e JOIN dim_b m ON m.item_id = e.item_id AND m.group_id = 90000 WHERE e.deleted_at IS NULL GROUP BY 1'),
	('anti',   'SELECT kind, count(*), round(avg(score)::numeric, 3) FROM ev e WHERE e.deleted_at IS NULL AND e.item_id NOT IN (SELECT id FROM keyset WHERE n = 90000) GROUP BY 1');
SELECT name, plan_of(q) AS plan, differences(q) AS differences FROM queries ORDER BY name;

-- the first statement of a backend: the lookup cache and the shared lookup tables off
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
SELECT name, plan_of(q) AS plan, differences(q) AS differences FROM queries ORDER BY name;

DROP TABLE queries, dim_b, keyset, ev, items;
DROP FUNCTION plan_of(text), differences(text);
DROP EXTENSION pg_sift;
