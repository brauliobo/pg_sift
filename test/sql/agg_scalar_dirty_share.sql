-- The share of the rows that hold a value a typed path leaf cannot store (a numeric string where a float8 is declared): 0.1%, 3% and 50% of 20,000
-- rows, spread over every unit of the index. The scan reads those rows again from the heap and answers as the normal plan, whatever the share;
-- the normal plan is the one the planner prefers when the rows read again cost more than it does (sift.explain_why names the slug).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE FUNCTION runq(q text, agg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), ''[]'')::text FROM (%s) t', q) INTO r;
	RETURN r;
EXCEPTION WHEN others THEN RETURN 'ERR ' || SQLSTATE || ' ' || SQLERRM;
END $$;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, false); b text := runq(q, true);
BEGIN
	RETURN CASE WHEN a = b THEN 'same' ELSE format('DIFF off=%s on=%s', left(a, 200), left(b, 200)) END;
END $$;

CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

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
	('list',      'SELECT o.cat, count(*), avg(o.score) FROM items_%s, jsonb_to_recordset(doc -> ''parts'') AS o(cat text, score float8) GROUP BY o.cat');

SELECT k AS every, s.n, pushed(format(s.q, k)) AS pushed, reread(format(s.q, k)) AS reread, same(format(s.q, k)) AS result
FROM unnest(ARRAY[1000, 33, 2]) k, shapes s ORDER BY k DESC, s.n;
DROP TABLE shapes, items_1000, items_33, items_2;
DROP FUNCTION reread(text);
DROP FUNCTION pushed(text);
DROP FUNCTION same(text);
DROP FUNCTION runq(text, bool);
DROP EXTENSION pg_sift;
