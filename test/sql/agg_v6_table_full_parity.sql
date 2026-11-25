-- One sift_table (no sift_col index) against its heap twin, operator by operator and wide type by wide type. Every cell of the matrix runs one statement on the table and on the twin and compares
-- the rows (as text, by value for a numeric whose display scale may differ, in their order where the statement has one), and asserts that the plan reads the sift_table ("Table:", never "Index:")
-- through the node of the operator, or, for a cell the engine does not offer, that it does not (refused: the stock plan answers, with the same rows). The five wide types are decoded as datums,
-- so the cells that need no order of the key (groups, quals, min/max, count distinct, the star join's groups, the partitions) are served; the ordered, parallel and unnest cells of a wide key are refused.
-- The plans are steered to the engine (prices of the stock plans raised), so that a cell asserts what the engine offers and not what it costs. The matrix is printed as
-- op | type | expect | plan_ok | rows_ok; the cells are the ones of the EXPLAIN & parity matrix of the integration notes. Parallel cells also assert that workers were launched.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;
SET max_parallel_workers = 8;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET cpu_tuple_cost = 100;
SET random_page_cost = 10000;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET enable_bitmapscan = off;
SET enable_sort = off;

CREATE TABLE src (id int, g int, k int, s int, at timestamp, v int, u uuid, b bytea, c char(6), n name, nm numeric, ta text[]);
INSERT INTO src
SELECT i, i % 17, i % 400, i / 12, timestamp '2030-01-01' + (i % 5000) * interval '37 minutes', i % 1000,
	CASE WHEN i % 31 = 0 THEN NULL ELSE ('00000000-0000-4000-8000-' || lpad(to_hex(i % 60), 12, '0'))::uuid END,
	CASE WHEN i % 29 = 0 THEN NULL ELSE decode(lpad(to_hex(i % 40), 2, '0') || CASE WHEN i % 3 = 0 THEN '00' WHEN i % 3 = 1 THEN '20' ELSE '' END, 'hex') END,
	CASE WHEN i % 19 = 0 THEN NULL ELSE ('c' || (i % 7)) || repeat(' ', i % 3) END,
	CASE WHEN i % 23 = 0 THEN NULL ELSE ('name' || (i % 9))::name END,
	CASE WHEN i % 17 = 0 THEN NULL ELSE ((i % 11)::numeric / 4) * (CASE WHEN i % 2 = 0 THEN 1.0 ELSE 1.00 END) END,
	CASE i % 7 WHEN 0 THEN NULL WHEN 1 THEN '{}' ELSE ARRAY['a' || i % 3, 'b' || i % 5] END
FROM generate_series(1, 60000) i;
INSERT INTO src
SELECT 150000 + i, 9, i, 12500 + i, timestamp '2031-01-01', 5, u, b, c, n, nm, NULL
FROM (VALUES
	(1, '00000000-0000-0000-0000-000000000000'::uuid, '\x'::bytea, ''::char(6), ''::name, '1.0'::numeric),
	(2, 'ffffffff-ffff-ffff-ffff-ffffffffffff', '\x00', '      ', 'a', '1.00'),
	(3, '00000000-0000-0000-0000-000000000001', '\x20', 'a', 'a ', '1.000'),
	(4, '00000000-0000-0000-0000-000000000100', '\x2000', 'a ', ' a', '0'),
	(5, '01000000-0000-0000-0000-000000000000', '\x0020', 'a    ', 'a  ', '-0'),
	(6, NULL, NULL, NULL, NULL, 'NaN'),
	(7, '00000000-0000-0000-0000-000000000000', '\xff', 'é', 'N', 'NaN'),
	(8, 'ffffffff-ffff-ffff-ffff-fffffffffffe', '\xffff', 'é  ', 'n ', 'Infinity'),
	(9, 'ffffffff-ffff-ffff-ffff-fffffffffffe', '\xfffe', 'abcdef', 'n', '-Infinity'),
	(10, '00000000-0000-0000-0000-000000000002', '\x20202020202020202020202020202020', 'abcde', repeat('x', 63), '1234567890123456789012345678901234567890.123456789012345678901234567890'),
	(11, '00000000-0000-0000-0000-000000000003', '\x01', 'a' || chr(1), 'n1', '1E+30'),
	(12, NULL, NULL, NULL, NULL, '1.5')
) AS e(i, u, b, c, n, nm);

CREATE TABLE ft (id int PRIMARY KEY, g int, k int, s int, at timestamp, v int, u uuid, b bytea, c char(6), n name, nm numeric, ta text[])
	USING sift_table WITH (autovacuum_enabled = false);
SELECT sift.table_set_option('ft', 'key_order', 's, at');
CREATE TABLE fh (LIKE ft INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE TABLE dt (k int PRIMARY KEY, label text, u uuid, b bytea, c char(6), n name, nm numeric) USING sift_table;
CREATE TABLE dh (LIKE dt INCLUDING ALL);
CREATE TABLE pp (id int, g int, v int, u uuid, b bytea, c char(6), n name, nm numeric) PARTITION BY RANGE (id);
CREATE TABLE pp1 PARTITION OF pp FOR VALUES FROM (0) TO (20000) USING sift_table;
CREATE TABLE pp2 PARTITION OF pp FOR VALUES FROM (20000) TO (40000) USING sift_table;
CREATE TABLE pp3 PARTITION OF pp FOR VALUES FROM (40000) TO (200000) USING sift_table;
CREATE TABLE ph (LIKE pp);
CREATE TABLE pm (id int, g int, v int, u uuid, b bytea, c char(6), n name, nm numeric) PARTITION BY RANGE (id);
CREATE TABLE pm1 PARTITION OF pm FOR VALUES FROM (0) TO (30000) USING sift_table;
CREATE TABLE pm2 PARTITION OF pm FOR VALUES FROM (30000) TO (200000);
CREATE INDEX pm2_sift ON pm2 USING sift_col (id, g, v);
CREATE TABLE pmh (LIKE pm);

INSERT INTO ft SELECT * FROM src;
INSERT INTO fh SELECT * FROM src;
INSERT INTO dt SELECT g, 'l' || g % 8, ('00000000-0000-4000-8000-' || lpad(to_hex(g % 60), 12, '0'))::uuid, decode(lpad(to_hex(g % 40), 2, '0'), 'hex'),
	'c' || g % 7, ('name' || g % 9)::name, (g % 11)::numeric / 4 FROM generate_series(0, 399) g;
INSERT INTO dh SELECT * FROM dt;
INSERT INTO pp SELECT id, g, v, u, b, c, n, nm FROM src;
INSERT INTO ph SELECT * FROM pp;
INSERT INTO pm SELECT id, g, v, u, b, c, n, nm FROM src;
INSERT INTO pmh SELECT * FROM pm;
SELECT sift.table_maintain('ft') IS NOT NULL AS maintained;
INSERT INTO ft SELECT 300000 + id, g, k, s, at, v, u, b, c, n, nm, ta FROM src WHERE id % 50 = 0;
INSERT INTO fh SELECT 300000 + id, g, k, s, at, v, u, b, c, n, nm, ta FROM src WHERE id % 50 = 0;
DELETE FROM ft WHERE id % 97 = 0;
DELETE FROM fh WHERE id % 97 = 0;
UPDATE ft SET v = v + 1, c = 'upd' WHERE id % 89 = 0;
UPDATE fh SET v = v + 1, c = 'upd' WHERE id % 89 = 0;
DROP TABLE src;
ANALYZE ft; ANALYZE fh; ANALYZE dt; ANALYZE dh; ANALYZE pp; ANALYZE ph; ANALYZE pm; ANALYZE pmh;

CREATE FUNCTION tw(q text, twin boolean) RETURNS text LANGUAGE sql IMMUTABLE AS
$$ SELECT CASE WHEN twin THEN replace(replace(replace(replace(q, '@T', 'fh'), '@D', 'dh'), '@P', 'ph'), '@M', 'pmh')
	ELSE replace(replace(replace(replace(q, '@T', 'ft'), '@D', 'dt'), '@P', 'pp'), '@M', 'pm') END $$;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; plan text := '';
BEGIN
	FOR r IN EXECUTE 'EXPLAIN (COSTS off) ' || tw(q, false) LOOP plan := plan || r || E'\n'; END LOOP;
	RETURN plan;
END $$;

CREATE FUNCTION launched(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE r text; n int := 0;
BEGIN
	FOR r IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || tw(q, false) LOOP
		IF r LIKE '%Workers Launched: %' THEN n := greatest(n, substring(r from 'Launched: (\d+)')::int); END IF;
	END LOOP;
	RETURN n;
END $$;

-- mode: o = the rows as text in their order, u = the rows as text in any order, n = the rows by value in any order, ov = the rows by value in their order
-- (a numeric group has the scale of its first row in PostgreSQL, the least scale in the engine)
CREATE FUNCTION same(q text, mode text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a int; b int; x text; y text;
BEGIN
	IF mode IN ('n', 'ov') THEN
		x := CASE WHEN mode = 'ov' THEN 'SELECT row_number() OVER () AS rn, * FROM (%s) r' ELSE '%s' END;
		EXECUTE format('SELECT count(*) FROM ((' || x || ') EXCEPT ALL (' || x || ')) z', tw(q, false), tw(q, true)) INTO a;
		EXECUTE format('SELECT count(*) FROM ((' || x || ') EXCEPT ALL (' || x || ')) z', tw(q, true), tw(q, false)) INTO b;
		RETURN a + b = 0;
	END IF;
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' %s), '''') FROM (%s) r', CASE WHEN mode = 'u' THEN 'ORDER BY r::text' ELSE '' END, tw(q, false)) INTO x;
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' %s), '''') FROM (%s) r', CASE WHEN mode = 'u' THEN 'ORDER BY r::text' ELSE '' END, tw(q, true)) INTO y;
	RETURN x = y;
END $$;

CREATE FUNCTION same_serial(q text, mode text) RETURNS boolean LANGUAGE sql SET max_parallel_workers_per_gather = 0 AS $$ SELECT same(q, mode) $$;

-- expect v6: the plan has the node `must` reading the table `tbl`, and no index; expect refused: it has no `must` (and the rows are the twin's all the same)
CREATE FUNCTION cell(q text, mode text, expect text, must text, tbl text, par boolean) RETURNS SETOF boolean LANGUAGE plpgsql AS $$
DECLARE plan text := plan_of(q); plan_ok boolean; rows_ok boolean;
BEGIN
	IF expect = 'v6' THEN
		plan_ok := plan LIKE '%' || must || '%' AND plan LIKE '%Table: ' || tbl || E'\n%' AND plan NOT LIKE '%Index:%';
	ELSE
		plan_ok := plan NOT LIKE '%' || must || '%';
	END IF;
	IF par AND expect = 'v6' THEN plan_ok := plan_ok AND plan LIKE '%Workers Planned%' AND launched(q) > 0; END IF;
	rows_ok := same(q, mode);
	IF par THEN rows_ok := rows_ok AND same_serial(q, mode); END IF;
	RETURN NEXT plan_ok;
	RETURN NEXT rows_ok;
END $$;

CREATE TEMP TABLE types (ord int, ty text, c text, v1 text, v2 text);
INSERT INTO types VALUES
	(0, 'int', 'k', '7', '11'),
	(1, 'uuid', 'u', $$'00000000-0000-4000-8000-000000000007'$$, $$'ffffffff-ffff-ffff-ffff-fffffffffffe'$$),
	(2, 'numeric', 'nm', '1.5', '1.00'),
	(3, 'char(n)', 'c', $$'c1'$$, $$'a   '$$),
	(4, 'bytea', 'b', $$'\x07'$$, $$'\x00'$$),
	(5, 'name', 'n', $$'name3'$$, $$'n '$$);

-- ord, op, statement (@C the typed column, @V and @W its literals), mode, node, table, parallel, types refused, types PostgreSQL has no such statement for,
-- the int column and the node that stand for the type int (the baseline of the row)
CREATE TEMP TABLE ops (ord int, op text, q text, mode text, must text, tbl text, par boolean, refuses text[], skips text[], ic text, imust text);
INSERT INTO ops VALUES
	(10, 'group key', $q$SELECT @C, count(*), sum(v) FROM @T GROUP BY @C$q$, 'n', 'Custom Scan (SiftAggV6)', 'ft', false, '{}', '{}', 'k', NULL),
	(11, 'count distinct', $q$SELECT g, count(DISTINCT @C), count(@C) FROM @T GROUP BY g$q$, 'n', 'Custom Scan (SiftAggV6)', 'ft', false, '{}', '{}', 'k', NULL),
	(12, 'qual =', $q$SELECT g, count(*) FROM @T WHERE @C = @V GROUP BY g$q$, 'n', 'Custom Scan (SiftAggV6)', 'ft', false, '{}', '{}', 'k', NULL),
	(13, 'qual IN', $q$SELECT g, count(*) FROM @T WHERE @C IN (@V, @W) GROUP BY g$q$, 'n', 'Custom Scan (SiftAggV6)', 'ft', false, '{}', '{}', 'k', NULL),
	(14, 'qual IS NULL', $q$SELECT g, count(*) FROM @T WHERE @C IS NULL GROUP BY g$q$, 'n', 'Custom Scan (SiftAggV6)', 'ft', false, '{}', '{int}', 'k', NULL),
	(15, 'min/max', $q$SELECT g, min(@C), max(@C) FROM @T GROUP BY g$q$, 'n', 'Custom Scan (SiftAggV6)', 'ft', false, '{}', '{uuid}', 'k', NULL),
	(16, 'group key (edge rows)', $q$SELECT @C, count(*) FROM @T WHERE id > 150000 AND id < 150013 GROUP BY @C$q$, 'n', 'Custom Scan (SiftAggV6)', 'ft', false, '{}', '{}', 'k', NULL),
	(17, 'count distinct (edge rows)', $q$SELECT count(DISTINCT @C), count(@C), count(*) FROM @T WHERE id > 150000 AND id < 150013$q$, 'n', 'Custom Scan (SiftAggV6)', 'ft', false, '{}', '{}', 'k', NULL),
	(18, 'qual = (edge rows)', $q$SELECT count(*), count(@C) FROM @T WHERE id > 150000 AND id < 150013 AND @C = @W$q$, 'n', 'Custom Scan (SiftAggV6)', 'ft', false, '{}', '{}', 'k', NULL),
	(20, 'parallel group key', $q$SELECT @C, count(*), sum(v) FROM @T GROUP BY @C$q$, 'n', 'Parallel Custom Scan (SiftAggV6)', 'ft', true, '{uuid,numeric,char(n),bytea,name}', '{}', 'k', NULL),
	(21, 'parallel count distinct', $q$SELECT g, count(DISTINCT @C) FROM @T GROUP BY g$q$, 'n', 'Parallel Custom Scan (SiftAggV6)', 'ft', true, '{uuid,numeric,char(n),bytea,name}', '{}', 'k', NULL),
	(30, 'top-n', $q$SELECT id, @C FROM @T ORDER BY @C, id LIMIT 7$q$, 'o', 'Top-N:', 'ft', false, '{}', '{}', 'v', 'Custom Scan (SiftAggV6)'),
	(31, 'top-n desc offset', $q$SELECT id, @C FROM @T ORDER BY @C DESC NULLS LAST, id DESC LIMIT 9 OFFSET 3$q$, 'o', 'Top-N:', 'ft', false, '{}', '{}', 'v', 'Custom Scan (SiftAggV6)'),
	(32, 'rows (payload)', $q$SELECT id, @C FROM @T WHERE k < 10 ORDER BY k, id LIMIT 5$q$, 'o', 'Custom Scan (SiftAggV6)', 'ft', false, '{}', '{}', 'v', NULL),
	(33, 'rows (key)', $q$SELECT id, @C FROM @T WHERE k < 10 ORDER BY @C, id LIMIT 5$q$, 'o', 'Custom Scan (SiftAggV6)', 'ft', false, '{}', '{}', 'v', NULL),
	(40, 'stream', $q$SELECT @C, count(*), sum(v) FROM @T GROUP BY @C ORDER BY @C$q$, 'ov', 'Custom Scan (SiftAggV6Stream)', 'ft', false, '{uuid,numeric,char(n),bytea,name}', '{}', 's', NULL),
	(41, 'window key', $q$SELECT id, @C, rank() OVER (PARTITION BY @C ORDER BY at) FROM @T$q$, 'u', 'Custom Scan (SiftAggV6Window)', 'ft', false, '{uuid,numeric,char(n),bytea,name}', '{}', 's', NULL),
	(42, 'window payload', $q$SELECT id, @C, rank() OVER (PARTITION BY s ORDER BY at) FROM @T$q$, 'u', 'Custom Scan (SiftAggV6Window)', 'ft', false, '{}', '{}', 's', NULL),
	(43, 'distinct on', $q$SELECT DISTINCT ON (@C) @C, at FROM @T ORDER BY @C, at$q$, 'o', 'Custom Scan (SiftAggV6Window)', 'ft', false, '{uuid,numeric,char(n),bytea,name}', '{}', 's', NULL),
	(44, 'unnest', $q$SELECT @C, e, count(*) FROM @T, unnest(ta) e GROUP BY @C, e$q$, 'n', 'Unnest: lateral columnar', 'ft', false, '{uuid,numeric,char(n),bytea,name}', '{}', 'k', NULL),
	(50, 'sip key', $q$SELECT f.id, f.@C FROM @T f JOIN @D d ON d.@C = f.@C WHERE d.label = 'l5'$q$, 'u', 'Custom Scan (SiftAggV6Sip)', 'ft', false, '{uuid,numeric,char(n),bytea,name}', '{}', 'k', NULL),
	(51, 'sip fact qual', $q$SELECT f.id, f.k FROM @T f JOIN @D d ON d.k = f.k WHERE d.label = 'l5' AND f.@C = @V$q$, 'u', 'Custom Scan (SiftAggV6Sip)', 'ft', false, '{}', '{}', 'g', NULL),
	(52, 'star key', $q$SELECT d.label, f.@C, count(*) FROM @T f JOIN @D d ON d.k = f.k GROUP BY d.label, f.@C$q$, 'n', 'Star Dimensions', 'ft', false, '{}', '{}', 'k', NULL),
	(53, 'star fact qual', $q$SELECT d.label, count(*), sum(f.v) FROM @T f JOIN @D d ON d.k = f.k WHERE f.@C = @V GROUP BY d.label$q$, 'n', 'Table: ft', 'ft', false, '{uuid,numeric,char(n),bytea,name}', '{}', 'g', 'Star Dimensions'),
	(54, 'sip semi (IN)', $q$SELECT f.id, f.k FROM @T f WHERE f.k IN (SELECT d.k FROM @D d WHERE d.label = 'l5') AND f.@C = @V$q$, 'u', 'Custom Scan (SiftAggV6Sip)', 'ft', false, '{}', '{}', 'g', NULL),
	(55, 'sip semi (EXISTS)', $q$SELECT f.id, f.k FROM @T f WHERE EXISTS (SELECT 1 FROM @D d WHERE d.k = f.k AND d.label = 'l5') AND f.@C = @V$q$, 'u', 'Custom Scan (SiftAggV6Sip)', 'ft', false, '{}', '{}', 'g', NULL),
	(56, 'sip anti (NOT EXISTS)', $q$SELECT f.id, f.k FROM @T f WHERE NOT EXISTS (SELECT 1 FROM @D d WHERE d.k = f.k AND d.label = 'l5') AND f.@C = @V$q$, 'u', 'Custom Scan (SiftAggV6Sip)', 'ft', false, '{uuid,numeric,char(n),bytea,name,int}', '{}', 'g', NULL),
	(60, 'partitioned key', $q$SELECT @C, count(*), sum(v) FROM @P GROUP BY @C$q$, 'n', 'Custom Scan (SiftAggV6)', 'pp1', false, '{}', '{}', 'g', NULL),
	(61, 'partitioned count distinct', $q$SELECT g, count(DISTINCT @C) FROM @P GROUP BY g$q$, 'n', 'Custom Scan (SiftAggV6)', 'pp1', false, '{}', '{}', 'v', NULL),
	(62, 'partitioned qual =', $q$SELECT g, count(*) FROM @P WHERE @C = @V GROUP BY g$q$, 'n', 'Custom Scan (SiftAggV6)', 'pp1', false, '{uuid,numeric,char(n),bytea,name}', '{}', 'v', NULL),
	(70, 'parallel partitioned key', $q$SELECT @C, count(*), sum(v) FROM @P GROUP BY @C$q$, 'n', 'Parallel Custom Scan (SiftAggV6)', 'pp1', true, '{uuid,numeric,char(n),bytea,name}', '{}', 'g', NULL),
	(71, 'parallel partitioned count distinct', $q$SELECT g, count(DISTINCT @C) FROM @P GROUP BY g$q$, 'n', 'Parallel Custom Scan (SiftAggV6)', 'pp1', true, '{uuid,numeric,char(n),bytea,name}', '{}', 'v', NULL),
	(80, 'parallel mixed key', $q$SELECT @C, count(*), sum(v) FROM @M GROUP BY @C$q$, 'n', 'Parallel Custom Scan (SiftAggV6)', 'pm1', true, '{uuid,numeric,char(n),bytea,name}', '{}', 'g', NULL),
	(81, 'parallel mixed count distinct', $q$SELECT g, count(DISTINCT @C) FROM @M GROUP BY g$q$, 'n', 'Parallel Custom Scan (SiftAggV6)', 'pm1', true, '{uuid,numeric,char(n),bytea,name}', '{}', 'v', NULL);

CREATE TEMP TABLE matrix (phase text, ord int, tord int, op text, ty text, expect text, plan_ok boolean, rows_ok boolean);

CREATE FUNCTION fill(phase text, parallel_pass boolean) RETURNS void LANGUAGE sql AS $$
	INSERT INTO matrix
	SELECT phase, o.ord, t.ord, o.op, t.ty, e.expect, c.r[1], c.r[2]
	FROM ops o CROSS JOIN types t,
		LATERAL (SELECT CASE WHEN t.ty = ANY(o.refuses) THEN 'refused' ELSE 'v6' END AS expect) e,
		LATERAL (SELECT CASE WHEN t.ty = 'numeric' THEN o.mode WHEN o.mode = 'n' THEN 'u' WHEN o.mode = 'ov' THEN 'o' ELSE o.mode END AS mode) m,
		LATERAL (SELECT array_agg(x) AS r FROM cell(replace(replace(replace(o.q, '@C', CASE WHEN t.ty = 'int' THEN o.ic ELSE t.c END), '@V', t.v1), '@W', t.v2), m.mode, e.expect,
			CASE WHEN t.ty = 'int' THEN coalesce(o.imust, o.must) ELSE o.must END, o.tbl, o.par) x) c
	WHERE o.par = parallel_pass AND NOT t.ty = ANY(o.skips) $$;

CREATE FUNCTION fill_all(phase text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	SET LOCAL max_parallel_workers_per_gather = 0;
	PERFORM fill(phase, false);
	SET LOCAL max_parallel_workers_per_gather = 4;
	SET LOCAL parallel_setup_cost = 0;
	SET LOCAL parallel_tuple_cost = 0;
	SET LOCAL min_parallel_table_scan_size = 0;
	PERFORM fill(phase, true);
END $$;

SELECT fill_all('staged');
SELECT sift.table_maintain('ft') IS NOT NULL AS maintained;
ANALYZE ft;
SELECT fill_all('maintained');

\echo == the matrix: per operator, the cells served by the sift_table (v6) and refused to the stock plan, over int and the five wide types, in both phases
SELECT op, count(*) FILTER (WHERE expect = 'v6') AS served, count(*) FILTER (WHERE expect = 'refused') AS refused, bool_and(plan_ok) AS plan_ok, bool_and(rows_ok) AS rows_ok
FROM matrix GROUP BY ord, op ORDER BY ord;
\echo == the matrix by type ("FAIL" marks a cell whose plan or rows are not the expected)
SELECT op, coalesce(min(CASE WHEN ty = 'int' THEN CASE WHEN plan_ok AND rows_ok THEN expect ELSE 'FAIL ' || expect END END), 'n/a') AS "int",
	coalesce(min(CASE WHEN ty = 'uuid' THEN CASE WHEN plan_ok AND rows_ok THEN expect ELSE 'FAIL ' || expect END END), 'n/a') AS "uuid",
	coalesce(min(CASE WHEN ty = 'numeric' THEN CASE WHEN plan_ok AND rows_ok THEN expect ELSE 'FAIL ' || expect END END), 'n/a') AS "numeric",
	coalesce(min(CASE WHEN ty = 'char(n)' THEN CASE WHEN plan_ok AND rows_ok THEN expect ELSE 'FAIL ' || expect END END), 'n/a') AS "char(n)",
	coalesce(min(CASE WHEN ty = 'bytea' THEN CASE WHEN plan_ok AND rows_ok THEN expect ELSE 'FAIL ' || expect END END), 'n/a') AS "bytea",
	coalesce(min(CASE WHEN ty = 'name' THEN CASE WHEN plan_ok AND rows_ok THEN expect ELSE 'FAIL ' || expect END END), 'n/a') AS "name"
FROM matrix GROUP BY ord, op ORDER BY ord;
SELECT count(*) AS cells, count(*) FILTER (WHERE plan_ok AND rows_ok) AS cells_ok, bool_and(plan_ok AND rows_ok) AS all_cells_ok FROM matrix;

\echo == plan and parity checks that name their operator
SELECT plan_of('SELECT g, sum(v) FROM @T GROUP BY g') LIKE '%Custom Scan (SiftAggV6)%Table: ft%' AND plan_of('SELECT g, sum(v) FROM @T GROUP BY g') NOT LIKE '%Index:%' AS serial_reads_the_table;
SELECT plan_of('SELECT g, sum(v) FROM @P GROUP BY g') LIKE '%Partitions: 3%' AS partitioned_reads_its_three_partitions;
SELECT plan_of('SELECT g, sum(v) FROM @M GROUP BY g') LIKE '%Partitions: 2%' AS mixed_reads_its_two_partitions;
SET max_parallel_workers_per_gather = 4;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT plan_of('SELECT g, sum(v) FROM @T GROUP BY g') LIKE '%Workers Planned%Parallel Custom Scan (SiftAggV6)%' AS parallel_table_planned;
SELECT launched('SELECT g, sum(v) FROM @T GROUP BY g') > 0 AS parallel_table_workers_launched;
SELECT launched('SELECT g, sum(v) FROM @P GROUP BY g') > 0 AS parallel_partitioned_workers_launched;
SELECT launched('SELECT g, sum(v) FROM @M GROUP BY g') > 0 AS parallel_mixed_workers_launched;
SELECT launched('SELECT u, count(*) FROM @T GROUP BY u') = 0 AS parallel_wide_key_runs_serial;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
SET max_parallel_workers_per_gather = 0;

\echo == the groups of the edge rows (ids 150001 to 150012): the count on the table is the one of its twin and the one the equality of PostgreSQL makes
CREATE FUNCTION edge(q text, expected bigint) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint;
BEGIN
	EXECUTE tw(q, false) INTO a;
	EXECUTE tw(q, true) INTO b;
	RETURN a = expected AND b = expected AND plan_of(q) LIKE '%Custom Scan (SiftAggV6)%Table: ft%';
END $$;
SELECT edge('SELECT count(DISTINCT nm) FROM @T WHERE id > 150000 AND id < 150013', 8) AS numeric_values_of_the_edge_rows;
SELECT edge('SELECT count(*) FROM (SELECT nm FROM @T WHERE id IN (150001, 150002, 150003, 150005) GROUP BY nm) x', 2) AS numeric_1_0_1_00_1_000_are_one_group_and_0_is_another;
SELECT edge('SELECT count(*) FROM (SELECT nm FROM @T WHERE id IN (150006, 150007) GROUP BY nm) x', 1) AS numeric_nan_is_one_group;
SELECT edge('SELECT count(*) FROM (SELECT nm FROM @T WHERE id IN (150008, 150009) GROUP BY nm) x', 2) AS numeric_infinities_are_two_groups;
SELECT edge('SELECT count(*) FROM (SELECT c FROM @T WHERE id IN (150001, 150002) GROUP BY c) x', 1) AS char_empty_and_blanks_are_one_group;
SELECT edge('SELECT count(*) FROM (SELECT b FROM @T WHERE id IN (150001, 150002) GROUP BY b) x', 2) AS bytea_empty_and_zero_byte_are_two_groups;
SELECT edge('SELECT count(*) FROM (SELECT n FROM @T WHERE id IN (150002, 150003) GROUP BY n) x', 2) AS name_trailing_blank_is_a_value;
SELECT edge('SELECT count(*) FROM (SELECT u FROM @T WHERE id IN (150001, 150007) GROUP BY u) x', 1) AS uuid_equal_values_are_one_group;
SELECT plan_of('SELECT id, n FROM @T ORDER BY n, id LIMIT 7') LIKE '%Table: ft%Top-N: n, id%' AS name_top_n_is_the_top_n_node;

DROP TABLE ft, fh, dt, dh, pp, ph, pm, pmh;
DROP FUNCTION tw(text, boolean), plan_of(text), launched(text), same(text, text), same_serial(text, text), cell(text, text, text, text, text, boolean), fill(text, boolean), fill_all(text), edge(text, bigint);
DROP EXTENSION pg_sift;
