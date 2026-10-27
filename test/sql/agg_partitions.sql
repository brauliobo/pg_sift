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
CREATE INDEX pt_col ON pt USING sift_col (id, g, d, v, amount, s, doc) WITH (rollup_dims = 'off', paths = 'doc->k int4', cube_minmax = false, superunit = 0);
VACUUM (FREEZE, ANALYZE) pt;
-- the maintenance view lists the indexes of the partitions, not the parent's, which has no storage to read
SELECT NOT EXISTS (SELECT 1 FROM sift.workers WHERE index = 'pt_col'::regclass) AS parent_skipped;
SELECT count(*) > 0 AS leaves_listed FROM sift.workers w JOIN pg_inherits h ON h.inhrelid = w.index WHERE h.inhparent = 'pt_col'::regclass;

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

-- the partitions a parameter or a stable function prunes when the statement runs are dropped when the scan starts: their units are not read.
-- A prepared statement with a generic plan against the literal query: the same rows, and as many partitions read as the normal plan scans
-- (a condition that no row can meet is planned as an empty result, which PostgreSQL 19 shows with a scan: no count for those)
CREATE FUNCTION same_exec(stmt text, ref text) RETURNS TABLE (scan text, partitions_read int, normal_scans int, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
DECLARE l text; listed int := 0; removed int := 0;
BEGIN
	scan := 'normal'; normal_scans := 0;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || stmt LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN scan := 'SiftAgg'; END IF;
		IF l LIKE '%Partitions:%' THEN listed := array_length(string_to_array(l, ','), 1); END IF;
		IF l LIKE '%Partitions Removed:%' THEN removed := trim(split_part(l, ':', 2))::int; END IF;
	END LOOP;
	partitions_read := CASE WHEN scan = 'SiftAgg' THEN listed - removed END;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', stmt);
	SET LOCAL sift.enable_agg = off;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || ref LOOP
		IF l LIKE '%Scan on p%' THEN normal_scans := normal_scans + 1; END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', ref);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

SET plan_cache_mode = force_generic_plan;
PREPARE pt_pruned (date) AS SELECT g, count(*) AS n FROM pt WHERE d >= $1 AND d < DATE '2024-07-01' GROUP BY g;
PREPARE pt_param (int) AS SELECT g, count(*) AS n FROM pt WHERE g = $1 GROUP BY g;
PREPARE pt_in (date[]) AS SELECT g, count(*) AS n FROM pt WHERE d = ANY ($1) GROUP BY g;
PREPARE pt_hash (int) AS SELECT g, count(*) AS n FROM pt WHERE id = $1 GROUP BY g;
SELECT plan_of('EXECUTE pt_pruned(DATE ''2024-05-01'')') AS pruned, plan_of('EXECUTE pt_param(3)') AS other,
       plan_of('EXECUTE pt_in(ARRAY[DATE ''2024-02-02'', DATE ''2024-11-08''])') AS in_list, plan_of('EXECUTE pt_hash(7)') AS hash_key;
SELECT * FROM same_exec('EXECUTE pt_pruned(DATE ''2024-05-01'')', 'SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2024-05-01'' AND d < DATE ''2024-07-01'' GROUP BY g');
SELECT * FROM same_exec('EXECUTE pt_pruned(DATE ''2024-01-02'')', 'SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2024-01-02'' AND d < DATE ''2024-07-01'' GROUP BY g');
SELECT scan, partitions_read, result_rows, vs_normal_plan FROM same_exec('EXECUTE pt_pruned(DATE ''2025-02-01'')', 'SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2025-02-01'' AND d < DATE ''2024-07-01'' GROUP BY g');
SELECT scan, partitions_read, result_rows, vs_normal_plan FROM same_exec('EXECUTE pt_pruned(NULL)', 'SELECT g, count(*) AS n FROM pt WHERE d >= NULL AND d < DATE ''2024-07-01'' GROUP BY g');
SELECT * FROM same_exec('EXECUTE pt_in(ARRAY[DATE ''2024-02-02'', DATE ''2024-11-08''])', 'SELECT g, count(*) AS n FROM pt WHERE d = ANY (ARRAY[DATE ''2024-02-02'', DATE ''2024-11-08'']) GROUP BY g');
SELECT * FROM same_exec('EXECUTE pt_in(ARRAY[DATE ''2023-02-01''])', 'SELECT g, count(*) AS n FROM pt WHERE d = ANY (ARRAY[DATE ''2023-02-01'']) GROUP BY g');
SELECT scan, partitions_read, result_rows, vs_normal_plan FROM same_exec('EXECUTE pt_in(''{}'')', 'SELECT g, count(*) AS n FROM pt WHERE d = ANY (''{}''::date[]) GROUP BY g');
-- the bounds the pruning of a plan read are not kept when a partition moves to another one: k = 11 was out of the first partition
CREATE TABLE pb (k int NOT NULL, v int) PARTITION BY RANGE (k);
CREATE TABLE pb_a PARTITION OF pb FOR VALUES FROM (0) TO (10) WITH (autovacuum_enabled = false);
CREATE TABLE pb_b PARTITION OF pb FOR VALUES FROM (10) TO (20) WITH (autovacuum_enabled = false);
INSERT INTO pb SELECT i % 20, i FROM generate_series(1, 400) i;
CREATE INDEX pb_col ON pb USING sift_col (k, v) WITH (rollup_dims = 'off', cube_minmax = false, superunit = 0);
VACUUM (FREEZE, ANALYZE) pb;
PREPARE pb_k (int) AS SELECT count(*) AS n, sum(v) AS s FROM pb WHERE k = $1;
SELECT * FROM same_exec('EXECUTE pb_k(11)', 'SELECT count(*) AS n, sum(v) AS s FROM pb WHERE k = 11');
ALTER TABLE pb DETACH PARTITION pb_a;
ALTER TABLE pb DETACH PARTITION pb_b;
INSERT INTO pb_a SELECT * FROM pb_b WHERE k < 12;
DELETE FROM pb_b WHERE k < 12;
ALTER TABLE pb ATTACH PARTITION pb_a FOR VALUES FROM (0) TO (12);
ALTER TABLE pb ATTACH PARTITION pb_b FOR VALUES FROM (12) TO (20);
VACUUM (FREEZE, ANALYZE) pb;
SELECT * FROM same_exec('EXECUTE pb_k(11)', 'SELECT count(*) AS n, sum(v) AS s FROM pb WHERE k = 11');
SELECT * FROM same_exec('EXECUTE pb_k(13)', 'SELECT count(*) AS n, sum(v) AS s FROM pb WHERE k = 13');
DEALLOCATE pb_k;
DROP TABLE pb;
-- what the planner and the scan keep of a partition (its index's layout, statistics and visibility, its bound) is read again when the table changes
-- between two executions of one prepared statement: rows written, a partition attached, one detached, an index rebuilt, a bound moved
CREATE TABLE pf (k int NOT NULL, g int, v int) PARTITION BY RANGE (k);
CREATE TABLE pf_a PARTITION OF pf FOR VALUES FROM (0) TO (10) WITH (autovacuum_enabled = false);
CREATE TABLE pf_b PARTITION OF pf FOR VALUES FROM (10) TO (20) WITH (autovacuum_enabled = false);
CREATE TABLE pf_c (k int NOT NULL, g int, v int) WITH (autovacuum_enabled = false);
INSERT INTO pf SELECT i % 20, i % 3, i FROM generate_series(1, 600) i;
INSERT INTO pf_c SELECT 20 + i % 10, i % 3, i FROM generate_series(1, 300) i;
CREATE INDEX pf_col ON pf USING sift_col (k, g, v) WITH (rollup_dims = 'off', cube_minmax = false, superunit = 0);
CREATE INDEX pf_c_col ON pf_c USING sift_col (k, g, v) WITH (rollup_dims = 'off', cube_minmax = false, superunit = 0);
VACUUM (FREEZE, ANALYZE) pf, pf_c;
PREPARE pf_q (int, int) AS SELECT g, count(*) AS n, sum(v) AS s FROM pf WHERE k >= $1 AND k < $2 GROUP BY g;
SELECT * FROM same_exec('EXECUTE pf_q(5, 15)', 'SELECT g, count(*) AS n, sum(v) AS s FROM pf WHERE k >= 5 AND k < 15 GROUP BY g');
-- rows written: the visibility and the write buffers move
UPDATE pf SET v = v + 1 WHERE k = 12;
INSERT INTO pf SELECT 3 + i % 9, i % 3, i FROM generate_series(1, 50) i;
SELECT * FROM same_exec('EXECUTE pf_q(5, 15)', 'SELECT g, count(*) AS n, sum(v) AS s FROM pf WHERE k >= 5 AND k < 15 GROUP BY g');
VACUUM pf;
SELECT * FROM same_exec('EXECUTE pf_q(5, 15)', 'SELECT g, count(*) AS n, sum(v) AS s FROM pf WHERE k >= 5 AND k < 15 GROUP BY g');
-- a partition attached, then detached
ALTER TABLE pf ATTACH PARTITION pf_c FOR VALUES FROM (20) TO (30);
SELECT * FROM same_exec('EXECUTE pf_q(5, 25)', 'SELECT g, count(*) AS n, sum(v) AS s FROM pf WHERE k >= 5 AND k < 25 GROUP BY g');
ALTER TABLE pf DETACH PARTITION pf_c;
SELECT * FROM same_exec('EXECUTE pf_q(5, 25)', 'SELECT g, count(*) AS n, sum(v) AS s FROM pf WHERE k >= 5 AND k < 25 GROUP BY g');
ALTER TABLE pf ATTACH PARTITION pf_c FOR VALUES FROM (20) TO (30);
-- the index of a partition rebuilt (a new file, the directory's versions start over), and its table emptied and filled again
REINDEX INDEX pf_c_col;
SELECT * FROM same_exec('EXECUTE pf_q(5, 25)', 'SELECT g, count(*) AS n, sum(v) AS s FROM pf WHERE k >= 5 AND k < 25 GROUP BY g');
TRUNCATE pf_c;
INSERT INTO pf_c SELECT 20 + i % 5, i % 3, i FROM generate_series(1, 40) i;
SELECT * FROM same_exec('EXECUTE pf_q(5, 25)', 'SELECT g, count(*) AS n, sum(v) AS s FROM pf WHERE k >= 5 AND k < 25 GROUP BY g');
-- a bound moved: the rows of pf_b below 15 now belong to the partition before it
ALTER TABLE pf DETACH PARTITION pf_a;
ALTER TABLE pf DETACH PARTITION pf_b;
INSERT INTO pf_a SELECT * FROM pf_b WHERE k < 15;
DELETE FROM pf_b WHERE k < 15;
ALTER TABLE pf ATTACH PARTITION pf_a FOR VALUES FROM (0) TO (15);
ALTER TABLE pf ATTACH PARTITION pf_b FOR VALUES FROM (15) TO (20);
SELECT * FROM same_exec('EXECUTE pf_q(5, 25)', 'SELECT g, count(*) AS n, sum(v) AS s FROM pf WHERE k >= 5 AND k < 25 GROUP BY g');
SELECT * FROM same_exec('EXECUTE pf_q(12, 14)', 'SELECT g, count(*) AS n, sum(v) AS s FROM pf WHERE k >= 12 AND k < 14 GROUP BY g');
SELECT * FROM same_exec('EXECUTE pf_q(16, 18)', 'SELECT g, count(*) AS n, sum(v) AS s FROM pf WHERE k >= 16 AND k < 18 GROUP BY g');
DEALLOCATE pf_q;
DROP TABLE pf, pf_c;
SELECT * FROM same_exec('EXECUTE pt_param(3)', 'SELECT g, count(*) AS n FROM pt WHERE g = 3 GROUP BY g');
SELECT * FROM same_exec('EXECUTE pt_hash(7)', 'SELECT g, count(*) AS n FROM pt WHERE id = 7 GROUP BY g');
RESET plan_cache_mode;

-- a stable function decides when the statement starts (current_date - current_date is 0, not folded at plan time)
SELECT * FROM same_exec('SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2024-05-01'' + (current_date - current_date) AND d < DATE ''2024-07-01'' + (current_date - current_date) GROUP BY g',
                        'SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2024-05-01'' AND d < DATE ''2024-07-01'' GROUP BY g');
SELECT * FROM same_exec('SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2024-11-15'' + (current_date - current_date) GROUP BY g',
                        'SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2024-11-15'' GROUP BY g');
SELECT * FROM same_exec('SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2030-01-01'' + (current_date - current_date) GROUP BY g',
                        'SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2030-01-01'' GROUP BY g');

-- forced parallel plans: the workers split the superblocks of all the partitions
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT n, (same(q)).* FROM pt_q WHERE n IN (1, 2, 5) ORDER BY n;
-- the workers take the superblocks of the partitions the leader kept
SET plan_cache_mode = force_generic_plan;
PREPARE pt_pruned_par (date) AS SELECT g, count(*) AS n FROM pt WHERE d >= $1 AND d < DATE '2024-07-01' GROUP BY g;
PREPARE pt_in_par (date[]) AS SELECT g, count(*) AS n FROM pt WHERE d = ANY ($1) GROUP BY g;
SELECT plan_of('EXECUTE pt_pruned_par(DATE ''2024-05-01'')'), plan_of('EXECUTE pt_in_par(ARRAY[DATE ''2024-02-02''])');
SELECT * FROM same_exec('EXECUTE pt_pruned_par(DATE ''2024-05-01'')', 'SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2024-05-01'' AND d < DATE ''2024-07-01'' GROUP BY g');
SELECT * FROM same_exec('EXECUTE pt_in_par(ARRAY[DATE ''2024-02-02'', DATE ''2024-11-08''])', 'SELECT g, count(*) AS n FROM pt WHERE d = ANY (ARRAY[DATE ''2024-02-02'', DATE ''2024-11-08'']) GROUP BY g');
RESET plan_cache_mode;
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
CREATE INDEX pn_a_col ON pn_a USING sift_col (id, g, d, v, doc) WITH (rollup_dims = 'off', paths = 'doc->k int4', cube_minmax = false, superunit = 0);
ANALYZE pn;
SELECT * FROM same('SELECT g, count(*) AS n FROM pn GROUP BY g');
SELECT * FROM same('SELECT g, count(*) AS n FROM pn WHERE d < DATE ''2024-07-01'' GROUP BY g');
CREATE INDEX pn_b_col ON pn_b USING sift_col (id, g, d, v, doc) WITH (rollup_dims = 'off', paths = 'doc->k int8', cube_minmax = false, superunit = 0);
SELECT * FROM same('SELECT g, count(*) AS n FROM pn GROUP BY g');
DROP INDEX pn_b_col;
CREATE INDEX pn_b_col ON pn_b USING sift_col (id, g, v, d, doc) WITH (rollup_dims = 'off', paths = 'doc->k int4', cube_minmax = false, superunit = 0);
SELECT * FROM same('SELECT g, count(*) AS n FROM pn GROUP BY g');
DROP INDEX pn_b_col;
CREATE INDEX pn_b_col ON pn_b USING sift_col (id, g, d, v, doc) WITH (rollup_dims = 'off', paths = 'doc->k int4', cube_minmax = false, superunit = 0);
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

-- the same after rows moved between partitions and one was emptied
SET plan_cache_mode = force_generic_plan;
SELECT * FROM same_exec('EXECUTE pt_pruned(DATE ''2024-05-01'')', 'SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2024-05-01'' AND d < DATE ''2024-07-01'' GROUP BY g');
SELECT * FROM same_exec('EXECUTE pt_in(ARRAY[DATE ''2024-02-02'', DATE ''2024-11-08''])', 'SELECT g, count(*) AS n FROM pt WHERE d = ANY (ARRAY[DATE ''2024-02-02'', DATE ''2024-11-08'']) GROUP BY g');
RESET plan_cache_mode;

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
CREATE INDEX ih_a_col ON ih_a USING sift_col (id, g) WITH (rollup_dims = 'off', cube_minmax = false, superunit = 0);
SELECT * FROM same('SELECT g, count(*) AS n FROM ih GROUP BY g');
SELECT * FROM same('SELECT g, count(*) AS n FROM ih_a GROUP BY g');

-- a text key split by list: the zones cannot refuse it, so the partitions the parameter drops are the whole saving
CREATE TABLE pl (id int, k text, v int) PARTITION BY LIST (k);
CREATE TABLE pl_a PARTITION OF pl FOR VALUES IN ('cat_a', 'cat_b') WITH (autovacuum_enabled = false);
CREATE TABLE pl_b PARTITION OF pl FOR VALUES IN ('cat_c') WITH (autovacuum_enabled = false);
CREATE TABLE pl_c PARTITION OF pl FOR VALUES IN ('cat_d') WITH (autovacuum_enabled = false);
CREATE TABLE pl_rest PARTITION OF pl DEFAULT WITH (autovacuum_enabled = false);
INSERT INTO pl SELECT i, (ARRAY['cat_a', 'cat_b', 'cat_c', 'cat_d', 'cat_e'])[1 + i % 5], i % 100 FROM generate_series(1, 4000) i;
CREATE INDEX pl_col ON pl USING sift_col (id, k, v) WITH (rollup_dims = 'off', cube_minmax = false, superunit = 0);
VACUUM (FREEZE, ANALYZE) pl;
SET plan_cache_mode = force_generic_plan;
PREPARE pl_k (text) AS SELECT k, count(*) AS n, sum(v) AS s FROM pl WHERE k = $1 GROUP BY k;
PREPARE pl_ks (text[]) AS SELECT k, count(*) AS n, sum(v) AS s FROM pl WHERE k = ANY ($1) GROUP BY k;
SELECT * FROM same_exec('EXECUTE pl_k(''cat_c'')', 'SELECT k, count(*) AS n, sum(v) AS s FROM pl WHERE k = ''cat_c'' GROUP BY k');
SELECT * FROM same_exec('EXECUTE pl_k(''cat_b'')', 'SELECT k, count(*) AS n, sum(v) AS s FROM pl WHERE k = ''cat_b'' GROUP BY k');
SELECT * FROM same_exec('EXECUTE pl_k(''cat_e'')', 'SELECT k, count(*) AS n, sum(v) AS s FROM pl WHERE k = ''cat_e'' GROUP BY k');
SELECT * FROM same_exec('EXECUTE pl_k(''cat_z'')', 'SELECT k, count(*) AS n, sum(v) AS s FROM pl WHERE k = ''cat_z'' GROUP BY k');
SELECT * FROM same_exec('EXECUTE pl_ks(ARRAY[''cat_a'', ''cat_d''])', 'SELECT k, count(*) AS n, sum(v) AS s FROM pl WHERE k = ANY (ARRAY[''cat_a'', ''cat_d'']) GROUP BY k');
-- a partition attached after the plan was made: the plan is made again with it
CREATE TABLE pl_n (LIKE pl INCLUDING DEFAULTS) WITH (autovacuum_enabled = false);
INSERT INTO pl_n SELECT 100000 + i, 'cat_z', i FROM generate_series(1, 300) i;
ALTER TABLE pl ATTACH PARTITION pl_n FOR VALUES IN ('cat_z');
SELECT * FROM same_exec('EXECUTE pl_k(''cat_z'')', 'SELECT k, count(*) AS n, sum(v) AS s FROM pl WHERE k = ''cat_z'' GROUP BY k');
ALTER TABLE pl DETACH PARTITION pl_n;
SELECT * FROM same_exec('EXECUTE pl_k(''cat_z'')', 'SELECT k, count(*) AS n, sum(v) AS s FROM pl WHERE k = ''cat_z'' GROUP BY k');
RESET plan_cache_mode;

-- a table split by hash: the remainders of the values a parameter, a stable function or a list leaves the key decide the partitions read (the key's
-- hash function and each partition's modulus and remainder, as PostgreSQL computes them when the statement starts)
CREATE TABLE ph (id int, k int, v int) PARTITION BY HASH (k);
SELECT format('CREATE TABLE ph_%s PARTITION OF ph FOR VALUES WITH (MODULUS 8, REMAINDER %s) WITH (autovacuum_enabled = false)', i, i) FROM generate_series(0, 7) i \gexec
INSERT INTO ph SELECT i, CASE WHEN i % 40 = 0 THEN NULL ELSE i % 300 END, i % 100 FROM generate_series(1, 4000) i;
CREATE INDEX ph_col ON ph USING sift_col (id, k, v) WITH (rollup_dims = 'off', cube_minmax = false, superunit = 0);
VACUUM (FREEZE, ANALYZE) ph;
-- two key columns (a remainder needs both), and a key that is an expression
CREATE TABLE phm (a int, b text, v int) PARTITION BY HASH (a, b);
SELECT format('CREATE TABLE phm_%s PARTITION OF phm FOR VALUES WITH (MODULUS 4, REMAINDER %s) WITH (autovacuum_enabled = false)', i, i) FROM generate_series(0, 3) i \gexec
INSERT INTO phm SELECT i % 50, 'cat_' || chr(97 + i % 6), i % 100 FROM generate_series(1, 3000) i;
CREATE INDEX phm_col ON phm USING sift_col (a, b, v) WITH (rollup_dims = 'off', cube_minmax = false, superunit = 0);
VACUUM (FREEZE, ANALYZE) phm;
CREATE TABLE phx (k int, v int) PARTITION BY HASH ((k + 1));
SELECT format('CREATE TABLE phx_%s PARTITION OF phx FOR VALUES WITH (MODULUS 4, REMAINDER %s) WITH (autovacuum_enabled = false)', i, i) FROM generate_series(0, 3) i \gexec
INSERT INTO phx SELECT i % 200, i % 100 FROM generate_series(1, 2000) i;
CREATE INDEX phx_col ON phx USING sift_col (k, v) WITH (rollup_dims = 'off', cube_minmax = false, superunit = 0);
VACUUM (FREEZE, ANALYZE) phx;
SET plan_cache_mode = force_generic_plan;
PREPARE ph_k (int) AS SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = $1;
PREPARE ph_ks (int[]) AS SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = ANY ($1);
PREPARE ph_big (bigint) AS SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = $1;
PREPARE ph_gt (int) AS SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k > $1;
PREPARE ph_and (int, int) AS SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = $1 AND v < $2;
PREPARE phm_ab (int, text) AS SELECT count(*) AS n, sum(v) AS s FROM phm WHERE a = $1 AND b = $2;
PREPARE phm_a (int) AS SELECT count(*) AS n, sum(v) AS s FROM phm WHERE a = $1;
PREPARE phm_abs (int[], text) AS SELECT count(*) AS n, sum(v) AS s FROM phm WHERE a = ANY ($1) AND b = $2;
PREPARE phx_k (int) AS SELECT count(*) AS n, sum(v) AS s FROM phx WHERE k = $1;
PREPARE phx_e (int) AS SELECT count(*) AS n, sum(v) AS s FROM phx WHERE k + 1 = $1;
SELECT plan_of('EXECUTE ph_k(7)') AS equality, plan_of('EXECUTE ph_ks(ARRAY[7, 8])') AS any_list, plan_of('EXECUTE ph_big(7)') AS other_type,
       plan_of('EXECUTE ph_gt(7)') AS range_test, plan_of('EXECUTE ph_and(7, 50)') AS with_filter, plan_of('EXECUTE phx_e(8)') AS expression_key;
-- a value, a list (duplicates, a NULL element, none, a NULL array), a NULL value, a value no row has, the rows whose key is NULL
SELECT * FROM same_exec('EXECUTE ph_k(7)', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = 7');
SELECT * FROM same_exec('EXECUTE ph_k(299)', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = 299');
SELECT * FROM same_exec('EXECUTE ph_k(100000)', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = 100000');
SELECT scan, partitions_read, result_rows, vs_normal_plan FROM same_exec('EXECUTE ph_k(NULL)', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = NULL');
SELECT * FROM same_exec('EXECUTE ph_ks(ARRAY[7, 8, 7])', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = ANY (ARRAY[7, 8, 7])');
SELECT * FROM same_exec('EXECUTE ph_ks(ARRAY[7, NULL, 12])', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = ANY (ARRAY[7, NULL, 12])');
SELECT * FROM same_exec('EXECUTE ph_ks(''{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20}'')', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = ANY (''{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20}''::int[])');
SELECT scan, partitions_read, result_rows, vs_normal_plan FROM same_exec('EXECUTE ph_ks(''{}'')', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = ANY (''{}''::int[])');
SELECT scan, partitions_read, result_rows, vs_normal_plan FROM same_exec('EXECUTE ph_ks(NULL)', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = ANY (NULL::int[])');
SELECT * FROM same_exec('EXECUTE ph_and(7, 50)', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = 7 AND v < 50');
SELECT * FROM same('SELECT count(*) AS n FROM ph WHERE k IS NULL');
-- a stable function decides when the statement starts
SELECT * FROM same_exec('SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = 7 + (current_date - current_date)', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = 7');
SELECT * FROM same_exec('SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k IN (7 + (current_date - current_date), 8, 9)', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k IN (7, 8, 9)');
-- two key columns: both are needed for a remainder
SELECT plan_of('EXECUTE phm_ab(3, ''cat_b'')') AS both, plan_of('EXECUTE phm_a(3)') AS one, plan_of('EXECUTE phm_abs(ARRAY[3, 4, 5], ''cat_b'')') AS list_and_value;
SELECT * FROM same_exec('EXECUTE phm_ab(3, ''cat_b'')', 'SELECT count(*) AS n, sum(v) AS s FROM phm WHERE a = 3 AND b = ''cat_b''');
SELECT * FROM same_exec('EXECUTE phm_ab(3, ''cat_z'')', 'SELECT count(*) AS n, sum(v) AS s FROM phm WHERE a = 3 AND b = ''cat_z''');
SELECT * FROM same_exec('EXECUTE phm_a(3)', 'SELECT count(*) AS n, sum(v) AS s FROM phm WHERE a = 3');
SELECT * FROM same_exec('EXECUTE phm_abs(ARRAY[3, 4, 5], ''cat_b'')', 'SELECT count(*) AS n, sum(v) AS s FROM phm WHERE a = ANY (ARRAY[3, 4, 5]) AND b = ''cat_b''');
-- a key that is an expression is PostgreSQL's to prune; a condition on the plain column reads every partition
SELECT * FROM same_exec('EXECUTE phx_k(7)', 'SELECT count(*) AS n, sum(v) AS s FROM phx WHERE k = 7');
SELECT scan, result_rows, vs_normal_plan FROM same_exec('EXECUTE phx_e(8)', 'SELECT count(*) AS n, sum(v) AS s FROM phx WHERE k + 1 = 8');
-- another operator or another type of the operand keeps the normal plan (which prunes as PostgreSQL does)
SELECT scan, result_rows, vs_normal_plan FROM same_exec('EXECUTE ph_gt(290)', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k > 290');
SELECT scan, result_rows, vs_normal_plan FROM same_exec('EXECUTE ph_big(7)', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = 7');
-- a hash level under a range level, beside the default partition: both decide
PREPARE pt_dh (date, int) AS SELECT g, count(*) AS n FROM pt WHERE d >= $1 AND d < DATE '2024-10-01' AND id = $2 GROUP BY g;
SELECT plan_of('EXECUTE pt_dh(DATE ''2024-07-02'', 31)');
SELECT * FROM same_exec('EXECUTE pt_dh(DATE ''2024-07-02'', 31)', 'SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2024-07-02'' AND d < DATE ''2024-10-01'' AND id = 31 GROUP BY g');
SELECT * FROM same_exec('EXECUTE pt_dh(DATE ''2024-07-02'', 32)', 'SELECT g, count(*) AS n FROM pt WHERE d >= DATE ''2024-07-02'' AND d < DATE ''2024-10-01'' AND id = 32 GROUP BY g');
-- the workers take the superblocks of the partitions the leader kept
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT plan_of('EXECUTE ph_ks(ARRAY[7, 8])');
SELECT * FROM same_exec('EXECUTE ph_ks(ARRAY[7, 8])', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = ANY (ARRAY[7, 8])');
SELECT * FROM same_exec('EXECUTE ph_k(7)', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = 7');
SET max_parallel_workers_per_gather = 0;
-- rows moved to other partitions and deleted, then the same
UPDATE ph SET k = k + 1 WHERE id % 5 = 0;
DELETE FROM ph WHERE id % 11 = 0;
SELECT * FROM same_exec('EXECUTE ph_ks(ARRAY[7, 8])', 'SELECT count(*) AS n, sum(v) AS s FROM ph WHERE k = ANY (ARRAY[7, 8])');
RESET plan_cache_mode;

-- the keys of an inner or semi join with a table decide the partitions of the join column: the scan loads the keys when it starts (the join key filter)
-- and does not read the partitions that none of them can be in (range and list bounds by the keys, hash by their remainders, the default partition
-- when the bounds of the others refute it)
CREATE TABLE pt_days (d date);
CREATE TABLE pt_ids (id int, w int);
CREATE TABLE pk (id int, k int, v int) PARTITION BY LIST (k);
CREATE TABLE pk_a PARTITION OF pk FOR VALUES IN (1, 2, 3) WITH (autovacuum_enabled = false);
CREATE TABLE pk_b PARTITION OF pk FOR VALUES IN (4, 5, 6) WITH (autovacuum_enabled = false);
CREATE TABLE pk_c PARTITION OF pk FOR VALUES IN (7, 8, 9) WITH (autovacuum_enabled = false);
CREATE TABLE pk_n PARTITION OF pk FOR VALUES IN (NULL) WITH (autovacuum_enabled = false);
CREATE TABLE pk_rest PARTITION OF pk DEFAULT WITH (autovacuum_enabled = false);
INSERT INTO pk SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 12 END, i % 100 FROM generate_series(1, 3000) i;
CREATE INDEX pk_col ON pk USING sift_col (id, k, v) WITH (rollup_dims = 'off', cube_minmax = false, superunit = 0);
VACUUM (FREEZE, ANALYZE) pk;
CREATE TABLE pk_keys (k int, kind text);
INSERT INTO pk_keys SELECT i, 'cat_' || chr(97 + i % 3) FROM generate_series(0, 14) i;
-- the columns of a join: the statement, then the partitions read and whether the rows are the normal plan's
CREATE FUNCTION same_join(stmt text) RETURNS TABLE (scan text, partitions_read int, key_filters int, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
DECLARE l text; listed int := 0; removed int := 0;
BEGIN
	scan := 'normal'; key_filters := 0;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || stmt LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN scan := 'SiftAgg'; END IF;
		IF l LIKE '%Partitions:%' THEN listed := array_length(string_to_array(l, ','), 1); END IF;
		IF l LIKE '%Partitions Removed:%' THEN removed := trim(split_part(l, ':', 2))::int; END IF;
		IF l LIKE '%Join Key Filters:%' THEN key_filters := trim(split_part(l, ':', 2))::int; END IF;
	END LOOP;
	partitions_read := CASE WHEN scan = 'SiftAgg' THEN listed - removed END;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', stmt);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', stmt);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;
-- a range: the dates of two partitions; one in the default partition; none; all
INSERT INTO pt_days VALUES ('2024-02-02'), ('2024-11-08');
SELECT * FROM same_join('SELECT p.g, count(*) AS n, sum(p.v) AS s FROM pt p JOIN pt_days x ON x.d = p.d GROUP BY p.g');
INSERT INTO pt_days VALUES ('2023-12-08');
SELECT * FROM same_join('SELECT p.g, count(*) AS n, sum(p.v) AS s FROM pt p JOIN pt_days x ON x.d = p.d GROUP BY p.g');
DELETE FROM pt_days;
SELECT * FROM same_join('SELECT p.g, count(*) AS n, sum(p.v) AS s FROM pt p JOIN pt_days x ON x.d = p.d GROUP BY p.g');
INSERT INTO pt_days SELECT DATE '2023-11-01' + i FROM generate_series(0, 500) i;
SELECT * FROM same_join('SELECT p.g, count(*) AS n, sum(p.v) AS s FROM pt p JOIN pt_days x ON x.d = p.d GROUP BY p.g');
-- a semi join, a key set that excludes the rows of the default partition, a condition on the dimension
DELETE FROM pt_days WHERE d NOT IN (DATE '2024-02-02', DATE '2024-05-03', DATE '2024-07-05');
SELECT * FROM same_join('SELECT p.g, count(*) AS n FROM pt p WHERE p.d IN (SELECT d FROM pt_days) GROUP BY p.g');
SELECT * FROM same_join('SELECT p.g, count(*) AS n FROM pt p WHERE EXISTS (SELECT 1 FROM pt_days x WHERE x.d = p.d) GROUP BY p.g');
SELECT * FROM same_join('SELECT p.g, count(*) AS n FROM pt p JOIN pt_days x ON x.d = p.d WHERE x.d > DATE ''2024-05-03'' GROUP BY p.g');
-- the keys of a hash level under the range level; the partitions of the other levels stay
INSERT INTO pt_ids VALUES (7, 1), (8, 2), (31, 3);
SELECT * FROM same_join('SELECT p.g, count(*) AS n, sum(i.w) AS s FROM pt p JOIN pt_ids i ON i.id = p.id GROUP BY p.g');
DELETE FROM pt_ids WHERE id <> 8;
SELECT * FROM same_join('SELECT p.g, count(*) AS n, sum(i.w) AS s FROM pt p JOIN pt_ids i ON i.id = p.id GROUP BY p.g');
-- a list: the keys name the partitions (a NULL partition and the default partition, which the keys 0, 10 and 11 reach)
SELECT * FROM same_join('SELECT x.kind, count(*) AS n, sum(f.v) AS s FROM pk f JOIN pk_keys x ON x.k = f.k WHERE x.k IN (2, 8) GROUP BY 1');
SELECT * FROM same_join('SELECT x.kind, count(*) AS n, sum(f.v) AS s FROM pk f JOIN pk_keys x ON x.k = f.k WHERE x.k IN (2, 11) GROUP BY 1');
SELECT * FROM same_join('SELECT x.kind, count(*) AS n, sum(f.v) AS s FROM pk f JOIN pk_keys x ON x.k = f.k WHERE x.k IN (0, 4, 5) GROUP BY 1');
SELECT * FROM same_join('SELECT x.kind, count(*) AS n, sum(f.v) AS s FROM pk f JOIN pk_keys x ON x.k = f.k WHERE x.k < 0 GROUP BY 1');
SELECT * FROM same_join('SELECT x.kind, count(*) AS n, sum(f.v) AS s FROM pk f JOIN pk_keys x ON x.k = f.k GROUP BY 1');
SELECT * FROM same_join('SELECT count(*) AS n FROM pk f WHERE f.k IN (SELECT k FROM pk_keys WHERE kind = ''cat_a'' AND k < 5)');
-- a key that is NULL in the dimension matches nothing
INSERT INTO pk_keys VALUES (NULL, 'cat_z');
SELECT * FROM same_join('SELECT x.kind, count(*) AS n FROM pk f JOIN pk_keys x ON x.k = f.k WHERE x.kind IN (''cat_z'', ''cat_b'') AND x.k IS NOT DISTINCT FROM f.k GROUP BY 1');
SELECT * FROM same_join('SELECT x.kind, count(*) AS n FROM pk f JOIN pk_keys x ON x.k = f.k WHERE x.kind = ''cat_z'' GROUP BY 1');
-- the keys of a hash table
CREATE TABLE ph_keys (k int);
INSERT INTO ph_keys VALUES (7), (8);
SELECT * FROM same_join('SELECT count(*) AS n, sum(f.v) AS s FROM ph f JOIN ph_keys x ON x.k = f.k');
SELECT * FROM same_join('SELECT count(*) AS n, sum(f.v) AS s FROM ph f WHERE f.k IN (SELECT k FROM ph_keys)');
DELETE FROM ph_keys;
SELECT * FROM same_join('SELECT count(*) AS n, sum(f.v) AS s FROM ph f JOIN ph_keys x ON x.k = f.k');
INSERT INTO ph_keys SELECT i FROM generate_series(0, 299) i;
SELECT * FROM same_join('SELECT count(*) AS n, sum(f.v) AS s FROM ph f JOIN ph_keys x ON x.k = f.k');
-- the remainders of a hash level are computed once for all its partitions: a few keys read a few of the 8 partitions, and a level with more
-- combinations of values than the limit (lowered to 2) reads every partition, with the same rows; so do keys beyond the exact test of the
-- range and list bounds (keys times partitions, lowered to 2): the range of the keys prunes alone
DELETE FROM ph_keys;
INSERT INTO ph_keys VALUES (1), (2), (3);
SELECT * FROM same_join('SELECT count(*) AS n, sum(f.v) AS s FROM ph f JOIN ph_keys x ON x.k = f.k');
SET sift.test_limits = 'hash_part_combos=2';
SELECT * FROM same_join('SELECT count(*) AS n, sum(f.v) AS s FROM ph f JOIN ph_keys x ON x.k = f.k');
RESET sift.test_limits;
CREATE TABLE pt_days2 (d date);
INSERT INTO pt_days2 VALUES ('2024-02-02'), ('2024-11-08');
SELECT * FROM same_join('SELECT p.g, count(*) AS n, sum(p.v) AS s FROM pt p JOIN pt_days2 x ON x.d = p.d GROUP BY p.g');
SET sift.test_limits = 'part_key_tests=2';
SELECT * FROM same_join('SELECT p.g, count(*) AS n, sum(p.v) AS s FROM pt p JOIN pt_days2 x ON x.d = p.d GROUP BY p.g');
RESET sift.test_limits;
DROP TABLE pt_days2;
-- off: the join key filter off reads every partition, with the rows of the normal plan
SET sift.agg_join_filter = off;
SELECT * FROM same_join('SELECT x.kind, count(*) AS n, sum(f.v) AS s FROM pk f JOIN pk_keys x ON x.k = f.k WHERE x.k IN (2, 8) GROUP BY 1');
RESET sift.agg_join_filter;
-- the workers take the superblocks of the partitions the leader kept
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT * FROM same_join('SELECT x.kind, count(*) AS n, sum(f.v) AS s FROM pk f JOIN pk_keys x ON x.k = f.k WHERE x.k IN (2, 8) GROUP BY 1');
SELECT * FROM same_join('SELECT p.g, count(*) AS n, sum(p.v) AS s FROM pt p JOIN pt_days x ON x.d = p.d GROUP BY p.g');
SET max_parallel_workers_per_gather = 0;
DROP FUNCTION same_join(text);
DROP TABLE pt_days, pt_ids, pk, pk_keys, ph_keys;

-- the build picks a rollup of the rows for each partition from its data (rollup_dims = auto): a partition attached later with too few rows to pick one is read with the others, the rollups are left out
CREATE TABLE pr (id int, g int, d date NOT NULL, v int) PARTITION BY RANGE (d);
CREATE TABLE pr_a PARTITION OF pr FOR VALUES FROM ('2024-01-01') TO ('2024-04-01') WITH (autovacuum_enabled = false);
CREATE TABLE pr_b PARTITION OF pr FOR VALUES FROM ('2024-04-01') TO ('2024-07-01') WITH (autovacuum_enabled = false);
INSERT INTO pr SELECT i, i % 5, DATE '2024-01-01' + i % 180, i FROM generate_series(1, 3000) i;
CREATE INDEX pr_col ON pr USING sift_col (id, g, d, v);
CREATE TABLE pr_n (LIKE pr INCLUDING DEFAULTS) WITH (autovacuum_enabled = false);
INSERT INTO pr_n SELECT 10000 + i, i % 5, DATE '2024-07-01' + i % 90, i FROM generate_series(1, 20) i;
ALTER TABLE pr ATTACH PARTITION pr_n FOR VALUES FROM ('2024-07-01') TO ('2024-10-01');
VACUUM (FREEZE, ANALYZE) pr;
SELECT string_agg(coalesce(c.reloptions::text, 'none'), ' ' ORDER BY c.relname) AS partition_rollups FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_col' AND c.relname LIKE 'pr\_%';
SELECT * FROM same_exec('SELECT g, count(*) AS n, sum(v) AS s FROM pr GROUP BY g', 'SELECT g, count(*) AS n, sum(v) AS s FROM pr GROUP BY g');
DROP TABLE pr;

DROP FUNCTION same_exec(text, text);
DROP FUNCTION same(text);
DROP FUNCTION plan_of(text);
DROP FUNCTION pt_fill(int, int);
DROP TABLE pt, pn, pt_n, pt_q, pl, pl_n, ih_a, ih, ph, phm, phx;
DROP EXTENSION pg_sift;
