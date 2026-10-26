-- A partitioned table whose partitions are sift_tables is one source of Custom Scan (SiftAgg), as one whose partitions carry a sift_col index is
-- (agg_partitions): the partitions PostgreSQL keeps are read one after the other through their own sources. Every statement is run with the pushdown and
-- with sift.enable_agg = off: the rows must be the same.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE pt (id int, g int, d date NOT NULL, v int, s text) PARTITION BY RANGE (d);
CREATE TABLE pt_a PARTITION OF pt FOR VALUES FROM ('2024-01-01') TO ('2024-04-01') USING sift_table;
-- other attribute numbers: a dropped column before the others
CREATE TABLE pt_b (junk text, id int, g int, d date NOT NULL, v int, s text) USING sift_table;
ALTER TABLE pt_b DROP COLUMN junk;
ALTER TABLE pt ATTACH PARTITION pt_b FOR VALUES FROM ('2024-04-01') TO ('2024-07-01');
CREATE TABLE pt_c PARTITION OF pt FOR VALUES FROM ('2024-07-01') TO ('2024-10-01') PARTITION BY HASH (id);
CREATE TABLE pt_c0 PARTITION OF pt_c FOR VALUES WITH (MODULUS 2, REMAINDER 0) USING sift_table;
CREATE TABLE pt_c1 PARTITION OF pt_c FOR VALUES WITH (MODULUS 2, REMAINDER 1) USING sift_table;
CREATE TABLE pt_rest PARTITION OF pt DEFAULT USING sift_table;

INSERT INTO pt SELECT i, i % 11, DATE '2023-12-01' + (i * 7) % 420, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 1000 END,
	(ARRAY['red', 'green', 'blue', NULL])[1 + i % 4] FROM generate_series(1, 60000) i;
VACUUM (FREEZE, ANALYZE) pt;
-- rows in the write buffers, deleted rows and an updated row, which the units on disk do not hold
INSERT INTO pt SELECT i, i % 11, DATE '2024-02-01' + i % 300, i % 1000, 'new' FROM generate_series(60001, 60500) i;
DELETE FROM pt WHERE id % 97 = 0;
UPDATE pt SET v = -1 WHERE id = 5;

\ir test/agg_rows_helpers.sql
CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; p text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
		IF l LIKE '%Partitions:%' THEN p := ' ' || trim(split_part(l, ':', 2)); END IF;
		IF l LIKE '%Workers Planned: _%' AND l NOT LIKE '%Workers Planned: 0%' THEN r := r || ' with workers'; END IF;
	END LOOP;
	RETURN r || p;
END $$;

CREATE FUNCTION same(q text) RETURNS TABLE (plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	plan := plan_of(q);
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE TABLE pt_q (n serial, q text);
INSERT INTO pt_q (q) VALUES
	('SELECT count(*) AS n, count(v) AS nv, sum(v) AS s, min(d) AS lo, max(d) AS hi FROM pt'),
	('SELECT g, count(*) AS n, sum(v) AS s FROM pt GROUP BY g'),
	('SELECT s, count(*) AS n, count(DISTINCT g) AS dg FROM pt GROUP BY s'),
	('SELECT date_trunc(''month'', d) AS m, count(*) AS n FROM pt GROUP BY 1'),
	('SELECT g, count(*) AS n FROM pt WHERE d >= ''2024-05-01'' AND d < ''2024-08-01'' AND v > 300 GROUP BY g'),
	('SELECT count(*) AS n FROM pt WHERE d < ''2024-02-01'''),
	('SELECT count(*) AS n FROM pt WHERE id = 1234'),
	('SELECT id, v FROM pt ORDER BY v DESC, id LIMIT 7'),
	('SELECT s, g, count(*) AS n FROM pt WHERE s IN (''red'', ''blue'') GROUP BY s, g');
SELECT n, plan, result_rows, vs_normal_plan FROM pt_q, LATERAL same(q) ORDER BY n;

-- the same with workers: the partitions are shared between them
SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;
SELECT n, plan, result_rows, vs_normal_plan FROM pt_q, LATERAL same(q) ORDER BY n;
RESET max_parallel_workers_per_gather;

-- a partition of another storage, or with its columns in another order, keeps the normal plan
CREATE TABLE pt_h (LIKE pt);
ALTER TABLE pt DETACH PARTITION pt_rest;
ALTER TABLE pt ATTACH PARTITION pt_h DEFAULT;
SELECT plan_of('SELECT g, count(*) FROM pt GROUP BY g');
CREATE TABLE pt_o (s text, v int, d date NOT NULL, g int, id int) USING sift_table;
ALTER TABLE pt DETACH PARTITION pt_h;
ALTER TABLE pt ATTACH PARTITION pt_o DEFAULT;
SELECT plan_of('SELECT g, count(*) FROM pt GROUP BY g');

DROP TABLE pt, pt_h, pt_rest, pt_q;
DROP FUNCTION plan_of(text), same(text), agg_pushed(text), agg_rows(text, bool);
DROP EXTENSION pg_sift;
