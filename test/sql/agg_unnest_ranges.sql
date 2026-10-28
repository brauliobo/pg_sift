-- The elements of the kept rows are found a row at a time (a range of elements each) when few rows are kept, and an element at a time (the owner of
-- each element looked up) when many are. Rows of 0 to 130 elements put the ranges across the 64 bit words of the bitmap; the answers stay the heap's whether
-- 0.5 %, 5 %, 30 % or 80 % of the rows are kept, with a conjunct on the elements, and for a left lateral (rows with no element).
CREATE EXTENSION pg_sift;

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
SET sift.agg_preagg = off;
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

-- the scan runs (SiftAgg), and its rows are the heap's
CREATE FUNCTION s_scan(qq text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || qq LOOP
		IF l ~ 'SiftAgg' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE FUNCTION s_diff(qq text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE b bigint;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE 'CREATE TEMP TABLE s_a ON COMMIT DROP AS ' || qq;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE 'CREATE TEMP TABLE s_b ON COMMIT DROP AS ' || qq;
	EXECUTE 'SELECT count(*) FROM ((SELECT * FROM s_a EXCEPT ALL SELECT * FROM s_b) UNION ALL (SELECT * FROM s_b EXCEPT ALL SELECT * FROM s_a)) d' INTO b;
	DROP TABLE s_a, s_b;
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN b;
END $$;
SELECT name, s_scan(q) AS scan, s_diff(q) AS differing_rows FROM s_q ORDER BY name;

RESET sift.enable_agg;
DROP FUNCTION s_scan(text), s_diff(text);
DROP TABLE s_q, ev, ev2;
DROP EXTENSION pg_sift;
