-- The outer sum of an int8 aggregate of the arms (sum of a count, or of a sum of int4 values) is a 128-bit sum that comes out as numeric, as
-- sum(int8) does in PostgreSQL: it never raises "exceeds bigint", it is exact however many cells it adds. The inner values here are near 2^31
-- so that the cells add up to a large exact total; the sum can only leave bigint past 2^32 such rows, which no test can hold (the bound was
-- checked by adding 2^62 to every cell). Each query runs as the scan, in parallel and as the normal plan; the rows and the type must be the same.
CREATE EXTENSION pg_sift;
-- the tuples through the arms are in the price of the scan: on these few rows the normal plan is the cheaper one, and the test asks about the scan
SET sift.agg_force = on;
SET seq_page_cost = 1000; SET random_page_cost = 1000;

CREATE TABLE events (id int PRIMARY KEY, kind text, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, 'k' || i % 5,
       jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || (i + j) % 6, 'qty', 2147483647 - (i * j) % 7)) FROM generate_series(1, 1 + i % 4) j))
FROM generate_series(1, 12000) i;
CREATE INDEX events_col ON events USING sift_col (id, kind, doc) WITH (paths = 'doc.parts[]{cat text, qty int4}');
VACUUM (FREEZE, ANALYZE) events;

CREATE TABLE cases (n int, shape text, q text);
INSERT INTO cases VALUES
	(1, 'by category and total', $q$SELECT category, sum(q) AS total, sum(n) AS n FROM (
	   SELECT e.id, o.cat AS category, sum(o.qty) AS q, count(*) AS n FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, qty int4) GROUP BY e.id, o.cat
	   UNION ALL
	   SELECT e.id, NULL::text, sum(o.qty), count(*) FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, qty int4) GROUP BY e.id) t
	 GROUP BY category$q$),
	(2, 'no category', $q$SELECT sum(q) AS total, sum(n) AS n FROM (
	   SELECT e.id, o.cat AS category, sum(o.qty) AS q, count(*) AS n FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, qty int4) GROUP BY e.id, o.cat
	   UNION ALL
	   SELECT e.id, NULL::text, sum(o.qty), count(*) FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, qty int4) GROUP BY e.id) t$q$),
	(3, 'no rows', $q$SELECT category, sum(q) AS total FROM (
	   SELECT e.id, o.cat AS category, sum(o.qty) AS q FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, qty int4) WHERE e.id < 0 GROUP BY e.id, o.cat
	   UNION ALL
	   SELECT e.id, NULL::text, sum(o.qty) FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, qty int4) WHERE e.id < 0 GROUP BY e.id) t
	 GROUP BY category$q$);

-- the type of the total, whether the scan made it, and the rows compared as text
CREATE FUNCTION check_case(q text, OUT pushed bool, OUT total_type text, OUT differences bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
	SELECT count(*) INTO differences FROM ((SELECT r_off::text FROM r_off EXCEPT ALL SELECT r_on::text FROM r_on) UNION ALL (SELECT r_on::text FROM r_on EXCEPT ALL SELECT r_off::text FROM r_off)) d;
	SELECT format_type(atttypid, NULL) INTO total_type FROM pg_attribute WHERE attrelid = 'r_on'::regclass AND attname = 'total';
	pushed := false;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		pushed := pushed OR l LIKE '%sum(inner int8)%';
	END LOOP;
	DROP TABLE r_off, r_on;
END $$;

SET max_parallel_workers_per_gather = 0;
SELECT n, shape, c.* FROM cases, LATERAL check_case(q) c ORDER BY n;

SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;
SELECT n, shape, c.* FROM cases, LATERAL check_case(q) c ORDER BY n;

DROP TABLE events, cases;
DROP EXTENSION pg_sift;
DROP FUNCTION check_case;
