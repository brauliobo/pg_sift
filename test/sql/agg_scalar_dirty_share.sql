-- The share of the rows that hold a value a typed path leaf cannot store (a numeric string where a float8 is declared): 0.1%, 3% and 50% of 20,000
-- rows, spread over every unit of the index. The scan reads those rows again from the heap and answers as the normal plan, whatever the share;
-- the normal plan is the one the planner prefers when the rows read again cost more than it does (sift.explain_why names the slug).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

\ir test/agg_helpers.sql

\ir test/agg_same_helpers.sql

CREATE FUNCTION reread(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text; n bigint := 0;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Rows Read Again From The Heap' THEN n := regexp_replace(l, '\D', '', 'g')::bigint; END IF;
	END LOOP;
	RETURN n;
END $$;

-- items_<k>: 20,000 rows of which every k-th holds its scores as strings (k = 1000, 33 and 2 are 0.1%, 3% and 50%)
DO $do$
DECLARE k int;
BEGIN
	FOREACH k IN ARRAY ARRAY[1000, 33, 2] LOOP
		EXECUTE format($t$CREATE TABLE items_%1$s (id int PRIMARY KEY, grp int, doc jsonb) WITH (autovacuum_enabled = false)$t$, k);
		EXECUTE format($t$INSERT INTO items_%1$s
			SELECT i, i %% 7,
			       jsonb_build_object('score', CASE WHEN i %% %1$s = 0 THEN to_jsonb(((i %% 9) + 0.5)::text) ELSE to_jsonb((i %% 9) + 0.5) END,
			                          'parts', jsonb_build_array(
			                              jsonb_build_object('cat', 'cat_a', 'score', CASE WHEN i %% %1$s = 0 THEN to_jsonb((i %% 5)::text) ELSE to_jsonb(i %% 5) END),
			                              jsonb_build_object('cat', 'cat_b', 'score', (i %% 3) + 0.25)))
			FROM generate_series(1, 20000) i$t$, k);
		EXECUTE format($t$CREATE INDEX items_%1$s_col ON items_%1$s USING sift_col (id, grp, doc) WITH (paths = 'doc.score float8, doc.parts[]{cat text, score float8}')$t$, k);
	END LOOP;
END $do$;
VACUUM ANALYZE items_1000;
VACUUM ANALYZE items_33;
VACUUM ANALYZE items_2;

CREATE TABLE shapes (n text, q text);
INSERT INTO shapes VALUES
	('scalar avg', 'SELECT grp, count(*), avg((doc ->> ''score'')::float8) FROM items_%s GROUP BY grp'),
	('scalar max', 'SELECT (doc ->> ''score'')::float8 k, count(*), max((doc ->> ''score'')::float8) FROM items_%s GROUP BY 1'),
	('scalar where', 'SELECT grp, count(*) FROM items_%s WHERE (doc ->> ''score'')::float8 > 3 GROUP BY grp'),
	('scalar filter', 'SELECT grp, count(*) FILTER (WHERE (doc ->> ''score'')::float8 > 3) FROM items_%s GROUP BY grp'),
	('scalar gated', 'SELECT grp, sum((doc ->> ''score'')::float8) FILTER (WHERE grp = 1), avg((doc ->> ''score'')::float8) FILTER (WHERE grp = 2) FROM items_%s GROUP BY grp'),
	('elements',  'SELECT p ->> ''cat'' c, count(*), avg((p ->> ''score'')::float8) FROM items_%s, jsonb_array_elements(doc -> ''parts'') p GROUP BY 1'),
	('list',      'SELECT o.cat, count(*), avg(o.score) FROM items_%s, jsonb_to_recordset(doc -> ''parts'') AS o(cat text, score float8) GROUP BY o.cat');

SELECT k AS every, s.n, pushed(format(s.q, k)) AS pushed, reread(format(s.q, k)) AS reread, same(format(s.q, k)) AS result
FROM unnest(ARRAY[1000, 33, 2]) k, shapes s ORDER BY k DESC, s.n;
-- at the server's own costs the plan follows the share: the scan while the rows it reads again cost less than the normal plan does, the normal plan
-- (and its answer) past a share the statistics' sample of the flagged units tells
RESET seq_page_cost;
RESET random_page_cost;
SELECT k AS every, pushed(format(s.q, k)) AS pushed, same(format(s.q, k)) AS result
FROM unnest(ARRAY[1000, 33, 2]) k, (SELECT q FROM shapes WHERE n = 'scalar avg') s ORDER BY k DESC;

DROP TABLE shapes, items_1000, items_33, items_2;
DROP FUNCTION reread(text);
DROP FUNCTION pushed(text);
DROP FUNCTION same(text);
DROP FUNCTION runq(text, bool), outcome, runs_in_scan;
DROP EXTENSION pg_sift;
