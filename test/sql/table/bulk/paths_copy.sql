-- COPY into a sift_table with declared paths packs many rows before it stages them: the text of a row's leaves and list elements is its own, not the
-- shredder's buffer that the next row overwrites. The rows of the elements are compared with the aggregate scan off and on, on both formats.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE FUNCTION gen(lo int, hi int) RETURNS TABLE (id int, doc jsonb) LANGUAGE sql AS $$
	SELECT i, jsonb_build_object('ch', 'ch_' || i % 7, 'parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || (i * 7 + j) % 5, 'score', j)) FROM generate_series(1, 1 + i % 4) j))
	FROM generate_series(lo, hi) i $$;
CREATE FUNCTION load(tbl regclass) RETURNS void LANGUAGE plpgsql AS $$
DECLARE f text := current_setting('data_directory') || '/paths_copy.dat';
BEGIN
	EXECUTE format('COPY (SELECT * FROM gen(1, 1000)) TO %L', f);
	EXECUTE format('COPY %s FROM %L', tbl, f);
END $$;
CREATE FUNCTION q(tbl regclass) RETURNS text LANGUAGE sql AS $$
	SELECT format('SELECT e.id, e.doc ->> %L, o ->> %L, count(*) FROM %s e JOIN LATERAL jsonb_array_elements(e.doc -> %L) o ON true GROUP BY 1, 2, 3', 'ch', 'cat', tbl, 'parts') $$;
CREATE FUNCTION rows_of(tbl regclass, agg bool) RETURNS TABLE (id int, ch text, cat text, n bigint) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', agg::text, true);
	RETURN QUERY EXECUTE q(tbl);
END $$;
CREATE FUNCTION wrong(tbl regclass) RETURNS bigint LANGUAGE sql AS $$
	SELECT count(*) FROM ((SELECT * FROM rows_of(tbl, false) EXCEPT ALL SELECT * FROM rows_of(tbl, true))
						 UNION ALL (SELECT * FROM rows_of(tbl, true) EXCEPT ALL SELECT * FROM rows_of(tbl, false))) d $$;

SET sift.test_create_format1 = off;
CREATE TABLE ev2 (id int, doc jsonb) USING sift_table;
SET sift.test_create_format1 = on;
CREATE TABLE ev1 (id int, doc jsonb) USING sift_table;
RESET sift.test_create_format1;
SELECT sift.table_set_paths(t, 'doc', 'doc->ch text, doc->parts[]->cat text, doc->parts[]->score float8') FROM unnest(ARRAY['ev2', 'ev1']::regclass[]) t;
SELECT load('ev2'), load('ev1');
ANALYZE ev2, ev1;

SELECT t, (SELECT count(*) FROM rows_of(t, false)) AS rows, wrong(t) AS wrong FROM unnest(ARRAY['ev2', 'ev1']::regclass[]) t;
SELECT t, (SELECT bool_or(used) FROM sift.explain_why(q(t))) AS pushed FROM unnest(ARRAY['ev2', 'ev1']::regclass[]) t;

DROP TABLE ev2, ev1;
DROP FUNCTION wrong(regclass), rows_of(regclass, bool), q(regclass), load(regclass), gen(int, int);
DROP EXTENSION pg_sift;
