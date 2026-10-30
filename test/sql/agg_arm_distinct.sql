-- count(DISTINCT x) of a column the per-row aggregates group by (a parent id, a region), under an outer aggregate: the scan keeps the
-- distinct ids of the column per outer group (a bitmap over the key's ids, a hash set or a sort as count(DISTINCT) of a column does),
-- not the per-row groups that PostgreSQL would sort. Every query runs with and without the pushdown; the answers must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE items (id int PRIMARY KEY, parent_id int, region text, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO items
SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 600 END, CASE WHEN i % 17 = 0 THEN NULL ELSE 'r' || i % 9 END,
       CASE WHEN i % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('k', (array['a', 'b', 'c'])[1 + (i + j) % 3], 'n', (i * j) % 10))
                                              FROM generate_series(1, 1 + i % 4) j)) END
FROM generate_series(1, 20000) i;
CREATE INDEX items_col ON items USING sift_col (id, parent_id, region, doc) WITH (paths = 'doc.parts[]{k text, n int4}');
VACUUM (FREEZE, ANALYZE) items;

-- the outer aggregates given; the arms are per item and key, and per item
CREATE FUNCTION report(aggs text, tail text DEFAULT '', filt text DEFAULT 'true') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH base AS (SELECT t.id, t.parent_id, t.region, p.k, p.n
	                                FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, n int) WHERE %s)
	                 SELECT k, %s FROM (SELECT id, parent_id, region, k, sum(n) AS s, count(*) AS c FROM base GROUP BY id, parent_id, region, k
	                                    UNION ALL
	                                    SELECT id, parent_id, region, NULL::text, sum(n), count(*) FROM base GROUP BY id, parent_id, region) q
	                 GROUP BY k %s$q$, filt, aggs, tail)
$$;

-- the plan has the scan and neither the grouping of PostgreSQL nor the node that groups rows
CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_is_agg_scan(q) $$;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; n bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO n FROM r_on;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND n > 0 AND pushed(q);
END $$;

-- an integer column and a text column, next to the row count and the sums; NULL parents and regions are no value
SELECT same(report('count(DISTINCT parent_id) AS parents')) AS parents;
SELECT same(report('count(DISTINCT region) AS regions')) AS regions;
SELECT same(report('count(DISTINCT parent_id) AS parents, count(DISTINCT region) AS regions, count(DISTINCT id) AS ids, count(*) AS cells, sum(s) AS total')) AS several;
-- the FILTER of an inner aggregate, and the same column with several FILTERs
SELECT same(report('count(DISTINCT parent_id) FILTER (WHERE c > 1) AS many, count(DISTINCT parent_id) FILTER (WHERE s > 9) AS big, count(DISTINCT parent_id) AS parents')) AS filters;
-- a HAVING on the outer query and a WHERE of the arms
SELECT same(report('count(DISTINCT parent_id) AS parents', 'HAVING count(DISTINCT region) > 3')) AS having;
SELECT same(report('count(DISTINCT parent_id) AS parents, count(DISTINCT region) AS regions', '', 'n > 3 AND t.parent_id < 400')) AS where_of_arms;
-- the column is an outer key too, and a grouping set
SELECT same(replace(report('count(DISTINCT parent_id) AS parents, count(DISTINCT region) AS regions'), 'GROUP BY k', 'GROUP BY ROLLUP (k)')) AS rollup;
-- the column of the distinct count is an outer key, next to another column and the ids (many groups: the sets follow the groups as they grow)
SELECT same(replace(report('count(DISTINCT parent_id) AS parents, count(DISTINCT region) AS regions, count(DISTINCT id) AS ids, count(*) AS cells'),
                    'GROUP BY k', 'GROUP BY k, parent_id')) AS outer_key;
-- not a column the arms group by: PostgreSQL's plan
SELECT pushed(report('count(DISTINCT s) AS sums')) AS pushed_not_a_key;

-- more ids than hash_mem holds: the set moves to a hash and a sort, and the answer is the same
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT same(report('count(DISTINCT parent_id) AS parents, count(DISTINCT id) AS ids, count(DISTINCT region) AS regions')) AS small_memory;
RESET work_mem;
RESET hash_mem_multiplier;

-- the plan shows the aggregates
CREATE FUNCTION agg_lines(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Pushed Aggregates' THEN RETURN NEXT trim(l); END IF;
	END LOOP;
END $$;
SELECT * FROM agg_lines(report('count(DISTINCT parent_id) AS parents, count(DISTINCT id) AS ids'));

DROP FUNCTION agg_lines(text), same(text), pushed(text), report(text, text, text);
-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
