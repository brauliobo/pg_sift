-- The rows of a lateral unnest that the index cannot hold are read from the heap and taken apart a chunk of elements at a time: a list of many more elements than a chunk gives the
-- pairs the statement gives, with a LEFT JOIN, the quals of the elements, an EXISTS over them and a cast that raises far into the list, on both formats.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

\ir test/common/sql/agg/base.sql

-- the rows the scan read again from the heap (NULL: the statement raises)
CREATE FUNCTION reread(q text) RETURNS int8 LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Rows Read Again From The Heap' THEN RETURN substring(l FROM '\d+$')::int8; END IF;
	END LOOP;
	RETURN NULL;
EXCEPTION WHEN data_exception THEN
	RETURN NULL;
END $$;

-- a list whose first element has a member of another type is the heap's: 5,000 elements, more than two chunks, and a map of thousands of keys and one that is no number
CREATE TABLE hc (id int, kind text, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO hc SELECT i, 'cat_' || i % 3, jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('kind', 3)) || (SELECT coalesce(jsonb_agg(jsonb_build_object('kind', 'k' || j % 4, 'score', j % 9)), '[]')
	FROM generate_series(1, CASE WHEN i % 4 = 0 THEN 0 ELSE 4999 + i END) j), 'm', (SELECT jsonb_object_agg('m' || j, j % 7) FROM generate_series(1, 300 * i) j) || '{"zz": true}')
FROM generate_series(1, 12) i;
-- a score that raises at the end of a list of the heap, the one element of its kind
INSERT INTO hc VALUES (13, 'cat_x', jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('kind', 3)) || (SELECT jsonb_agg(jsonb_build_object('kind', CASE WHEN j = 4500 THEN 'kz' ELSE 'k1' END, 'score', CASE WHEN j = 4500 THEN '"abc"'::jsonb ELSE
	to_jsonb(j) END)) FROM generate_series(1, 4500) j)));
CREATE INDEX hc_col ON hc USING sift_col (id, kind, doc) WITH (paths = 'doc.parts[]{kind text, score float8}, doc.m.* float8');
VACUUM (FREEZE, ANALYZE) hc;

CREATE TABLE q (n int, sql text);
INSERT INTO q VALUES
	(1, $$SELECT e ->> 'kind', count(*), sum((e ->> 'score')::float8) FROM hc t CROSS JOIN LATERAL jsonb_array_elements(t.doc -> 'parts') e WHERE t.id <= 12 GROUP BY 1$$),
	(2, $$SELECT t.kind, count(*), count(e ->> 'kind') FROM hc t LEFT JOIN LATERAL jsonb_array_elements(t.doc -> 'parts') e ON e ->> 'kind' = 'k9' WHERE t.id <= 12 GROUP BY 1$$),
	(3, $$SELECT t.kind, count(*), sum(o.score) FROM hc t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') o(kind text, score float8)
		WHERE t.id <= 12 AND o.kind IN ('k1', 'k3') GROUP BY 1$$),
	(4, $$SELECT r.key, count(*), sum(r.value::float8) FROM hc t CROSS JOIN LATERAL jsonb_each_text(t.doc -> 'm') r WHERE r.key LIKE 'm1%' GROUP BY 1$$),
	(5, $$SELECT count(*) FROM hc t WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(t.doc -> 'parts') AS e(kind text) WHERE e.kind = 'kz')$$),
	(6, $$SELECT t.kind, sum((e ->> 'score')::float8) FROM hc t CROSS JOIN LATERAL jsonb_array_elements(t.doc -> 'parts') e GROUP BY 1$$),
	(7, $$SELECT t.kind, count(*) FROM hc t CROSS JOIN LATERAL jsonb_array_elements(t.doc -> 'parts') e WHERE e ->> 'kind' IN ('k0', 'kz') GROUP BY 1$$);
SELECT n, outcome(sql, false) = outcome(sql, true) AS same, runs_in_scan(sql) AS scan, reread(sql) > 0 AS heap FROM q ORDER BY n;

DROP TABLE q, hc;
DROP FUNCTION reread(text), runq(text, bool), pushed(text), outcome(text, bool), runs_in_scan(text);
DROP EXTENSION pg_sift;
