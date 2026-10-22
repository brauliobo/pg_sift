-- The rows of a unit that hold a value a path cannot store are read again from the heap, but only those that every condition on a column the stored unit
-- holds lets through: a row a key set, a rare NOT NULL or a stored comparison rejects is not fetched. A condition on the path itself, or of a shape the scan
-- cannot test on the stored unit, keeps the row. The rows and the errors are those of the normal plan; "Rows Read Again From The Heap" counts the fetches.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
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

-- 'same' when the scan and the normal plan end alike (the rows or the error), else both outcomes
CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, false); b text := runq(q, true);
BEGIN
	RETURN CASE WHEN a = b THEN 'same' ELSE format('DIFF off=%s on=%s', left(a, 200), left(b, 200)) END;
END $$;

-- the plan, and the rows the scan read again from the heap
CREATE FUNCTION plan_of(q text, OUT scan bool, OUT gather bool, OUT reread bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	scan := false; gather := false; reread := 0;
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		scan := scan OR l ~ 'Custom Scan \(SiftAgg\)';
		gather := gather OR l ~ 'Workers Planned: [1-9]';
		IF l ~ 'Rows Read Again From The Heap' THEN reread := regexp_replace(l, '\D', '', 'g')::bigint; END IF;
	END LOOP;
EXCEPTION WHEN others THEN reread := -1;
END $$;

-- the rows of the heap that hold a value the path cannot store (a container) and pass the condition
CREATE FUNCTION dirty(w text) RETURNS bigint LANGUAGE plpgsql AS $f$
DECLARE n bigint;
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE format('SELECT count(*) FROM events WHERE (%s) AND jsonb_typeof(payload -> $$k$$) IN ($$object$$, $$array$$)', w) INTO n;
	RETURN n;
END $f$;

-- the shapes that read the path: a key, a filter on the path itself, an extreme per group, and the columns of a recordset
CREATE FUNCTION e_q(w text) RETURNS text[] LANGUAGE sql AS $f$
	SELECT ARRAY[
		'SELECT payload ->> $$k$$ AS k, count(*) FROM events WHERE ' || w || ' GROUP BY 1',
		'SELECT count(*), sum(amount) FROM events WHERE ' || w || ' AND payload ->> $$k$$ = $$a$$',
		'SELECT kind, count(payload ->> $$k$$), max(payload ->> $$k$$) FROM events WHERE ' || w || ' GROUP BY kind',
		'SELECT count(*), sum(r.score) FROM events e, jsonb_to_recordset(e.payload -> $$parts$$) AS r(score float8) WHERE ' || replace(w, 'parent_id', 'e.parent_id')] $f$;

CREATE TABLE events (id int PRIMARY KEY, parent_id int, kind text, rare int, amount int, payload jsonb) WITH (autovacuum_enabled = false);

-- 5% of the documents hold a container in k (the path cannot store it); 2% a numeric string and 0.25% a non-number in the score of a part
CREATE FUNCTION ev_payload(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT jsonb_build_object('k', CASE WHEN i % 25 = 3 OR i % 100 = 11 THEN '{"x": 1}'::jsonb ELSE to_jsonb((ARRAY['a', 'b', 'c'])[i % 3 + 1]) END,
	                          'parts', jsonb_build_array(jsonb_build_object('score', CASE WHEN i % 400 = 8 THEN to_jsonb('n/a'::text) WHEN i % 50 = 7 THEN to_jsonb('7'::text) ELSE to_jsonb(i % 11 + 0.5) END)))
$$;

INSERT INTO events SELECT i, i % 240, 'cat_' || (ARRAY['a', 'b', 'c', 'd'])[i % 4 + 1], CASE WHEN i % 229 = 0 OR i % 425 = 3 THEN i END, i % 13, ev_payload(i) FROM generate_series(1, 12000) i;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, rare, amount, payload) WITH (paths = 'payload.k astext, payload.parts[]{score float8}');
VACUUM ANALYZE events;

-- a key set of 48 parents
SELECT format('parent_id IN (%s)', string_agg(p::text, ', ')) AS scope FROM generate_series(3, 239, 5) p \gset

CREATE TABLE cases (n int, label text, w text);
INSERT INTO cases VALUES
	(1, 'key set and rare NOT NULL', :'scope' || ' AND rare IS NOT NULL'),
	(2, 'rare NOT NULL', 'rare IS NOT NULL'),
	(3, 'key set and stored comparison', :'scope' || ' AND kind = $$cat_a$$'),
	(4, 'key set', :'scope'),
	(5, 'key set and test of the path', :'scope' || ' AND payload ->> $$k$$ <> $$b$$'),
	(6, 'key set or stored comparison', :'scope' || ' OR kind = $$cat_b$$'),
	(7, 'no condition', 'true');

-- each case: the rows read again are the dirty rows that pass the conditions of the stored columns, and the answers are those of the normal plan
CREATE FUNCTION run_cases() RETURNS TABLE (n int, label text, scan bool, gather bool, reread bigint, dirty bigint, rows_same text) LANGUAGE sql AS $$
	SELECT c.n, c.label, p.scan, p.gather, p.reread, dirty(c.w),
	       CASE WHEN bool_and(same(q) = 'same') THEN 'same' ELSE string_agg(same(q), ' / ') FILTER (WHERE same(q) <> 'same') END
	FROM cases c, LATERAL plan_of((e_q(c.w))[1]) p, LATERAL unnest(e_q(c.w)) q GROUP BY c.n, c.label, p.scan, p.gather, p.reread, c.w ORDER BY c.n
$$;

-- clean index, then a written one: documents updated (a text to a number, a number to a container), rows deleted, rows in the write buffer
SELECT * FROM run_cases();

UPDATE events SET payload = jsonb_set(payload, '{k}', '12') WHERE id % 97 = 0;
UPDATE events SET payload = jsonb_set(payload, '{k}', '{"y": [2]}') WHERE id % 89 = 0;
DELETE FROM events WHERE id % 61 = 0;
INSERT INTO events SELECT i, i % 240, 'cat_' || (ARRAY['a', 'b', 'c', 'd'])[i % 4 + 1], CASE WHEN i % 229 = 0 OR i % 425 = 3 THEN i END, i % 13, ev_payload(i) FROM generate_series(12001, 12600) i;
SELECT * FROM run_cases();

-- the workers of a parallel scan
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET seq_page_cost = 10000;
SELECT * FROM run_cases();

-- after a merge of the write buffer
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT * FROM run_cases();

DROP FUNCTION run_cases();
DROP FUNCTION e_q(text);
DROP FUNCTION dirty(text);
DROP FUNCTION plan_of(text);
DROP FUNCTION same(text);
DROP FUNCTION runq(text, bool);
DROP TABLE cases, events;
DROP FUNCTION ev_payload(int);
DROP EXTENSION pg_sift;
