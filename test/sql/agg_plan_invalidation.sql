-- A prepared aggregate keeps its cached plan until something it reads changes; the next EXECUTE then plans again from the new catalog and
-- index state (the pushdown reads the index metadata and the zone sample while it plans, and a statement with a shared CTE is planned twice).
-- Between PREPARE and EXECUTE: ALTER INDEX, REINDEX, a new write buffer, DROP and CREATE of the index with other columns, a table rewrite
-- and DDL on the joined table. Every step compares the plan shape and the rows with sift.enable_agg = off.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
-- the table is small: page costs that keep the btree and bitmap plans from looking cheap as well
SET seq_page_cost = 10000; SET random_page_cost = 10000;
-- a statement without parameters is planned once and kept; only an invalidation plans it again
SET plan_cache_mode = force_generic_plan;

CREATE TABLE dims (id int PRIMARY KEY, kind text);
INSERT INTO dims SELECT i, 'cat_' || chr(97 + i % 4) FROM generate_series(1, 50) i;
CREATE TABLE events (id int PRIMARY KEY, dim_id int, ts date, kind text, amount int, payload jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, 1 + i % 50, '2025-01-01'::date + i % 40, 'cat_' || chr(97 + i % 4), i % 9,
       jsonb_build_object('parts', jsonb_build_object('a', i % 7), 'metrics', jsonb_build_object('k', 'catg_' || chr(97 + i % 3)))
FROM generate_series(1, 6000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_id, ts, kind, amount, payload) WITH (paths = 'payload.parts.a int, payload.metrics.k astext');
VACUUM (FREEZE, ANALYZE) events;
ANALYZE dims;

-- a lookup join, a jsonb leaf, and a shared CTE read by two aggregates
PREPARE lookup_agg AS
	SELECT d.kind, count(*) AS n, sum(e.amount) AS total FROM events e JOIN dims d ON d.id = e.dim_id WHERE e.ts >= '2025-01-10' GROUP BY d.kind;
PREPARE path_agg AS
	SELECT payload -> 'metrics' ->> 'k' AS k, count(*) AS n, sum((payload -> 'parts' ->> 'a')::int) AS s FROM events WHERE amount > 2 GROUP BY 1;
PREPARE shared_agg AS
	WITH s AS (SELECT dim_id, amount FROM events WHERE ts >= '2025-01-10')
	SELECT dim_id AS g, count(*) AS n FROM s GROUP BY dim_id UNION ALL SELECT dim_id % 3, sum(amount) FROM s GROUP BY dim_id % 3;

-- the plan of an EXECUTE (SiftAgg or normal) and the rows that differ between the pushdown and the normal plan
CREATE FUNCTION plan_of(name text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) EXECUTE ' || name LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN 'SiftAgg'; END IF;
	END LOOP;
	RETURN 'normal';
END $$;

CREATE FUNCTION diff_of(name text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS EXECUTE ' || name;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS EXECUTE ' || name;
	RESET sift.enable_agg;
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	DROP TABLE r_on, r_off;
	RETURN d;
END $$;

CREATE FUNCTION chk(label text) RETURNS TABLE (step text, lookup_plan text, lookup_diff bigint, path_plan text, path_diff bigint, shared_plan text, shared_diff bigint)
LANGUAGE plpgsql AS $$
BEGIN
	step := label;
	lookup_plan := plan_of('lookup_agg'); lookup_diff := diff_of('lookup_agg');
	path_plan := plan_of('path_agg'); path_diff := diff_of('path_agg');
	shared_plan := plan_of('shared_agg'); shared_diff := diff_of('shared_agg');
	RETURN NEXT;
END $$;

SELECT * FROM chk('prepared');
SELECT * FROM chk('cached');

-- a reloption change invalidates the index entry
ALTER INDEX events_col SET (write_buffer_limit = 128);
SELECT * FROM chk('alter index');

-- the index is rebuilt: new relation, generations start over
REINDEX INDEX events_col;
SELECT * FROM chk('reindex');

-- new rows sit in the write buffer; no catalog change at all, and the zones of the sample do not cover them
INSERT INTO events SELECT i, 1 + i % 50, '2025-03-01'::date + i % 20, 'cat_a', i % 9, jsonb_build_object('parts', jsonb_build_object('a', 3), 'metrics', jsonb_build_object('k', 'catg_a'))
FROM generate_series(6001, 6600) i;
SELECT * FROM chk('write buffer');
VACUUM events;
SELECT * FROM chk('vacuum');

-- without the index the plans are the normal ones, with an index that lacks a column too (the shared CTE takes the scan for the aggregates the index covers when
-- they cost 1.3 times less than the work they replace and the plan with the ordinary reader of the rest totals at most 1.1 times the shared one)
DROP INDEX events_col;
SELECT * FROM chk('drop index');
CREATE INDEX events_col ON events USING sift_col (id, dim_id, ts, kind);
SELECT * FROM chk('narrow index');
DROP INDEX events_col;
CREATE INDEX events_col ON events USING sift_col (id, dim_id, ts, kind, amount, payload) WITH (paths = 'payload.parts.a int, payload.metrics.k astext');
SELECT * FROM chk('new index');

-- the table is rewritten with another type of an indexed column (no column the queries return) and the index is built again
ALTER TABLE events ALTER COLUMN kind TYPE varchar(20);
SELECT * FROM chk('table rewrite');
-- the rewrite left no page all-visible: the scans check the rows of their pages in the heap, and are still the cheaper plan of the shared CTE
VACUUM (FREEZE, ANALYZE) events;
SELECT * FROM chk('vacuum after rewrite');

-- DDL on the lookup table
ALTER TABLE dims ADD COLUMN extra int;
SELECT * FROM chk('add column');
CREATE INDEX dims_kind ON dims (kind);
SELECT * FROM chk('lookup index');
ANALYZE dims;
ANALYZE events;
SELECT * FROM chk('analyze');

-- a second backend-visible change inside one transaction: the plan made after it sees the new index
BEGIN;
DROP INDEX events_col;
SELECT * FROM chk('drop in transaction');
ROLLBACK;
SELECT * FROM chk('rollback');

DROP FUNCTION chk(text), diff_of(text), plan_of(text);
DROP TABLE events, dims;
DROP EXTENSION pg_sift;
