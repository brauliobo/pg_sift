-- A partitioned table is one source of Custom Scan (SiftAgg) when every partition PostgreSQL keeps carries a sift_col index of the same
-- layout (docs/AGG.md, "Partitioned tables"). Every statement is run with the pushdown and with sift.enable_agg = off: the rows must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE pt (id int, g int, d date NOT NULL, v int, amount numeric(10, 2), s text, doc jsonb) PARTITION BY RANGE (d);
CREATE TABLE pt_a PARTITION OF pt FOR VALUES FROM ('2024-01-01') TO ('2024-04-01') WITH (autovacuum_enabled = false);
-- other attribute numbers: a dropped column before the indexed ones
CREATE TABLE pt_b (junk text, doc jsonb, s text, amount numeric(10, 2), v int, d date NOT NULL, g int, id int) WITH (autovacuum_enabled = false);
ALTER TABLE pt_b DROP COLUMN junk;
ALTER TABLE pt ATTACH PARTITION pt_b FOR VALUES FROM ('2024-04-01') TO ('2024-07-01');
-- partitioned again
CREATE TABLE pt_c PARTITION OF pt FOR VALUES FROM ('2024-07-01') TO ('2024-10-01') PARTITION BY HASH (id);
CREATE TABLE pt_c0 PARTITION OF pt_c FOR VALUES WITH (MODULUS 2, REMAINDER 0) WITH (autovacuum_enabled = false);
CREATE TABLE pt_c1 PARTITION OF pt_c FOR VALUES WITH (MODULUS 2, REMAINDER 1) WITH (autovacuum_enabled = false);
CREATE UNLOGGED TABLE pt_u PARTITION OF pt FOR VALUES FROM ('2024-10-01') TO ('2025-01-01') WITH (autovacuum_enabled = false);
CREATE TABLE pt_rest PARTITION OF pt DEFAULT WITH (autovacuum_enabled = false);

CREATE FUNCTION pt_fill(first_id int, last_id int) RETURNS void LANGUAGE sql AS $$
	INSERT INTO pt
	SELECT i, i % 11, DATE '2023-12-01' + (i * 7) % 420, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 1000 END, (i % 5000) / 10.0,
	       (ARRAY['red', 'green', 'blue', NULL])[1 + i % 4], jsonb_build_object('k', i % 9)
	FROM generate_series(first_id, last_id) i $$;
SELECT pt_fill(1, 6000);
-- on the parent: one index per partition, with the partitions' own attribute numbers
CREATE INDEX pt_col ON pt USING sift_col (id, g, d, v, amount, s, doc) WITH (paths = 'doc->k int4');
VACUUM (FREEZE, ANALYZE) pt;

-- the plan: SiftAgg with the partitions it reads (and workers), or the normal plan
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
	('SELECT count(*) AS n, count(v) AS nv, sum(v) AS s, min(d) AS lo, max(d) AS hi, sum(amount) AS a FROM pt'),
	('SELECT g, count(*) AS n, sum(v) AS s, min(id) AS lo, max(id) AS hi FROM pt GROUP BY g'),
	('SELECT s, count(*) AS n, count(DISTINCT g) AS gs FROM pt WHERE s IS NOT NULL AND s <> ''blue'' GROUP BY s'),
	('SELECT (doc->>''k'')::int AS k, count(*) AS n FROM pt GROUP BY 1'),
	-- the partition key as the key, and as a condition: PostgreSQL prunes at plan time, the scan reads the partitions left
	('SELECT date_trunc(''month'', d)::date AS m, count(*) AS n, sum(amount) AS s FROM pt GROUP BY 1'),
	('SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2024-04-01'' AND d < DATE ''2024-07-01'' GROUP BY g'),
	('SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2024-05-01'' AND d < DATE ''2024-08-15'' GROUP BY g'),
	('SELECT count(*) AS n FROM pt WHERE d >= DATE ''2025-01-01''');

SELECT n, (same(q)).* FROM pt_q ORDER BY n;

-- ONLY the partitioned table has no rows of its own; a statement on a partition is the plain table's
SELECT * FROM same('SELECT count(*) AS n FROM ONLY pt');
SELECT * FROM same('SELECT g, count(*) AS n FROM pt_b GROUP BY g');

-- a prepared statement whose parameter prunes partitions when it runs keeps the normal plan, which prunes; one on another column does not
SET plan_cache_mode = force_generic_plan;
PREPARE pt_pruned (date) AS SELECT g, count(*) AS n FROM pt WHERE d >= $1 AND d < DATE '2024-07-01' GROUP BY g;
PREPARE pt_param (int) AS SELECT g, count(*) AS n FROM pt WHERE g = $1 GROUP BY g;
SELECT plan_of('EXECUTE pt_pruned(DATE ''2024-05-01'')') AS pruned, plan_of('EXECUTE pt_param(3)') AS other;
EXECUTE pt_pruned('2024-05-01');
EXECUTE pt_param(3);
RESET plan_cache_mode;

-- forced parallel plans: the workers split the superblocks of all the partitions
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT n, (same(q)).* FROM pt_q WHERE n IN (1, 2, 5) ORDER BY n;
SET max_parallel_workers_per_gather = 0;

-- a partition attached and detached between statements: the plan lists the partitions the statement was planned with (the attached table
-- gets the parent's index)
CREATE TABLE pt_n (LIKE pt INCLUDING DEFAULTS) WITH (autovacuum_enabled = false);
INSERT INTO pt_n SELECT 100000 + i, i % 11, DATE '2025-03-01' + i % 20, i, i / 10.0, 'red', '{"k": 1}' FROM generate_series(1, 500) i;
ALTER TABLE pt DETACH PARTITION pt_rest;
ALTER TABLE pt ATTACH PARTITION pt_n FOR VALUES FROM ('2025-03-01') TO ('2025-04-01');
SELECT * FROM same('SELECT g, count(*) AS n, sum(v) AS s FROM pt GROUP BY g');
ALTER TABLE pt DETACH PARTITION pt_n;
ALTER TABLE pt ATTACH PARTITION pt_rest DEFAULT;
SELECT * FROM same('SELECT g, count(*) AS n, sum(v) AS s FROM pt GROUP BY g');

-- the index per partition: every partition needs one of the same layout (a partition without it, or with other paths, keeps the normal plan)
CREATE TABLE pn (id int, g int, d date NOT NULL, v int, doc jsonb) PARTITION BY RANGE (d);
CREATE TABLE pn_a PARTITION OF pn FOR VALUES FROM ('2024-01-01') TO ('2024-07-01') WITH (autovacuum_enabled = false);
CREATE TABLE pn_b PARTITION OF pn FOR VALUES FROM ('2024-07-01') TO ('2025-01-01') WITH (autovacuum_enabled = false);
INSERT INTO pn SELECT i, i % 7, DATE '2024-01-01' + i % 360, i % 100, jsonb_build_object('k', i % 5) FROM generate_series(1, 3000) i;
CREATE INDEX pn_a_col ON pn_a USING sift_col (id, g, d, v, doc) WITH (paths = 'doc->k int4');
ANALYZE pn;
SELECT * FROM same('SELECT g, count(*) AS n FROM pn GROUP BY g');
SELECT * FROM same('SELECT g, count(*) AS n FROM pn WHERE d < DATE ''2024-07-01'' GROUP BY g');
CREATE INDEX pn_b_col ON pn_b USING sift_col (id, g, d, v, doc) WITH (paths = 'doc->k int8');
SELECT * FROM same('SELECT g, count(*) AS n FROM pn GROUP BY g');
DROP INDEX pn_b_col;
CREATE INDEX pn_b_col ON pn_b USING sift_col (id, g, v, d, doc) WITH (paths = 'doc->k int4');
SELECT * FROM same('SELECT g, count(*) AS n FROM pn GROUP BY g');
DROP INDEX pn_b_col;
CREATE INDEX pn_b_col ON pn_b USING sift_col (id, g, d, v, doc) WITH (paths = 'doc->k int4');
SELECT * FROM same('SELECT g, count(*) AS n, sum(v) AS s FROM pn GROUP BY g');

-- a plan prepared before a partition lost its index is replanned
SET plan_cache_mode = force_custom_plan;
PREPARE pn_sum AS SELECT g, count(*) AS n FROM pn GROUP BY g;
SELECT plan_of('EXECUTE pn_sum');
DROP INDEX pn_b_col;
SELECT plan_of('EXECUTE pn_sum');
EXECUTE pn_sum;
RESET plan_cache_mode;

-- rows into the partitions through the parent, moved between partitions, deleted, then a partition emptied
SELECT pt_fill(6001, 7500);
UPDATE pt SET d = d + 130, v = v + 1 WHERE id % 17 = 0;
DELETE FROM pt WHERE id % 23 = 0;
SELECT n, (same(q)).* FROM pt_q WHERE n IN (1, 2, 5, 6) ORDER BY n;
TRUNCATE pt_b;
SELECT n, (same(q)).* FROM pt_q WHERE n IN (1, 2, 6) ORDER BY n;
VACUUM pt;
SELECT n, (same(q)).* FROM pt_q WHERE n IN (1, 5) ORDER BY n;

-- a partition's own DML stays visible to the parent in the same transaction
BEGIN;
INSERT INTO pt_a VALUES (900001, 1, '2024-02-02', 5, 1.5, 'red', '{"k": 2}');
SELECT * FROM same('SELECT count(*) AS n, sum(v) AS s FROM pt');
ROLLBACK;

-- privileges are those of the partitioned table, as in PostgreSQL
CREATE ROLE pt_reader;
GRANT USAGE ON SCHEMA public TO pt_reader;
GRANT SELECT ON pt TO pt_reader;
SET ROLE pt_reader;
SELECT plan_of('SELECT g, count(*) AS n FROM pt GROUP BY g');
SELECT count(*) FROM pt;
SELECT count(*) FROM pt_a;
RESET ROLE;
-- the partitioned table's row security applies to the scan too
ALTER TABLE pt ENABLE ROW LEVEL SECURITY;
CREATE POLICY pt_g ON pt FOR SELECT TO pt_reader USING (g < 3);
SET ROLE pt_reader;
SELECT plan_of('SELECT g, count(*) AS n FROM pt GROUP BY g');
SELECT g, count(*) AS n FROM pt GROUP BY g ORDER BY g;
RESET ROLE;
DROP POLICY pt_g ON pt;
ALTER TABLE pt DISABLE ROW LEVEL SECURITY;
REVOKE ALL ON pt FROM pt_reader;
REVOKE USAGE ON SCHEMA public FROM pt_reader;
DROP ROLE pt_reader;

-- inheritance parents and a partition that is another table's child stay on the normal plan
CREATE TABLE ih (id int, g int) WITH (autovacuum_enabled = false);
CREATE TABLE ih_a (CHECK (g < 5)) INHERITS (ih);
INSERT INTO ih_a SELECT i, i % 5 FROM generate_series(1, 500) i;
CREATE INDEX ih_a_col ON ih_a USING sift_col (id, g);
SELECT * FROM same('SELECT g, count(*) AS n FROM ih GROUP BY g');
SELECT * FROM same('SELECT g, count(*) AS n FROM ih_a GROUP BY g');

DROP FUNCTION same(text);
DROP FUNCTION plan_of(text);
DROP FUNCTION pt_fill(int, int);
DROP TABLE pt, pn, pt_n, pt_q, ih_a, ih;
DROP EXTENSION pg_sift;
