-- A LATERAL sub-select cut to the first or the latest row of a sift_table, keyed by the row it is joined to (an equality or an inequality, a NULL key, a parent with no fact), is read by SiftAggV6 once for each
-- row of the outer table, with the parameters of that row; the rows must be the heap's own (sift.enable_agg = off), on a table of Format 1 and of Format 2.0, with the scans of the heap forced off.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET sift.test_unified_agg = on;
SET max_parallel_workers_per_gather = 0;
SET TimeZone = 'UTC';

CREATE FUNCTION make_tables(fmt text, format1 boolean) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('SET sift.test_create_format1 = %s', CASE WHEN format1 THEN 'on' ELSE 'off' END);
	EXECUTE format('CREATE TABLE events_%s (id int, parent_id int, ts date, kind text, deleted_at timestamptz) USING sift_table', fmt);
	EXECUTE format('CREATE TABLE facts_%s (id int, parent_id int, ts date, role text, val float8) USING sift_table', fmt);
	EXECUTE format($q$INSERT INTO events_%s
		SELECT i, CASE WHEN i %% 41 = 0 THEN NULL ELSE 1 + i %% 64 END, date '2025-01-01' + i %% 400, 'cat_' || (i %% 4), CASE WHEN i %% 29 = 0 THEN now() END
		FROM generate_series(1, 2400) i$q$, fmt);
	EXECUTE format($q$INSERT INTO facts_%s
		SELECT i, 1 + i %% 56, date '2024-12-01' + (i * 7) %% 450, 'cat_' || (i %% 4), round((1 + i %% 17 + (i %% 5) / 4.0)::numeric, 2)::float8
		FROM generate_series(1, 1500) i$q$, fmt);
END $$;

SELECT make_tables('f1', true);
SELECT make_tables('f2', false);
RESET sift.test_create_format1;
VACUUM (FREEZE, ANALYZE) events_f1, facts_f1, events_f2, facts_f2;

SELECT t AS "table", (sift.index_format(t::regclass)).major AS format_major
FROM unnest(ARRAY['events_f1', 'facts_f1', 'events_f2', 'facts_f2']) t;

CREATE FUNCTION same(q text) RETURNS TABLE (engine text, result_rows bigint, vs_heap bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	engine := 'normal';
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_indexscan = off;
	SET LOCAL enable_bitmapscan = off;
	SET LOCAL enable_indexonlyscan = off;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAggV6%' THEN engine := substring(l FROM 'Custom Scan \((SiftAggV6\w*)\)'); END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS SELECT x::text AS r FROM (%s) x', q);
	RESET enable_seqscan;
	RESET enable_indexscan;
	RESET enable_bitmapscan;
	RESET enable_indexonlyscan;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS SELECT x::text AS r FROM (%s) x', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_heap := (SELECT count(*) FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE TABLE stmts (id int, q text);
INSERT INTO stmts VALUES
-- the latest fact at or before the row: an inequality and an equality, two sort keys, the mean of a column of the sub-select
(1, 'SELECT e.kind, count(*) AS n, round(avg(l.val)::numeric, 3) AS mean FROM events_%1$s e LEFT JOIN LATERAL (SELECT f.val FROM facts_%1$s f WHERE f.parent_id = e.parent_id AND f.ts <= e.ts ORDER BY f.ts DESC, f.id DESC LIMIT 1) l ON true WHERE e.deleted_at IS NULL AND e.ts >= date ''2025-06-01'' AND e.ts < date ''2025-06-15'' GROUP BY 1 ORDER BY 1'),
(2, 'SELECT e.kind, count(*) AS n, count(l.val) AS found, sum(l.val) AS total FROM events_%1$s e LEFT JOIN LATERAL (SELECT f.val FROM facts_%1$s f WHERE f.parent_id = e.parent_id AND f.ts <= e.ts ORDER BY f.ts DESC, f.id DESC LIMIT 1) l ON true GROUP BY 1 ORDER BY 1'),
(3, 'SELECT e.kind, count(*) AS n, sum(l.val) AS total FROM events_%1$s e JOIN LATERAL (SELECT f.val FROM facts_%1$s f WHERE f.parent_id = e.parent_id AND f.ts <= e.ts ORDER BY f.ts DESC, f.id DESC LIMIT 1) l ON true GROUP BY 1 ORDER BY 1'),
-- the first fact at or after the row, ascending
(4, 'SELECT e.kind, count(l.val) AS found, sum(l.val) AS total FROM events_%1$s e LEFT JOIN LATERAL (SELECT f.val FROM facts_%1$s f WHERE f.parent_id = e.parent_id AND f.ts >= e.ts ORDER BY f.ts, f.id LIMIT 1) l ON true GROUP BY 1 ORDER BY 1'),
-- the first fact of the parent: an equality alone
(5, 'SELECT e.kind, count(*) AS n, sum(l.val) AS total FROM events_%1$s e LEFT JOIN LATERAL (SELECT f.val FROM facts_%1$s f WHERE f.parent_id = e.parent_id ORDER BY f.ts, f.id LIMIT 1) l ON true WHERE e.deleted_at IS NULL GROUP BY 1 ORDER BY 1');

SELECT id, f.fmt, s.*
FROM stmts, (VALUES ('f1'), ('f2')) f(fmt), LATERAL same(format(stmts.q, f.fmt)) s
ORDER BY id, f.fmt;

DROP TABLE stmts, events_f1, facts_f1, events_f2, facts_f2;
DROP FUNCTION same(text), make_tables(text, boolean);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
