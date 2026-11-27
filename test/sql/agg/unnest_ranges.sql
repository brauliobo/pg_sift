-- The elements of the kept rows are found a row at a time (a range of elements each) when few rows are kept, and an element at a time (the owner of
-- each element looked up) when many are. Rows of 0 to 130 elements put the ranges across the 64 bit words of the bitmap; the answers stay the heap's whether
-- 0.5 %, 5 %, 30 % or 80 % of the rows are kept, with a conjunct on the elements, and for a left lateral (rows with no element).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;

CREATE TABLE ev (id int PRIMARY KEY, day date, items jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, '2026-01-01'::date + i % 400,
       (SELECT coalesce(jsonb_agg(jsonb_build_object('kind', 'c' || ((i + k) % 7), 'n', (i * k) % 9)), '[]') FROM generate_series(1, (i * 7) % 131) k)
FROM generate_series(1, 6000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, day, items) WITH (paths = 'items[]{kind text, n int4}');
-- rows of 0 to 2 elements: the lookup of the owner of each element is the cheaper way
CREATE TABLE ev2 (id int PRIMARY KEY, day date, items jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ev2 SELECT i, '2026-01-01'::date + i % 400,
       (SELECT coalesce(jsonb_agg(jsonb_build_object('kind', 'c' || ((i + k) % 7), 'n', (i * k) % 9)), '[]') FROM generate_series(1, i % 3) k)
FROM generate_series(1, 6000) i;
CREATE INDEX ev2_col ON ev2 USING sift_col (id, day, items) WITH (paths = 'items[]{kind text, n int4}');
VACUUM ANALYZE ev, ev2;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;

CREATE TABLE s_q (name text, q text);
INSERT INTO s_q SELECT 'kept ' || pct || ' %' || CASE WHEN el THEN ', element conjunct' ELSE '' END,
       'SELECT i.kind, count(*) AS n, sum(i.n) AS s, count(DISTINCT e.id) AS d FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.items) i(kind text, n int) WHERE (e.id + 0) % 200 < '
       || pct * 2 || CASE WHEN el THEN ' AND i.kind <> ''c3''' ELSE '' END || ' GROUP BY 1 ORDER BY 1'
FROM (VALUES (0.5), (5), (30), (80)) p(pct) CROSS JOIN (VALUES (false), (true)) x(el);
INSERT INTO s_q SELECT replace(name, 'kept', 'few elements, kept'), replace(q, ' ev e ', ' ev2 e ') FROM s_q WHERE name LIKE 'kept 30 %' OR name LIKE 'kept 80 %';
INSERT INTO s_q VALUES
 ('left lateral, kept 5 %', 'SELECT i.kind, count(*) AS n, count(e.id) AS ids FROM ev e LEFT JOIN LATERAL jsonb_to_recordset(e.items) i(kind text, n int) ON true WHERE (e.id + 0) % 200 < 10 GROUP BY 1 ORDER BY 1'),
 ('one row of the last word', 'SELECT i.kind, count(*) AS n FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.items) i(kind text, n int) WHERE e.id + 0 IN (63, 64, 127, 128, 5999, 6000) GROUP BY 1 ORDER BY 1');
-- the quals of the element: LIKE a literal inside the text or at its ends, IS DISTINCT FROM a constant
INSERT INTO s_q VALUES
 ('element LIKE', 'SELECT i.kind, count(*) AS n, sum(i.n) AS s FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.items) i(kind text, n int) WHERE i.kind LIKE ''%3%'' OR i.kind LIKE ''%5'' GROUP BY 1 ORDER BY 1'),
 ('element IS DISTINCT FROM', 'SELECT i.kind, count(*) AS n, sum(i.n) AS s FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.items) i(kind text, n int) WHERE i.kind IS DISTINCT FROM ''c3'' AND i.n IS DISTINCT FROM 4 GROUP BY 1 ORDER BY 1');
-- the quals of the row no scan key says (NOT IN, an OR of two columns, arithmetic), and a range of its ctid
INSERT INTO s_q VALUES
 ('row NOT IN', 'SELECT i.kind, count(*) AS n, sum(i.n) AS s FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.items) i(kind text, n int) WHERE e.id NOT IN (5, 63, 64, 5999) GROUP BY 1 ORDER BY 1'),
 ('row OR', 'SELECT i.kind, count(*) AS n, sum(i.n) AS s FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.items) i(kind text, n int) WHERE e.id < 100 OR e.day > ''2026-12-01'' GROUP BY 1 ORDER BY 1'),
 ('row arithmetic', 'SELECT i.kind, count(*) AS n, sum(i.n) AS s FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.items) i(kind text, n int) WHERE e.id * 3 % 7 = 2 GROUP BY 1 ORDER BY 1'),
 ('row ctid range', 'SELECT i.kind, count(*) AS n, sum(i.n) AS s FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.items) i(kind text, n int) WHERE e.ctid >= ''(30,3)'' AND e.ctid < ''(400,1)'' GROUP BY 1 ORDER BY 1');

-- the scan runs (SiftAgg), and its rows are the heap's
CREATE FUNCTION s_scan(qq text) RETURNS boolean LANGUAGE sql AS $$ SELECT sift_helpers.plan_mentions_siftagg_regex(qq) $$;

CREATE FUNCTION s_diff(qq text) RETURNS bigint LANGUAGE sql AS $$ SELECT sift_helpers.differences_agg_temp(qq) $$;
SELECT name, s_scan(q) AS scan, s_diff(q) AS differing_rows FROM s_q ORDER BY name;

-- a bound on the key of the table narrows the granules the scan of the elements reads, as it does a scan of the table alone; the rows stay the heap's
CREATE TABLE ur_p (id int PRIMARY KEY, items jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ur_p SELECT i, (SELECT jsonb_agg(jsonb_build_object('kind', 'c' || (i + k) % 5, 'n', k)) FROM generate_series(1, 40) k) FROM generate_series(1, 400) i;
CREATE INDEX ur_p_col ON ur_p USING sift_col (id, items) WITH (paths = 'items[]{kind text, n int4}');
VACUUM ANALYZE ur_p;
CREATE FUNCTION s_granules(qq text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || qq LOOP
		IF l ~ 'Granules: scanned=' THEN RETURN substring(l FROM 'scanned=(\d+)')::int; END IF;
	END LOOP;
	RETURN NULL;
END $$;
\set ur_all 'SELECT i.kind, count(*) AS n, sum(i.n) AS s FROM ur_p e CROSS JOIN LATERAL jsonb_to_recordset(e.items) i(kind text, n int) GROUP BY 1'
\set ur_few 'SELECT i.kind, count(*) AS n, sum(i.n) AS s FROM ur_p e CROSS JOIN LATERAL jsonb_to_recordset(e.items) i(kind text, n int) WHERE e.id < 40 GROUP BY 1'
SELECT s_scan(:'ur_few') AS scan, s_diff(:'ur_few') AS differing_rows, s_granules(:'ur_few') < s_granules(:'ur_all') OR s_granules(:'ur_all') = 1 AS fewer_granules;

RESET sift.enable_agg;
DROP FUNCTION s_scan(text), s_diff(text), s_granules(text);
DROP TABLE s_q, ev, ev2, ur_p;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
