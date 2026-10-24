-- The rows of a joined table, VALUES list, sub-select or function are loaded once, by the leader, and the workers of a scan read them from the
-- shared memory: a query with such a join plans workers, and its rows are those of the scan without workers and of the normal plan. A child plan
-- that runs per combination of leaves (unless the leader reads it as a table of the first row of a unique key), and a query with a function that is
-- unsafe or restricted in a worker, keep one process.
CREATE EXTENSION pg_sift;

SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET seq_page_cost = 10000;
SET sift.agg_preagg = off;

CREATE TABLE zones (id int PRIMARY KEY, name text, tier int, w numeric, tag text);
INSERT INTO zones SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE 'z' || i % 7 END, i % 3, CASE WHEN i % 13 = 0 THEN NULL ELSE i * 1.25 END,
                      repeat('t', i % 5) FROM generate_series(0, 59) i;
CREATE TABLE labels (kind text, label text);
INSERT INTO labels VALUES ('a', 'first'), ('a', 'again'), ('b', 'second'), (NULL, 'none');
CREATE TABLE ev (id int PRIMARY KEY, zone_id int, day date, kind text, qty int, price float8) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 65 END, '2026-03-01'::date + i % 40, (ARRAY['a', 'b', 'c'])[1 + i % 3], i % 9, (i % 13) + 0.5
FROM generate_series(1, 12000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, zone_id, day, kind, qty, price);
VACUUM ANALYZE ev;
ANALYZE zones;
ANALYZE labels;

CREATE FUNCTION unsafe_id(int) RETURNS int LANGUAGE sql PARALLEL UNSAFE AS 'SELECT $1';
CREATE FUNCTION restricted_imm(int) RETURNS int LANGUAGE plpgsql IMMUTABLE PARALLEL RESTRICTED AS 'BEGIN RETURN $1; END';

CREATE FUNCTION p_rows(q text, push bool, workers int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION p_mode(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; par bool := false; agg bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		par := par OR l LIKE '%Workers Planned: 2%';
		agg := agg OR l LIKE '%Custom Scan (SiftAgg)%';
	END LOOP;
	RETURN CASE WHEN agg AND par THEN 'parallel' WHEN agg THEN 'serial' ELSE 'none' END;
END $$;

-- did a worker read the shared rows? (a launch can fail when the other sessions hold the workers: tried again)
CREATE FUNCTION p_shared(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; launched bool; shared bool;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);
	FOR i IN 1..20 LOOP
		launched := false;
		shared := false;
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			launched := launched OR l ~ 'Workers Launched: [1-9]';
			shared := shared OR l ~ 'Lookup Rows Shared: [1-9]';
		END LOOP;
		IF launched AND shared THEN RETURN true; END IF;
		PERFORM pg_sleep(0.2);
	END LOOP;
	RETURN false;
END $$;

CREATE TABLE p_q (n serial, q text, par bool);
INSERT INTO p_q (q, par) VALUES
	-- a table: text and numeric columns, NULL keys and columns, a key with two rows, a key with none
	('SELECT z.name, z.tier, count(*), sum(e.qty) FROM ev e JOIN zones z ON z.id = e.zone_id GROUP BY 1, 2', true),
	('SELECT l.label, count(*), sum(e.price) FROM ev e JOIN labels l ON l.kind = e.kind GROUP BY 1', true),
	('SELECT l.label, count(*), sum(e.price) FROM ev e LEFT JOIN labels l ON l.kind = e.kind GROUP BY 1', true),
	('SELECT e.kind, count(*) FROM ev e WHERE e.zone_id IN (SELECT id FROM zones WHERE tier = 1) GROUP BY 1', true),
	('SELECT e.kind, count(*) FROM ev e WHERE NOT EXISTS (SELECT 1 FROM zones z WHERE z.id = e.zone_id AND z.tier = 2) GROUP BY 1', true),
	-- a VALUES list, a sub-select, a function: the leader runs them
	('SELECT v.n, count(*), avg(e.price) FROM ev e JOIN (VALUES (0, ''zero''), (1, ''one''), (2, ''two''), (2, ''deux''), (NULL, ''null'')) v(i, n) ON v.i = e.qty GROUP BY 1', true),
	('SELECT v.up, count(*), sum(e.qty) FROM ev e JOIN (VALUES (''a'', ''A''), (''b'', ''B''), (''b'', ''BB'')) v(k, up) ON v.k = e.kind GROUP BY 1', true),
	('SELECT s.g, count(*), max(e.day) FROM ev e JOIN (SELECT id AS i, ''g'' || tier AS g FROM zones WHERE tier < 2) s ON s.i = e.zone_id GROUP BY 1', true),
	('SELECT g.i, count(*) FROM ev e JOIN generate_series(0, 20) g(i) ON g.i = e.qty * 2 GROUP BY 1', true),
	('SELECT v.n, count(*) FROM ev e LEFT JOIN (SELECT id AS i, name AS n FROM zones WHERE tier = 0) v ON v.i = e.zone_id GROUP BY 1', true),
	-- distinct, grouping sets, arms that share a pass
	('SELECT e.kind, count(DISTINCT e.zone_id), count(*) FROM ev e JOIN (VALUES (0, ''zero''), (1, ''one''), (2, ''two'')) v(i, n) ON v.i = e.qty GROUP BY e.kind', true),
	('SELECT e.qty, e.kind, count(*), sum(e.price) FROM ev e GROUP BY GROUPING SETS ((e.qty), (e.kind), ())', true),
	('SELECT v.n AS a, NULL::text AS b, count(*) AS c, sum(e.price) AS s FROM ev e JOIN (VALUES (0, ''zero''), (1, ''one'')) v(i, n) ON v.i = e.qty GROUP BY v.n
	  UNION ALL SELECT v.n, e.kind, count(*), sum(e.price) FROM ev e JOIN (VALUES (0, ''zero''), (1, ''one'')) v(i, n) ON v.i = e.qty GROUP BY v.n, e.kind
	  UNION ALL SELECT NULL, NULL, count(*), sum(e.price) FROM ev e JOIN (VALUES (0, ''zero''), (1, ''one'')) v(i, n) ON v.i = e.qty', true),
	('SELECT e.kind, sum(e.qty), sum(z.w) FROM ev e JOIN zones z ON z.id = e.zone_id GROUP BY 1', false),
	('SELECT e.kind, count(z.w), min(z.tag), max(z.tier) FROM ev e JOIN zones z ON z.id = e.zone_id GROUP BY 1', false),
	('SELECT count(*) FROM ev e WHERE e.qty NOT IN (SELECT i FROM (VALUES (1), (2), (7)) v(i))', false),
	-- a sum over a column of the joined table, a hashed sub-plan, a function that is unsafe or restricted in a worker: one process; a child plan the leader reads as a table of one row by unique key: workers
	('SELECT s.name, count(*) FROM ev e JOIN LATERAL (SELECT name FROM zones z WHERE z.id = e.zone_id LIMIT 1) s ON true GROUP BY 1', true),
	('SELECT v.n, unsafe_id(count(*)::int) FROM ev e JOIN (VALUES (0, ''zero''), (1, ''one'')) v(i, n) ON v.i = e.qty GROUP BY 1', false),
	('SELECT v.n, count(*) FROM ev e JOIN (VALUES (0, ''zero''), (1, ''one'')) v(i, n) ON v.i = e.qty WHERE restricted_imm(e.qty) < 5 GROUP BY 1', false);

CREATE FUNCTION p_compare(OUT n int, OUT mode text, OUT par_same bool, OUT off_same bool) RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE q text; x bool; want text;
BEGIN
	FOR n, q, x IN SELECT p_q.n, p_q.q, p_q.par FROM p_q ORDER BY p_q.n LOOP
		mode := p_mode(q);
		want := CASE WHEN x THEN 'parallel' ELSE 'serial' END;
		mode := CASE WHEN mode = want THEN 'as expected' ELSE mode || ' (expected ' || want || ')' END;
		par_same := p_rows(q, true, 2) IS NOT DISTINCT FROM p_rows(q, true, 0);
		off_same := p_rows(q, true, 2) IS NOT DISTINCT FROM p_rows(q, false, 0);
		RETURN NEXT;
	END LOOP;
END $$;

SELECT n, mode, par_same, off_same FROM p_compare();

-- the leader does not scan: the tables of the workers are the whole answer
SET parallel_leader_participation = off;
SELECT n, mode, par_same, off_same FROM p_compare();
RESET parallel_leader_participation;

-- the workers read the rows the leader shared
SELECT p_shared(q) AS shared FROM p_q WHERE n IN (1, 3, 7, 9, 10) ORDER BY n;

-- rows in the write buffer, rows updated and deleted since the index was built (the units that are dirty are read again from the heap)
INSERT INTO ev SELECT i, i % 65, '2026-04-01'::date + i % 10, (ARRAY['a', 'b', 'c'])[1 + i % 3], i % 9, (i % 13) + 0.5 FROM generate_series(12001, 12600) i;
UPDATE ev SET qty = 1, kind = 'b' WHERE id % 97 = 0;
DELETE FROM ev WHERE id % 89 = 0;
SELECT n, mode, par_same, off_same FROM p_compare();

-- a prepared statement with a generic plan, run again with other values
PREPARE p_gen (int) AS
	SELECT v.n, count(*), sum(e.price) FROM ev e JOIN (VALUES (0, 'zero'), (1, 'one'), (2, 'two')) v(i, n) ON v.i = e.qty WHERE e.zone_id >= $1 GROUP BY 1 ORDER BY 1;
SET plan_cache_mode = force_generic_plan;
SET sift.enable_agg = on;
EXECUTE p_gen (10);
EXECUTE p_gen (30);
EXECUTE p_gen (10);
SET sift.enable_agg = off;
EXECUTE p_gen (10);
EXECUTE p_gen (30);
RESET plan_cache_mode;
SET sift.enable_agg = on;

-- a cursor, and a query that runs again for every row of the outer query
BEGIN;
DECLARE c NO SCROLL CURSOR FOR
	SELECT v.n, count(*), sum(e.price) FROM ev e JOIN (VALUES (0, 'zero'), (1, 'one'), (2, 'two')) v(i, n) ON v.i = e.qty GROUP BY 1 ORDER BY 1;
FETCH 2 FROM c;
FETCH ALL FROM c;
COMMIT;

SELECT z.id, (SELECT count(*) FROM ev e JOIN (VALUES (1), (2)) v(i) ON v.i = e.qty WHERE e.zone_id = z.id) AS n
FROM zones z WHERE z.id < 5 ORDER BY z.id;
SET sift.enable_agg = off;
SELECT z.id, (SELECT count(*) FROM ev e JOIN (VALUES (1), (2)) v(i) ON v.i = e.qty WHERE e.zone_id = z.id) AS n
FROM zones z WHERE z.id < 5 ORDER BY z.id;

DROP TABLE ev, zones, labels, p_q;
DROP FUNCTION p_rows(text, bool, int), p_mode(text), p_shared(text), p_compare(), unsafe_id(int), restricted_imm(int);
DROP EXTENSION pg_sift;
