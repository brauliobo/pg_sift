-- A group table that outgrows hash memory (statistics that predate a bulk load: 10 groups known, 20,000 real) ends no statement with 54000, whatever the kind of
-- node and the context the statement runs in. A node that can spill (grouping sets, a generic aggregate with a combine function) writes its groups to partitions.
-- A node that cannot (an ordered scan on a text key, with or without a limit, an ordered-set aggregate, a generic aggregate that sorts, a distinct count of a joined column) holds its groups
-- to the end: where the statement can run again as PostgreSQL plans it (a SELECT that calls no volatile function: also a cursor's first fetch, a function's query
-- with parameters, a prepared statement, a scan with workers that failed) it does; where it cannot (an INSERT, a volatile function) the planner leaves the
-- statement to the normal plan when the most groups the source can make would not fit hash memory.
-- A node that can spill but runs with workers does not spill (each process holds its groups): past its share of hash memory it runs again too.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE stale (id int, g int, v int, t text, dim_id int);
ALTER TABLE stale SET (autovacuum_enabled = false);
INSERT INTO stale SELECT i, i % 10, i % 100, 'a' || (i % 13), i % 250 FROM generate_series(1, 400) i;
CREATE TABLE dims (id int PRIMARY KEY, name text);
INSERT INTO dims SELECT i, 'n' || (i % 50) FROM generate_series(0, 249) i;
CREATE INDEX stale_col ON stale USING sift_col (id, g, v, t, dim_id);
VACUUM (FREEZE, ANALYZE) stale;
ANALYZE dims;
INSERT INTO stale SELECT i, i, i % 100, 'a' || (i % 13), i % 250 FROM generate_series(401, 20400) i;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SET sift.agg_force = on;

-- one query for each kind, with the condition lim >= 0 that every row passes; its rows are (k, n, x)
CREATE TABLE kinds (kind text PRIMARY KEY, q text);
INSERT INTO kinds VALUES
	('sets',         'SELECT g::text AS k, count(*) AS n, sum(v)::text AS x FROM stale WHERE v >= %s GROUP BY GROUPING SETS ((g), ())'),
	('generic',      'SELECT g::text AS k, count(*) AS n, bit_or(v)::text AS x FROM stale WHERE v >= %s GROUP BY g'),
	('text order',   'SELECT g::text AS k, count(*) AS n, ''''::text AS x FROM stale WHERE v >= %s GROUP BY g::text ORDER BY 1'),
	('ordered set',  'SELECT g::text AS k, count(*) AS n, percentile_cont(0.5) WITHIN GROUP (ORDER BY v)::text AS x FROM stale WHERE v >= %s GROUP BY g'),
	('generic sort', 'SELECT g::text AS k, count(*) AS n, string_agg(t, '','' ORDER BY t) AS x FROM stale WHERE v >= %s GROUP BY g'),
	('joined',       'SELECT s.g::text AS k, count(*) AS n, count(DISTINCT d.name)::text AS x FROM stale s JOIN dims d ON d.id = s.dim_id WHERE s.v >= %s GROUP BY s.g'),
	('top-N',        'SELECT g::text AS k, count(*) AS n, ''''::text AS x FROM stale WHERE v >= %s GROUP BY g::text ORDER BY 1 LIMIT 3000'),
	('two level',    'SELECT s.kk AS k, count(*) AS n, sum(s.c)::text AS x FROM (SELECT g::text AS kk, id, count(*) AS c FROM stale WHERE v >= %s GROUP BY g::text, id) s GROUP BY s.kk');

-- the same query as the body of a PL/pgSQL function and of an SQL function, each with the condition as its parameter
DO $$
DECLARE k record;
BEGIN
	FOR k IN SELECT row_number() OVER (ORDER BY kind) AS i, q FROM kinds LOOP
		EXECUTE format('CREATE FUNCTION ctx_pl%s(lim int) RETURNS TABLE (k text, n bigint, x text) LANGUAGE plpgsql AS %L', k.i,
			format('DECLARE r record; BEGIN FOR r IN %s LOOP k := r.k; n := r.n; x := r.x; RETURN NEXT; END LOOP; END', format(k.q, 'lim')));
		EXECUTE format('CREATE FUNCTION ctx_sql%s(lim int) RETURNS TABLE (k text, n bigint, x text) LANGUAGE sql AS %L', k.i, format(k.q, 'lim'));
	END LOOP;
END $$;

CREATE TABLE cells (kind text, ctx text, rows bigint, differ bigint, spilled boolean, reran boolean, state text, workers boolean);
CREATE FUNCTION stat(n text) RETURNS bigint LANGUAGE sql AS $$ SELECT value FROM sift.stats(local => true) WHERE name = n $$;

CREATE FUNCTION run_cell(kind text, ctx text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE i int; q text; sp bigint; rr bigint; wl bigint; r record; err text := 'ok'; nrows bigint; nd bigint; js json;
BEGIN
	SELECT n, kinds.q INTO i, q FROM (SELECT row_number() OVER (ORDER BY kinds.kind) AS n, kinds.kind, kinds.q FROM kinds) kinds WHERE kinds.kind = run_cell.kind;
	DROP TABLE IF EXISTS got;
	DROP TABLE IF EXISTS want;
	CREATE TEMP TABLE got (k text, n bigint, x text);
	sp := stat('group_table_spills');
	rr := stat('statements_rerun');
	wl := stat('workers_launched');
	BEGIN
		IF ctx = 'plain' THEN
			EXECUTE format('SELECT json_agg(s) FROM (%s) s', format(q, '0')) INTO js;
			INSERT INTO got SELECT * FROM json_populate_recordset(NULL::got, js);
		ELSIF ctx = 'cursor' THEN
			EXECUTE 'DECLARE c CURSOR FOR ' || format(q, '0');
			FOR r IN EXECUTE 'FETCH 3 FROM c' LOOP INSERT INTO got VALUES (r.k, r.n, r.x); END LOOP;
			FOR r IN EXECUTE 'FETCH ALL FROM c' LOOP INSERT INTO got VALUES (r.k, r.n, r.x); END LOOP;
			EXECUTE 'CLOSE c';
		ELSIF ctx = 'scroll cursor' THEN
			EXECUTE 'DECLARE c SCROLL CURSOR FOR ' || format(q, '0');
			FOR r IN EXECUTE 'FETCH 3 FROM c' LOOP INSERT INTO got VALUES (r.k, r.n, r.x); END LOOP;
			EXECUTE 'FETCH BACKWARD 1 FROM c';
			FOR r IN EXECUTE 'FETCH FORWARD 1 FROM c' LOOP NULL; END LOOP;
			FOR r IN EXECUTE 'FETCH ALL FROM c' LOOP INSERT INTO got VALUES (r.k, r.n, r.x); END LOOP;
			EXECUTE 'CLOSE c';
		ELSIF ctx = 'plpgsql function' THEN
			EXECUTE format('INSERT INTO got SELECT * FROM ctx_pl%s(0)', i);
		ELSIF ctx = 'sql function' THEN
			EXECUTE format('INSERT INTO got SELECT * FROM ctx_sql%s(0)', i);
		ELSIF ctx = 'prepared' THEN
			EXECUTE format('PREPARE p(int) AS %s', format(q, '$1'));
			SET plan_cache_mode = force_generic_plan;
			CREATE TEMP TABLE got_p AS EXECUTE p(0);
			INSERT INTO got SELECT * FROM got_p;
			DROP TABLE got_p;
			RESET plan_cache_mode;
			DEALLOCATE p;
		ELSIF ctx = 'workers only' THEN
			SET max_parallel_workers_per_gather = 2;
			SET parallel_setup_cost = 0;
			SET parallel_tuple_cost = 0;
			SET min_parallel_table_scan_size = 0;
			SET min_parallel_index_scan_size = 0;
			SET parallel_leader_participation = off;
			EXECUTE format('CREATE TEMP TABLE got_w AS %s', format(q, '0'));
			INSERT INTO got SELECT * FROM got_w;
			DROP TABLE got_w;
			RESET max_parallel_workers_per_gather;
			RESET parallel_setup_cost;
			RESET parallel_tuple_cost;
			RESET min_parallel_table_scan_size;
			RESET min_parallel_index_scan_size;
			RESET parallel_leader_participation;
		ELSIF ctx = 'insert' THEN
			CREATE TEMP TABLE ins (k text, n bigint, x text);
			EXECUTE format('INSERT INTO ins %s', format(q, '0'));
			INSERT INTO got SELECT * FROM ins;
			DROP TABLE ins;
		ELSIF ctx = 'volatile' THEN
			EXECUTE format('INSERT INTO got SELECT * FROM (%s) s WHERE clock_timestamp() IS NOT NULL', format(q, '0'));
		END IF;
	EXCEPTION WHEN OTHERS THEN
		err := SQLSTATE;
		RESET plan_cache_mode;
		RESET max_parallel_workers_per_gather;
		RESET parallel_setup_cost;
		RESET parallel_tuple_cost;
		RESET min_parallel_table_scan_size;
		RESET min_parallel_index_scan_size;
		RESET parallel_leader_participation;
	END;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE format('CREATE TEMP TABLE want AS %s', format(q, '0'));
	PERFORM set_config('sift.enable_agg', 'on', true);
	SELECT count(*) INTO nrows FROM got;
	SELECT count(*) INTO nd FROM (SELECT * FROM got EXCEPT ALL SELECT * FROM want UNION ALL SELECT * FROM want EXCEPT ALL SELECT * FROM got) d;
	INSERT INTO cells VALUES (kind, ctx, nrows, nd, stat('group_table_spills') > sp, stat('statements_rerun') > rr, err, stat('workers_launched') > wl);
END $$;

SELECT run_cell(kind, ctx) FROM kinds, unnest(ARRAY['plain', 'cursor', 'scroll cursor', 'plpgsql function', 'sql function', 'prepared', 'workers only', 'insert', 'volatile']) ctx
	ORDER BY kind, ctx;
SELECT kind, ctx, rows, differ, CASE WHEN spilled THEN 'spilled' WHEN reran THEN 'ran again' ELSE '' END AS how, state FROM cells ORDER BY kind, ctx;
-- the cells of the scan with workers did run with workers (what a worker spills is not in the leader's counter: such a cell shows neither)
SELECT kind, workers FROM cells WHERE ctx = 'workers only' ORDER BY kind;

-- what the planner leaves the node: with stale statistics, a statement that can run again takes the scan (its estimate is small); one that cannot, the most groups
-- the source can make (its rows) against hash memory
CREATE TABLE ins_shape (k text, n bigint, x text);
CREATE FUNCTION shape(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'heap';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF, SIFT) ' || q LOOP
		IF l ~ 'SiftAgg' THEN r := 'SiftAgg'; END IF;
		IF l ~ 'Sift: not used: group_memory' THEN r := 'heap: group_memory'; END IF;
	END LOOP;
	RETURN r;
END $$;
SELECT kind, shape(format(q, '0')) AS select_, shape('INSERT INTO ins_shape ' || format(q, '0')) AS insert_,
	shape(format('SELECT * FROM (%s) s WHERE clock_timestamp() IS NOT NULL', format(q, '0'))) AS volatile_
FROM kinds ORDER BY kind;
SELECT kind, used, slug FROM kinds, sift.explain_why(replace(format(q, '0'), ' AS n,', ' AS n, clock_timestamp() AS t,')) WHERE kind = 'generic sort';

DO $$
DECLARE k record;
BEGIN
	FOR k IN SELECT row_number() OVER (ORDER BY kind) AS i FROM kinds LOOP
		EXECUTE format('DROP FUNCTION ctx_pl%s(int)', k.i);
		EXECUTE format('DROP FUNCTION ctx_sql%s(int)', k.i);
	END LOOP;
END $$;
DROP FUNCTION run_cell(text, text), stat(text), shape(text);
DROP TABLE cells, kinds, ins_shape, dims, stale;
DROP EXTENSION pg_sift;
