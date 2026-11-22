-- The zones and scope statistics a planning takes from the facts of an index are copies the planning owns until a later planning reads the facts again; no plan keeps them. A statement
-- prepared and run on a generic plan, a plan kept by plpgsql, a cursor open across the re-read, and a plan with workers give the rows of the normal plan after each change that makes
-- the next planning read the facts again (rows inserted, a merge, ANALYZE, ALTER INDEX SET, ALTER TABLE SET, COMMENT, REINDEX).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET jit = off;
SET sift.agg_force = on;

CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 6) j WHERE m % 25 <> 0;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE zr (id int, grp int, dim_a_id int, v int) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO zr SELECT i, i % 10, 1 + (i * 7919) % 300, i % 13 FROM generate_series(1, 40000) i;
VACUUM (FREEZE, ANALYZE) zr, tenant_scope;
CREATE INDEX zr_col ON zr USING sift_col (id, grp, dim_a_id, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) max_multiplicity(8)');

-- the rows of a statement with the normal plan, as one string
CREATE FUNCTION normal_rows(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT string_agg(t::text, '','' ORDER BY t::text) FROM (%s) t', q) INTO r;
	RESET sift.enable_agg;
	RETURN r;
END $$;

CREATE FUNCTION window_q(lo int, hi int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT grp, count(*), sum(v) FROM zr WHERE id BETWEEN %s AND %s GROUP BY grp', lo, hi) $$;
CREATE FUNCTION scope_q(k int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT count(*), sum(v), min(id) FROM zr e WHERE sift.in_scope(''zr_col''::regclass, ''tenant_id'', e.dim_a_id, %s)', k) $$;

SET plan_cache_mode = force_generic_plan;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET seq_page_cost = 10000;
SET max_parallel_workers_per_gather = 2;
PREPARE by_window (int, int) AS SELECT grp, count(*), sum(v) FROM zr WHERE id BETWEEN $1 AND $2 GROUP BY grp;
PREPARE by_scope AS SELECT count(*), sum(v), min(id) FROM zr e WHERE sift.in_scope('zr_col'::regclass, 'tenant_id', e.dim_a_id, 5);

CREATE FUNCTION kept_window(lo int, hi int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	SELECT string_agg(t::text, ',' ORDER BY t::text) INTO r FROM (SELECT grp, count(*), sum(v) FROM zr WHERE id BETWEEN lo AND hi GROUP BY grp) t;
	RETURN r;
END $$;

CREATE FUNCTION kept_scope() RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	SELECT string_agg(t::text, ',' ORDER BY t::text) INTO r FROM (SELECT count(*), sum(v), min(id) FROM zr e WHERE sift.in_scope('zr_col'::regclass, 'tenant_id', e.dim_a_id, 5)) t;
	RETURN r;
END $$;

CREATE FUNCTION prepared_rows(call text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'CREATE TEMP TABLE pr AS EXECUTE ' || call;
	SELECT string_agg(t::text, ',' ORDER BY t::text) INTO r FROM pr t;
	DROP TABLE pr;
	RETURN r;
END $$;

CREATE FUNCTION scope_ref(k int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT count(*), sum(v), min(id) FROM zr e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = %s)', k) $$;

-- the statements kept by the session (the generic plans, the plan of plpgsql) against the normal plan, on the table as it is now
CREATE FUNCTION kept_plans_same() RETURNS boolean LANGUAGE plpgsql AS $$
BEGIN
	RETURN prepared_rows('by_window(1000, 30000)') = normal_rows(window_q(1000, 30000))
		AND prepared_rows('by_scope') = normal_rows(scope_ref(5))
		AND kept_window(1000, 30000) = normal_rows(window_q(1000, 30000))
		AND kept_scope() = normal_rows(scope_ref(5));
END $$;

-- the cursor is open while change runs and the next plannings read the facts again; its rows are those of the normal plan at its open
CREATE FUNCTION cursor_across(change text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
	c CURSOR FOR SELECT grp, count(*), sum(v) FROM zr WHERE id BETWEEN 2000 AND 38000 GROUP BY grp ORDER BY grp;
	s CURSOR FOR SELECT count(*), sum(v), min(id) FROM zr e WHERE sift.in_scope('zr_col'::regclass, 'tenant_id', e.dim_a_id, 5);
	rec record;
	got text[] := '{}';
	want text := normal_rows(window_q(2000, 38000));
	want_scope text := normal_rows(scope_ref(5));
BEGIN
	OPEN c;
	OPEN s;
	FETCH c INTO rec;
	got := got || rec::text;
	EXECUTE change;
	EXECUTE 'EXPLAIN ' || window_q(3000, 20000);
	EXECUTE 'EXPLAIN ' || scope_q(7);
	LOOP
		FETCH c INTO rec;
		EXIT WHEN NOT FOUND;
		got := got || rec::text;
	END LOOP;
	CLOSE c;
	FETCH s INTO rec;
	CLOSE s;
	RETURN (SELECT string_agg(x, ',' ORDER BY x) FROM unnest(got) x) = want AND rec::text = want_scope;
END $$;

-- a change that makes the next planning read the facts again, then new plannings of both shapes, then the kept plans
CREATE FUNCTION after_change(change text) RETURNS boolean LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE change;
	EXECUTE 'EXPLAIN ' || window_q(3000, 20000);
	EXECUTE 'EXPLAIN ' || scope_q(7);
	RETURN kept_plans_same();
END $$;

CREATE FUNCTION plan_has(stmt text, node text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE stmt LOOP
		IF l LIKE '%' || node || '%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- the plans the session keeps: a window of the zones with workers, and over the scope units
SELECT prepared_rows('by_window(1000, 30000)') = normal_rows(window_q(1000, 30000)) AS window_run,
       prepared_rows('by_scope') = normal_rows(scope_ref(5)) AS scope_run;
SELECT plan_has('EXPLAIN (COSTS OFF) EXECUTE by_window(1000, 30000)', 'SiftAgg') AS window_scan,
       plan_has('EXPLAIN (COSTS OFF) EXECUTE by_window(1000, 30000)', 'Workers Planned') AS with_workers,
       plan_has('EXPLAIN (COSTS OFF) EXECUTE by_scope', 'Scope Index') AS over_scope;
SELECT kept_plans_same() AS baseline;
SELECT after_change($$INSERT INTO zr SELECT i, i % 10, 1 + (i * 7919) % 300, i % 13 FROM generate_series(40001, 52000) i$$) AS rows_inserted;
SELECT cursor_across($$INSERT INTO zr SELECT i, i % 10, 1 + (i * 7919) % 300, i % 13 FROM generate_series(52001, 60000) i$$) AS cursor_rows_inserted;
SELECT kept_plans_same() AS after_cursor;
SELECT after_change($$SELECT sift.col_merge('zr_col', true)$$) AS merged;
SELECT cursor_across($$SELECT sift.col_merge('zr_col', true)$$) AS cursor_merged;
SELECT after_change($$ANALYZE zr$$) AS analyzed;
SELECT cursor_across($$ANALYZE zr$$) AS cursor_analyzed;
SELECT after_change($$ALTER INDEX zr_col SET (dead_rewrite_pct = 30)$$) AS index_set;
SELECT after_change($$ALTER TABLE zr SET (fillfactor = 90)$$) AS table_set;
SELECT after_change($$COMMENT ON TABLE zr IS 'rows of the facts'$$) AS commented;
SELECT after_change($$REINDEX INDEX zr_col$$) AS reindexed;
DROP TABLE zr, tenant_scope;
DROP FUNCTION normal_rows, window_q, scope_q, scope_ref, kept_window, kept_scope, prepared_rows, plan_has, kept_plans_same, cursor_across, after_change;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
