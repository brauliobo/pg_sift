-- A set filter bound as one array parameter (col = ANY($1)) on a generic plan (docs/SET-FILTERS.md): the length of the array is not known to the
-- planner, which assumes 10 elements and prices an index or bitmap scan for them. The scan of Custom Scan (SiftAgg) reads the whole array into a
-- table when it starts, so the generic plan takes it by default (sift.agg_param_set_size), and the answers are those of the normal plan for every array.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ev (id int PRIMARY KEY, entity_id int, small_id int2, big_id int8, kind text, amount numeric(10, 2), day date) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i,
       CASE WHEN i <= 5 THEN -i                           -- negative ids
            WHEN i <= 10 THEN 2147483647 - i              -- far above the others: a span no byte table covers
            WHEN i % 1000 = 0 THEN NULL
            ELSE 1 + (i::bigint * 7919) % 40000 END,
       (i % 50)::int2, 5000000000 + (i::bigint * 7919) % 40000,
       (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4], (i % 1000) / 10.0, date '2024-01-01' + i % 700
FROM generate_series(1, 400000) i;
CREATE INDEX ev_entity ON ev (entity_id, day);
CREATE INDEX ev_col ON ev USING sift_col (entity_id, small_id, big_id, kind, amount, day);
VACUUM (FREEZE, ANALYZE) ev;

-- the plan of a statement, EXPLAIN without the numbers
CREATE FUNCTION plan_kind(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
		IF l LIKE '%Workers Planned: 2%' THEN r := r || ' with workers'; END IF;
	END LOOP;
	RETURN r;
END $$;

SET plan_cache_mode = force_generic_plan;
-- the pushed plan, prepared with the aggregate pushdown on, and the normal plan, prepared with it off: a generic plan is made at the first EXECUTE
PREPARE on_q(int4[]) AS
SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE day >= '2024-01-01' AND entity_id = ANY ($1) GROUP BY kind ORDER BY kind;
SELECT plan_kind('EXECUTE on_q(''{1,2,3}'')') AS default_costs;
SET sift.enable_agg = off;
PREPARE off_q(int4[]) AS
SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE day >= '2024-01-01' AND entity_id = ANY ($1) GROUP BY kind ORDER BY kind;
SELECT plan_kind('EXECUTE off_q(''{1,2,3}'')') AS enable_agg_off;
RESET sift.enable_agg;

-- the planner's own guess of 10 elements keeps the bitmap scan; the elements assumed by sift.agg_param_set_size move the choice
SET sift.agg_param_set_size = 0;
PREPARE guess_q(int4[]) AS
SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE day >= '2024-01-01' AND entity_id = ANY ($1) GROUP BY kind ORDER BY kind;
SELECT plan_kind('EXECUTE guess_q(''{1,2,3}'')') AS planner_guess;
RESET sift.agg_param_set_size;

-- the plan shows the parameter, and a query SiftAgg cannot run keeps its plan whatever the size of the array
EXPLAIN (COSTS OFF) EXECUTE on_q('{1,2,3}');
PREPARE other_q(int4[]) AS SELECT count(*) FROM ev WHERE entity_id = ANY ($1) AND kind ~ '^[ab]' AND (id % 7 = 0 OR amount > 99);
SELECT plan_kind('EXECUTE other_q(''{1,2,3}'')') AS unsupported_shape;
PREPARE rows_q(int4[]) AS SELECT id FROM ev WHERE entity_id = ANY ($1) ORDER BY id LIMIT 5;
SELECT plan_kind('EXECUTE rows_q(''{1,2,3}'')') AS no_aggregate;

-- the same rows as the normal plan, for every shape of array
CREATE FUNCTION same_rows(arr int4[]) RETURNS TABLE (arr_len int, rows_on bigint, rows_diff bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS EXECUTE on_q(%L)', arr);
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS EXECUTE off_q(%L)', arr);
	RETURN QUERY SELECT coalesce(cardinality(arr), -1), (SELECT count(*) FROM r_on),
		(SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
END $$;
SELECT * FROM same_rows('{1,2,3}');
SELECT * FROM same_rows('{}');
SELECT * FROM same_rows('{NULL}');
SELECT * FROM same_rows('{1,NULL,3}');
SELECT * FROM same_rows('{7,7,7,8,8}');
SELECT * FROM same_rows(NULL);
SELECT * FROM same_rows('{-1,-3,-100,40001,2147483640,2147483642}');
SELECT * FROM same_rows('{2147483640,2147483641,-5}');
SELECT * FROM same_rows(ARRAY(SELECT generate_series(1, 40000, 4)));
SELECT * FROM same_rows(ARRAY(SELECT generate_series(1, 200000)));
SELECT * FROM same_rows(ARRAY(SELECT generate_series(1, 200000) UNION ALL SELECT NULL));
SELECT * FROM same_rows(ARRAY(SELECT x FROM generate_series(1, 30000) x CROSS JOIN generate_series(1, 3)));

-- other element types, a second set, NOT IN with a NULL, and the same query as a custom plan and as a prepared statement that turns generic
CREATE FUNCTION diff_count(a text, b text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM ((%1$s EXCEPT %2$s) UNION ALL (%2$s EXCEPT %1$s)) d', a, b) INTO n;
	RETURN n;
END $$;
CREATE FUNCTION compare_modes(q text, VARIADIC args text[]) RETURNS TABLE (pushed text, rows_on bigint, rows_diff bigint) LANGUAGE plpgsql AS $$
DECLARE a text := array_to_string(args, ', ');
BEGIN
	EXECUTE 'PREPARE cm_on AS ' || q;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'PREPARE cm_off AS ' || q;
	SET LOCAL sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE cm_a ON COMMIT DROP AS EXECUTE cm_on(%s)', a);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE cm_b ON COMMIT DROP AS EXECUTE cm_off(%s)', a);
	SET LOCAL sift.enable_agg = on;
	RETURN QUERY SELECT plan_kind(format('EXECUTE cm_on(%s)', a)), (SELECT count(*) FROM cm_a), diff_count('TABLE cm_a', 'TABLE cm_b');
	DEALLOCATE cm_on;
	DEALLOCATE cm_off;
END $$;

CREATE FUNCTION set_of(n int, step int) RETURNS int4[] LANGUAGE sql AS 'SELECT array_agg(x) FROM generate_series(1, n, step) x';
SELECT * FROM compare_modes('SELECT small_id, count(*) AS n, sum(amount) AS s FROM ev WHERE small_id = ANY ($1) GROUP BY small_id', '''{1,5,9,NULL}''::int2[]');
SELECT * FROM compare_modes('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE big_id = ANY ($1) GROUP BY kind', '''{5000000001,5000000007,5000000011}''::int8[]');
SELECT * FROM compare_modes('SELECT kind, count(*) AS n FROM ev WHERE entity_id = ANY ($1) AND small_id = ANY ($2) GROUP BY kind',
                            '''{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40}''::int4[]', '''{1,2,3,4,5}''::int2[]');
SELECT * FROM compare_modes('SELECT kind, count(*) AS n FROM ev WHERE entity_id <> ALL ($1) GROUP BY kind', '''{1,2,3}''::int4[]');
SELECT * FROM compare_modes('SELECT kind, count(*) AS n FROM ev WHERE entity_id <> ALL ($1) GROUP BY kind', '''{1,2,NULL}''::int4[]');
SELECT * FROM compare_modes('SELECT kind, count(*) AS n FROM ev WHERE entity_id <> ALL ($1) GROUP BY kind', '''{}''::int4[]');
SELECT * FROM compare_modes('SELECT count(*) AS n, count(entity_id) AS nn, min(entity_id) AS lo, max(entity_id) AS hi FROM ev WHERE entity_id = ANY ($1)', '''{-3,-2,40000,2147483640}''::int4[]');

-- custom plans see the array and are as before; a statement in auto mode turns generic after five executions with the same answers
SET plan_cache_mode = force_custom_plan;
PREPARE custom_q(int4[]) AS SELECT kind, count(*) AS n FROM ev WHERE entity_id = ANY ($1) GROUP BY kind ORDER BY kind;
SELECT * FROM same_rows('{1,2,3}');
EXECUTE custom_q('{1,2,3}');
SET plan_cache_mode = auto;
PREPARE auto_q(int4[]) AS SELECT kind, count(*) AS n FROM ev WHERE day >= '2024-01-01' AND entity_id = ANY ($1) GROUP BY kind ORDER BY kind;
EXECUTE auto_q('{1,2,3}');
EXECUTE auto_q('{1,2,3}');
EXECUTE auto_q('{1,2,3}');
EXECUTE auto_q('{1,2,3}');
EXECUTE auto_q('{1,2,3}');
EXECUTE auto_q('{1,2,3}');
EXECUTE auto_q('{4,5,6}');
SET plan_cache_mode = force_generic_plan;
CREATE TEMP TABLE auto_r AS EXECUTE auto_q('{4,5,6}');
SELECT diff_count('TABLE auto_r', 'SELECT kind, count(*) FROM ev WHERE day >= ''2024-01-01'' AND entity_id IN (4, 5, 6) GROUP BY kind') AS auto_diff;

-- parallel workers
SET plan_cache_mode = force_generic_plan;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET seq_page_cost = 10000;
SET sift.agg_preagg = off;
SELECT * FROM compare_modes('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE day >= ''2024-01-01'' AND entity_id = ANY ($1) GROUP BY kind',
                            'set_of(40000, 3)');

DEALLOCATE ALL;
DROP FUNCTION compare_modes(text, text[]);
DROP FUNCTION set_of(int, int);
DROP FUNCTION diff_count(text, text);
DROP FUNCTION same_rows(int4[]);
DROP FUNCTION plan_kind(text);
DROP TABLE ev;
DROP EXTENSION pg_sift;
