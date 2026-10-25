-- NOT IN (SELECT ..) and IN (SELECT ..) under OR or NOT are hashed SubPlans (a semi join is not possible for them). Custom Scan (SiftAgg) asks
-- PostgreSQL's own SubPlan once for every distinct value of the leaf (a lookup table), so its NULL rule of IN and NOT IN is PostgreSQL's
-- (docs/AGG.md, "Uncorrelated sub-selects"). Every statement is run with the pushdown and with sift.enable_agg = off: the rows must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
-- the table is small: page costs that keep the btree and bitmap plans from looking cheap as well
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, tenant_id int, big_id int8, region text, kind text, day date, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE 1 + (i * 7919) % 300 END, 1 + i % 40, i % 50, (ARRAY['north', 'south', 'east', 'west', NULL])[1 + i % 5],
       (ARRAY['a', 'b', 'c'])[1 + i % 3], date '2024-01-01' + i % 90, (i % 500) / 10.0
FROM generate_series(1, 6000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, dim_a_id, tenant_id, big_id, region, kind, day, amount);
CREATE TABLE tenants (id int PRIMARY KEY, kind text);
INSERT INTO tenants SELECT i, (ARRAY['outlet', 'popup', 'flagship'])[1 + i % 3] FROM generate_series(1, 40) i;
-- the sub-selects: values with a duplicate and without NULL, with a NULL, none, text values with a NULL, dates
CREATE TABLE blocked (dim_a_id int);
INSERT INTO blocked SELECT i FROM generate_series(1, 300, 3) i UNION ALL SELECT i FROM generate_series(1, 300, 6) i;
CREATE TABLE blocked_null (dim_a_id int);
INSERT INTO blocked_null SELECT i FROM generate_series(1, 300, 7) i UNION ALL SELECT NULL;
CREATE TABLE nothing (dim_a_id int);
CREATE TABLE regions_off (region text);
INSERT INTO regions_off VALUES ('north'), ('west'), ('west');
CREATE TABLE regions_off_null (region text);
INSERT INTO regions_off_null VALUES ('north'), (NULL);
CREATE TABLE holidays (d date);
INSERT INTO holidays SELECT date '2024-01-01' + i * 7 FROM generate_series(0, 12) i;
VACUUM (FREEZE, ANALYZE) ev;
ANALYZE tenants; ANALYZE blocked; ANALYZE blocked_null; ANALYZE nothing; ANALYZE regions_off; ANALYZE regions_off_null; ANALYZE holidays;

-- the shape of the plan: SiftAgg with the sub-plan under it, or the normal plan; with the workers the plan launched
CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
		IF trim(l) LIKE 'SubPlan %' THEN r := r || ' with ' || trim(l); END IF;
		IF l LIKE '%Workers Launched: _%' AND l NOT LIKE '%Workers Launched: 0%' THEN r := r || ' with workers'; END IF;
	END LOOP;
	RETURN r;
END $$;

-- the plan chosen, without running the query (a statement that raises when run)
CREATE FUNCTION shape_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
	END LOOP;
	RETURN r;
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

-- NOT IN: a sub-select without NULL and with duplicates, one with a NULL (never true: no row), none (true for every row, a NULL value too)
SELECT * FROM same($$SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked) GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n, count(dim_a_id) AS with_entity FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked WHERE dim_a_id IS NOT NULL) GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked_null) GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked_null WHERE dim_a_id IS NOT NULL) GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n, count(dim_a_id) AS with_entity FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM nothing) GROUP BY kind$$);
-- IN under OR (the other arm decides for a NULL), NOT of an IN, NOT IN under OR with a NULL in the sub-select
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM blocked) OR kind = 'c' GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM blocked_null) OR tenant_id < 5 GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE NOT (dim_a_id IN (SELECT dim_a_id FROM blocked_null)) OR tenant_id < 5 GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE NOT (dim_a_id IN (SELECT dim_a_id FROM blocked)) GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked_null) OR tenant_id < 5 GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE (dim_a_id NOT IN (SELECT dim_a_id FROM blocked) AND tenant_id < 20) OR region IN (SELECT region FROM regions_off) GROUP BY kind$$);
-- text, date and expression leaves; a cross-type comparison; a leaf with NULLs against a sub-select with NULLs
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE region NOT IN (SELECT region FROM regions_off) GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE region NOT IN (SELECT region FROM regions_off_null) OR kind = 'a' GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE region IN (SELECT region FROM regions_off) OR kind = 'a' GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE day NOT IN (SELECT d FROM holidays) OR tenant_id = 1 GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE (tenant_id + 1) NOT IN (SELECT id FROM tenants WHERE kind = 'outlet') OR kind = 'b' GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE tenant_id NOT IN (SELECT id::bigint FROM tenants WHERE kind = 'outlet') GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE upper(region) NOT IN (SELECT upper(region) FROM regions_off) OR kind = 'a' GROUP BY kind$$);
-- a sub-select that reads the table the scan reads, with GROUP BY, with LIMIT and a set operation
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM ev WHERE tenant_id < 3 AND dim_a_id IS NOT NULL) GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM ev WHERE dim_a_id IS NOT NULL GROUP BY dim_a_id HAVING count(*) > 20) GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT id FROM tenants ORDER BY id LIMIT 12 OFFSET 2) OR kind = 'b' GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT id FROM tenants UNION SELECT dim_a_id FROM blocked WHERE dim_a_id < 100) GROUP BY kind$$);
-- an array: NOT IN a sub-select that unnests it (a NULL element), and <> ALL of an ARRAY sub-select (an initplan result, not a SubPlan: the lookup table asks it once)
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT unnest(ARRAY[1, 2, 3, 4, 5, 6, 7])) GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT unnest(ARRAY[1, 2, 3, NULL])) OR tenant_id = 1 GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE tenant_id <> ALL (ARRAY(SELECT id FROM tenants WHERE id < 30)) GROUP BY kind$$);
-- a SubPlan in a CASE (a group key) and in a FILTER
SELECT * FROM same($$SELECT CASE WHEN dim_a_id NOT IN (SELECT dim_a_id FROM blocked) THEN 'free' ELSE 'blocked' END AS grp, count(*) AS n FROM ev GROUP BY 1$$);
SELECT * FROM same($$SELECT kind, count(*) FILTER (WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked WHERE dim_a_id IS NOT NULL)) AS free, count(*) FILTER (WHERE region IN (SELECT region FROM regions_off)) AS off, count(*) AS n FROM ev GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, sum(CASE WHEN tenant_id IN (SELECT id FROM tenants WHERE kind = 'popup') THEN amount END) AS popup, count(*) AS n FROM ev GROUP BY kind$$);

-- a sub-select that reads a CTE (the CTE is the plan's, the sub-select its reader)
SELECT * FROM same($$WITH s AS MATERIALIZED (SELECT id FROM tenants WHERE id < 20) SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT id FROM s) OR kind = 'b' GROUP BY kind$$);

-- a column whose range is wide, with the values the statistics did not see (a few rows after ANALYZE): the lookup table cannot be dense, the answer is the same
CREATE TABLE wide (id int PRIMARY KEY, dim_a_id int, kind text) WITH (autovacuum_enabled = false);
INSERT INTO wide SELECT i, 1 + (i * 7919) % 300, (ARRAY['a', 'b', 'c'])[1 + i % 3] FROM generate_series(1, 6000) i;
CREATE INDEX wide_col ON wide USING sift_col (dim_a_id, kind);
VACUUM (FREEZE, ANALYZE) wide;
INSERT INTO wide VALUES (10001, -5, 'a'), (10002, 2147483640, 'b'), (10003, 1000000000, 'c'), (10004, NULL, 'a');
VACUUM (FREEZE) wide;
SELECT * FROM same($$SELECT kind, count(*) AS n FROM wide WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked WHERE dim_a_id IS NOT NULL) GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM wide WHERE dim_a_id IN (SELECT dim_a_id FROM blocked) OR kind = 'c' GROUP BY kind$$);
SELECT * FROM same($$SELECT count(*) AS n FROM wide WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked WHERE dim_a_id IS NOT NULL)$$);

-- the sub-select runs once: the SubPlan of EXPLAIN ANALYZE is under the node and was started once
SELECT plan_of($$SELECT kind, count(*) FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked) GROUP BY kind$$) AS not_in;
SELECT plan_of($$SELECT kind, count(*) FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM blocked) OR kind = 'c' GROUP BY kind$$) AS in_or;
EXPLAIN (COSTS OFF) SELECT kind, count(*) FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked) GROUP BY kind;
SET sift.enable_agg = off;
SELECT plan_of($$SELECT kind, count(*) FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked) GROUP BY kind$$) AS pushdown_off;
RESET sift.enable_agg;

-- what keeps the normal plan: a sub-select that reads a column of the row, a volatile one, one that may raise (a division, a cast), one that holds
-- a sub-select of its own (it may raise), FOR UPDATE, a sub-select too large for the hash table (a SubPlan that is not hashed). A test of two columns
-- (a row-valued NOT IN) or of a leaf the lookup table does not hold (an int8 column) is not a lookup table: the scan asks the SubPlan for each tuple.
SELECT plan_of($$SELECT kind, count(*) FROM ev WHERE (dim_a_id, tenant_id) NOT IN (SELECT dim_a_id, 7 FROM blocked WHERE dim_a_id IS NOT NULL) GROUP BY kind$$) AS row_valued;
SELECT plan_of($$SELECT kind, count(*) FROM ev WHERE (dim_a_id, tenant_id) IN (SELECT dim_a_id, 7 FROM blocked) OR kind = 'c' GROUP BY kind$$) AS row_valued_in;
SELECT plan_of($$SELECT kind, count(*) FROM ev e WHERE dim_a_id NOT IN (SELECT b.dim_a_id FROM blocked b WHERE b.dim_a_id IS NOT NULL AND b.dim_a_id % 5 = e.tenant_id % 5) GROUP BY kind$$) AS correlated;
SELECT plan_of($$SELECT kind, count(*) FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked WHERE dim_a_id IS NOT NULL AND random() < 2) GROUP BY kind$$) AS volatile_subselect;
SELECT shape_of($$SELECT kind, count(*) FROM ev WHERE dim_a_id NOT IN (SELECT 100 / (id - 5) FROM tenants) GROUP BY kind$$) AS raising_division;
SELECT shape_of($$SELECT kind, count(*) FROM ev WHERE kind NOT IN (SELECT (id::text)::int::text FROM tenants) GROUP BY kind$$) AS raising_cast;
SELECT plan_of($$SELECT kind, count(*) FROM ev WHERE dim_a_id NOT IN (SELECT id FROM tenants WHERE id > (SELECT dim_a_id FROM blocked LIMIT 1)) GROUP BY kind$$) AS nested_subplan;
SELECT plan_of($$SELECT kind, count(*) FROM ev WHERE dim_a_id NOT IN (SELECT id FROM tenants FOR UPDATE) GROUP BY kind$$) AS locking;
SELECT plan_of($$SELECT kind, count(*) FROM ev WHERE big_id NOT IN (SELECT dim_a_id FROM blocked WHERE dim_a_id IS NOT NULL) GROUP BY kind$$) AS int8_leaf;
SET work_mem = '64kB'; SET hash_mem_multiplier = 1;
SELECT plan_of($$SELECT kind, count(*) FROM ev WHERE dim_a_id NOT IN (SELECT i FROM generate_series(1, 400000) i WHERE i % 7 = 0) GROUP BY kind$$) AS not_hashed;
RESET work_mem; RESET hash_mem_multiplier;
-- the same ones run: the rows are those of the normal plan
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev e WHERE dim_a_id NOT IN (SELECT b.dim_a_id FROM blocked b WHERE b.dim_a_id IS NOT NULL AND b.dim_a_id % 5 = e.tenant_id % 5) GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE (dim_a_id, tenant_id) NOT IN (SELECT dim_a_id, 7 FROM blocked WHERE dim_a_id IS NOT NULL) GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT id FROM tenants WHERE id > (SELECT min(dim_a_id) FROM blocked)) GROUP BY kind$$);
SELECT * FROM same($$WITH s AS MATERIALIZED (SELECT id FROM tenants WHERE id < 20) SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT id FROM s) GROUP BY kind$$);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE big_id NOT IN (SELECT dim_a_id FROM blocked WHERE dim_a_id IS NOT NULL) GROUP BY kind$$);

-- parallel workers: each worker starts the sub-plan in an executor state of the statement
SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;
SELECT plan_of($$SELECT kind, count(*) FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked WHERE dim_a_id IS NOT NULL) GROUP BY kind$$) AS with_subplan;
SELECT plan_of($$SELECT kind, count(*) FROM ev WHERE dim_a_id NOT IN (1, 2, 3, 4, 5, 6) GROUP BY kind$$) AS constants;
SELECT * FROM same($$SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked WHERE dim_a_id IS NOT NULL) GROUP BY kind$$);
RESET max_parallel_workers_per_gather; RESET parallel_setup_cost; RESET parallel_tuple_cost; RESET min_parallel_table_scan_size;

-- a worker runs the sub-plan with the snapshot of the statement active: a stable SQL function in it reads a row this transaction wrote, under REPEATABLE READ
CREATE FUNCTION blocked_count() RETURNS bigint LANGUAGE sql STABLE PARALLEL SAFE AS $$ SELECT count(*) FROM blocked $$;
BEGIN ISOLATION LEVEL REPEATABLE READ;
INSERT INTO blocked VALUES (1), (2), (3);
SET LOCAL max_parallel_workers_per_gather = 2; SET LOCAL parallel_setup_cost = 0; SET LOCAL parallel_tuple_cost = 0; SET LOCAL min_parallel_table_scan_size = 0;
SELECT plan_of($$SELECT kind, count(*) FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked WHERE dim_a_id IS NOT NULL AND blocked_count() > 0) GROUP BY kind$$) AS with_subplan;
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked WHERE dim_a_id IS NOT NULL AND blocked_count() > 0) GROUP BY kind$$);
ROLLBACK;
DROP FUNCTION blocked_count();

-- a rescan: the node is the inner side of a nested loop and runs again for each row of the outer side (the groups are replayed)
SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_material = off;
SELECT * FROM same($$SELECT g, c.n FROM generate_series(1, 3) g CROSS JOIN (SELECT count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked WHERE dim_a_id IS NOT NULL)) c$$);
SELECT * FROM same($$WITH c AS NOT MATERIALIZED (SELECT kind, count(*) AS n FROM ev WHERE region IN (SELECT region FROM regions_off) OR kind = 'b' GROUP BY kind)
	SELECT a.kind, a.n, b.n AS again FROM c a JOIN c b ON a.kind = b.kind$$);
RESET enable_hashjoin; RESET enable_mergejoin; RESET enable_material;

-- the sub-select's table changes between statements (and inside a transaction): every statement starts the SubPlan again and sees the new rows
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked) GROUP BY kind$$);
INSERT INTO blocked SELECT i FROM generate_series(2, 300, 2) i;
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked) GROUP BY kind$$);
UPDATE blocked SET dim_a_id = dim_a_id + 1 WHERE dim_a_id < 150;
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked) GROUP BY kind$$);
DELETE FROM blocked WHERE dim_a_id > 100;
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked) GROUP BY kind$$);
INSERT INTO blocked VALUES (NULL);
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked) GROUP BY kind$$);
DELETE FROM blocked WHERE dim_a_id IS NULL;
BEGIN;
INSERT INTO blocked SELECT i FROM generate_series(101, 200) i;
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked) GROUP BY kind$$);
ROLLBACK;
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked) GROUP BY kind$$);
-- the table the scan reads changes too
UPDATE ev SET dim_a_id = NULL WHERE id % 10 = 0;
INSERT INTO ev SELECT 10000 + i, 1 + i % 300, 1, 1, 'north', 'a', date '2024-02-01', 1 FROM generate_series(1, 200) i;
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM blocked) OR kind = 'c' GROUP BY kind$$);

-- parameters: the sub-select and the tested column read them; a custom plan and a generic one answer like the normal plan
PREPARE nin(text, int) AS SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE tenant_id NOT IN (SELECT id FROM tenants WHERE kind = $1 AND id > $2) GROUP BY kind ORDER BY 1;
SET plan_cache_mode = force_custom_plan;
EXECUTE nin('outlet', 3);
EXECUTE nin('popup', 20);
SET plan_cache_mode = force_generic_plan;
EXECUTE nin('outlet', 3);
EXECUTE nin('popup', 20);
SELECT plan_of('EXECUTE nin(''flagship'', 1)') AS generic_plan;
SET sift.enable_agg = off;
EXECUTE nin('outlet', 3);
EXECUTE nin('popup', 20);
RESET sift.enable_agg; RESET plan_cache_mode;

-- plan invalidation: the table of the sub-select is dropped and made again with other rows; the cached plan is made again, and the statement before it fails
CREATE TABLE dropme (dim_a_id int);
INSERT INTO dropme SELECT i FROM generate_series(1, 300, 4) i;
PREPARE dropq AS SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM dropme) OR kind = 'c' GROUP BY kind ORDER BY 1;
SET plan_cache_mode = force_generic_plan;
SELECT plan_of('EXECUTE dropq') AS before_drop;
EXECUTE dropq;
DROP TABLE dropme;
EXECUTE dropq;
CREATE TABLE dropme (dim_a_id int);
INSERT INTO dropme SELECT i FROM generate_series(1, 300, 3) i;
EXECUTE dropq;
SELECT plan_of('EXECUTE dropq') AS after_drop;
SET sift.enable_agg = off;
EXECUTE dropq;
RESET sift.enable_agg; RESET plan_cache_mode;
-- a column of the sub-select changes type: the plan is made again
ALTER TABLE dropme ALTER COLUMN dim_a_id TYPE bigint;
SELECT * FROM same($$SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT dim_a_id FROM dropme) OR kind = 'c' GROUP BY kind$$);
EXECUTE dropq;

-- a cursor reads the snapshot it was declared in: the sub-select and the scan both, whatever commits (here the transaction's own later changes) afterwards
CREATE TABLE tags (id int);
INSERT INTO tags SELECT i FROM generate_series(1, 150) i;
BEGIN;
DECLARE c_on CURSOR FOR SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT id FROM tags) GROUP BY kind ORDER BY 1;
SET LOCAL sift.enable_agg = off;
DECLARE c_off CURSOR FOR SELECT kind, count(*) AS n FROM ev WHERE dim_a_id NOT IN (SELECT id FROM tags) GROUP BY kind ORDER BY 1;
RESET sift.enable_agg;
DELETE FROM tags WHERE id < 100;
INSERT INTO tags SELECT i FROM generate_series(150, 250) i;
FETCH ALL FROM c_on;
FETCH ALL FROM c_off;
CLOSE c_on; CLOSE c_off;
ROLLBACK;

DEALLOCATE ALL;
DROP FUNCTION same(text);
DROP FUNCTION plan_of(text);
DROP FUNCTION shape_of(text);
DROP TABLE ev, tenants, blocked, blocked_null, nothing, regions_off, regions_off_null, holidays, dropme, tags, wide;
DROP EXTENSION pg_sift;
