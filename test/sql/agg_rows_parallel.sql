-- The scans that emit a row for each element of an array (a group for each row of the table, then a count(DISTINCT) over the rows) with 0, 2 and 4
-- workers: the rows are the rows of the normal plan, with rows the index cannot store, rows in the write buffer, a node that is scanned again, a
-- cursor and a generic plan. mode: parallel (workers planned), serial (SiftAgg alone), none (the normal plan); it is shown for the shapes with few
-- groups, which plan workers. A scan that makes a group of each element is priced for the groups its workers send to the leader and usually stays
-- in one process, so its mode is not shown: what is checked there is that the rows are the same whatever the workers allowed.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET sift.agg_preagg = off;

CREATE TABLE facts (id int PRIMARY KEY, parent_id int, bucket int, doc jsonb) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO facts
SELECT i, i % 23, i % 3,
       CASE WHEN i % 17 = 0 THEN NULL
            ELSE jsonb_build_object('parts', coalesce((SELECT jsonb_agg(jsonb_build_object('kind', (ARRAY['cat_a', 'cat_b', 'cat_c', 'cat_d'])[1 + (i + j) % 4],
                                                                                          'dir_a', CASE WHEN (i + j) % 5 = 0 THEN NULL ELSE (ARRAY['type_a', 'type_b'])[1 + (i + j) % 2] END,
                                                                                          'score', CASE WHEN (i + j) % 11 = 0 THEN NULL ELSE (i * j) % 10 END))
                                                       FROM generate_series(1, i % 6) j), '[]')) END
FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, parent_id, bucket, doc) WITH (paths = 'doc.parts[]{kind text, dir_a text, score float8}');
VACUUM ANALYZE facts;

SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET seq_page_cost = 10000;

CREATE FUNCTION rows_of(q text, push bool, workers int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION mode_of(q text, workers int) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_mode_workers(q, workers) $$;

-- the mode with 4 workers, then whether 0, 2 and 4 workers give the rows of the normal plan
CREATE FUNCTION check_rows(q text, show_mode bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE base text := rows_of(q, false, 0);
BEGIN
	RETURN format('%s w0 %s w2 %s w4 %s, %s rows', CASE WHEN show_mode THEN mode_of(q, 4) ELSE '-' END,
				  CASE WHEN rows_of(q, true, 0) = base THEN 'same' ELSE 'DIFF' END,
				  CASE WHEN rows_of(q, true, 2) = base THEN 'same' ELSE 'DIFF' END,
				  CASE WHEN rows_of(q, true, 4) = base THEN 'same' ELSE 'DIFF' END,
				  (SELECT count(*) FROM regexp_split_to_table(coalesce(base, ''), E'\n')));
END $$;

CREATE TABLE shapes (n serial, name text, q text, show_mode bool);
INSERT INTO shapes (name, q, show_mode) VALUES
	('cells', 'SELECT f.id, f.parent_id, p.kind, p.dir_a, avg(p.score) AS val, count(*) FILTER (WHERE p.score > 5) AS big
	           FROM facts f CROSS JOIN LATERAL jsonb_to_recordset(f.doc -> $$parts$$) AS p(kind text, dir_a text, score float8)
	           GROUP BY f.id, f.parent_id, p.kind, p.dir_a', false),
	('breakdown', 'SELECT p.kind, f.parent_id, count(*) AS n, sum(p.score) AS s, min(p.score) AS lo, max(p.score) AS hi
	               FROM facts f CROSS JOIN LATERAL jsonb_to_recordset(f.doc -> $$parts$$) AS p(kind text, dir_a text, score float8)
	               WHERE p.dir_a IS NOT NULL GROUP BY p.kind, f.parent_id', true),
	('distinct', 'WITH spans AS (SELECT * FROM (VALUES ($$a$$, 0), ($$b$$, 1), ($$c$$, 2)) v(label, bucket)),
	                   cells AS (SELECT f.id, f.parent_id, f.bucket, p.kind, avg(p.score) AS val
	                             FROM facts f CROSS JOIN LATERAL jsonb_to_recordset(f.doc -> $$parts$$) AS p(kind text, dir_a text, score float8)
	                             GROUP BY f.id, f.parent_id, f.bucket, p.kind)
	              SELECT c.kind, v.label, count(DISTINCT c.id) AS ids, count(DISTINCT c.parent_id) AS parents, avg(c.val) AS val
	              FROM spans v LEFT JOIN cells c ON c.bucket = v.bucket GROUP BY c.kind, v.label', false),
	('total', 'SELECT count(*) AS n, count(DISTINCT f.id) AS ids, avg(p.score) AS val
	           FROM facts f CROSS JOIN LATERAL jsonb_to_recordset(f.doc -> $$parts$$) AS p(kind text, dir_a text, score float8)', true);

CREATE FUNCTION check_all(label text) RETURNS TABLE (stage text, shape text, result text) LANGUAGE sql AS $$
	SELECT label, name, check_rows(q, show_mode) FROM shapes ORDER BY n
$$;

SELECT * FROM check_all('clean');

-- rows no typed leaf can store (a string, a JSON null, a missing key), read again from the heap
UPDATE facts SET doc = jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('kind', 'cat_a', 'dir_a', 'type_a', 'score', '7'))) WHERE id % 211 = 5;
UPDATE facts SET doc = '{"parts": [{"kind": null, "score": null}, {"dir_a": "type_b"}]}' WHERE id % 401 = 7;
SELECT * FROM check_all('dirty, not vacuumed');
VACUUM facts;
SELECT * FROM check_all('dirty, vacuumed');

-- rows in the write buffer and rows deleted since the index was built
INSERT INTO facts SELECT i, i % 23, i % 3, jsonb_build_object('parts', jsonb_build_array(jsonb_build_object('kind', 'cat_b', 'dir_a', 'type_a', 'score', i % 10),
                                                                                         jsonb_build_object('kind', 'cat_d', 'dir_a', NULL, 'score', i % 7)))
FROM generate_series(20001, 20600) i;
DELETE FROM facts WHERE id % 97 = 3;
SELECT * FROM check_all('write buffer and deletes');

-- a node scanned again for each row of an outer relation, a cursor and a generic plan
CREATE FUNCTION rescan_rows(workers int, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	SELECT string_agg(x::text, E'\n' ORDER BY x::text) INTO r
	FROM (SELECT g.b, s.kind, s.n, s.ids
	      FROM generate_series(0, 3) g(b),
	           LATERAL (SELECT p.kind, count(*) AS n, count(DISTINCT f.id) AS ids
	                    FROM facts f CROSS JOIN LATERAL jsonb_to_recordset(f.doc -> 'parts') AS p(kind text, dir_a text, score float8)
	                    WHERE f.bucket = g.b GROUP BY p.kind) s) x;
	RETURN r;
END $$;
SELECT rescan_rows(0, true) = rescan_rows(0, false) AS rescan_w0, rescan_rows(2, true) = rescan_rows(0, false) AS rescan_w2,
       rescan_rows(4, true) = rescan_rows(0, false) AS rescan_w4;

CREATE FUNCTION cursor_rows(q text, workers int, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE c refcursor; r record; out text := '';
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	OPEN c SCROLL FOR EXECUTE format('SELECT * FROM (%s) x ORDER BY x::text', q);
	FETCH ABSOLUTE 5 FROM c INTO r; out := out || r::text;
	FETCH NEXT FROM c INTO r; out := out || r::text;
	FETCH PRIOR FROM c INTO r; out := out || r::text;
	FETCH LAST FROM c INTO r; out := out || r::text;
	FETCH FIRST FROM c INTO r; out := out || r::text;
	CLOSE c;
	RETURN out;
END $$;
SELECT s.name, cursor_rows(s.q, 2, true) = cursor_rows(s.q, 0, false) AS cursor_w2, cursor_rows(s.q, 4, true) = cursor_rows(s.q, 0, false) AS cursor_w4
FROM shapes s ORDER BY s.n;

SET plan_cache_mode = force_generic_plan;
PREPARE by_bucket (int) AS
	SELECT p.kind, f.parent_id, count(*) AS n, count(DISTINCT f.id) AS ids, avg(p.score) AS val
	FROM facts f CROSS JOIN LATERAL jsonb_to_recordset(f.doc -> 'parts') AS p(kind text, dir_a text, score float8)
	WHERE f.bucket = $1 GROUP BY p.kind, f.parent_id;
CREATE FUNCTION generic_rows(b int, workers int, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	EXECUTE format('CREATE TEMP TABLE g_rows AS EXECUTE by_bucket(%s)', b);
	SELECT string_agg(g::text, E'\n' ORDER BY g::text) INTO r FROM g_rows g;
	DROP TABLE g_rows;
	RETURN r;
END $$;
SELECT b, generic_rows(b, 2, true) = generic_rows(b, 0, false) AS generic_w2, generic_rows(b, 4, true) = generic_rows(b, 0, false) AS generic_w4
FROM generate_series(0, 2) b;
RESET plan_cache_mode;
DEALLOCATE by_bucket;

DROP TABLE facts, shapes;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
DROP FUNCTION check_all, check_rows, cursor_rows, generic_rows, mode_of, rescan_rows, rows_of;
