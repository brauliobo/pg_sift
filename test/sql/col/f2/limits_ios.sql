-- Format major 2: a table of more than 128 columns and an index of the most leaves; index-only scans that return text, varchar, uuid and numeric columns exactly (NULL, empty, long,
-- multibyte and NaN values) with no heap fetch; the columns an index cannot return; and the generated columns PostgreSQL 18 does not store.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION ios_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text; plan text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = on; SET LOCAL enable_indexonlyscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q INTO plan;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_indexscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index <> via_heap THEN 'DIFFERENT: ' || via_index || ' / ' || via_heap
				WHEN plan !~ '"Node Type": "Index Only Scan"' THEN 'no index-only scan'
				ELSE 'same, index-only, heap fetches ' || (regexp_match(plan, '"Heap Fetches": (\d+)'))[1] END;
END $$;

-- A table of 200 columns: the index holds the ones its queries read
DO $$ BEGIN
	EXECUTE 'CREATE TABLE w200 (id int, ' || (SELECT string_agg(format('c%s int', i), ', ' ORDER BY i) FROM generate_series(1, 199) i) || ')';
	EXECUTE 'INSERT INTO w200 SELECT i, ' || (SELECT string_agg(format('i %% %s', i + 1), ', ' ORDER BY i) FROM generate_series(1, 199) i) || ' FROM generate_series(1, 3000) i';
END $$;
SELECT count(*) AS columns FROM pg_attribute WHERE attrelid = 'w200'::regclass AND attnum > 0;
CREATE INDEX w200_idx ON w200 USING sift_col (id, c1, c100, c199) WITH (key_order = 'id');
SELECT sift._test_f2_check_keys('w200_idx');
SELECT sift_helpers.f2_same('SELECT id, c199 FROM w200 WHERE c100 = 7 AND c1 = 1', true, false) AS wide_table;
VACUUM w200;
SELECT ios_same('SELECT id, c1, c100, c199 FROM w200 WHERE c199 < 5') AS wide_ios;

-- An index of 1,024 leaves (the key and 1,023 paths) is built; 1,025 is refused
CREATE TABLE wide (id int, doc jsonb);
INSERT INTO wide SELECT i, (SELECT jsonb_object_agg('k' || j, (i * 7 + j) % 1000) FROM generate_series(0, 1022) j) FROM generate_series(1, 60) i;
DO $$ BEGIN
	EXECUTE format('CREATE INDEX wide_max ON wide USING sift_col (id, doc) WITH (key_order = ''id'', paths = %L)',
				   (SELECT string_agg('doc.k' || j || ' int', ', ' ORDER BY j) FROM generate_series(0, 1022) j));
END $$;
SELECT sift._test_f2_check_keys('wide_max');
SELECT sift_helpers.f2_same('SELECT id FROM wide WHERE (doc->>''k1022'')::int = 70', true, false) AS last_leaf,
       sift_helpers.f2_same('SELECT id FROM wide WHERE (doc->>''k0'')::int < 100', true, false) AS first_leaf,
       sift_helpers.f2_same('SELECT count(*), sum((doc->>''k511'')::int) FROM wide', true, false) AS aggregate;
DO $$ BEGIN
	EXECUTE format('CREATE INDEX wide_over ON wide USING sift_col (id, doc) WITH (key_order = ''id'', paths = %L)',
				   (SELECT string_agg('doc.k' || j || ' int', ', ' ORDER BY j) FROM generate_series(0, 1023) j));
END $$;

-- 255 arrays and wildcards are accepted in one shredding spec
DO $$ BEGIN
	EXECUTE format('CREATE INDEX wide_groups ON wide USING sift_col (id, doc) WITH (key_order = ''id'', paths = %L)',
				   (SELECT string_agg(format('doc.g%s[] int4', j), ', ' ORDER BY j) FROM generate_series(1, 255) j));
END $$;
SELECT sift._test_f2_check_keys('wide_groups');
DROP TABLE wide;

-- Index-only scans return text, varchar, uuid and numeric(p, s) columns exactly
CREATE TABLE ios (id int, t text, v varchar(40), u uuid, n numeric(12, 2), pad text DEFAULT repeat('x', 300));
ALTER TABLE ios ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ios (id, t, v, u, n)
SELECT i,
	   CASE WHEN i % 11 = 0 THEN NULL WHEN i % 13 = 0 THEN '' WHEN i % 17 = 0 THEN 'caf' || chr(233) || ' ' || chr(8364) || chr(128512) || i
			WHEN i % 19 = 0 THEN repeat('long ', 3000) || i ELSE 'str ' || i END,
	   CASE WHEN i % 23 = 0 THEN NULL WHEN i % 29 = 0 THEN '' ELSE 'v' || i % 700 END,
	   CASE WHEN i % 31 = 0 THEN NULL ELSE md5(i::text)::uuid END,
	   CASE WHEN i % 37 = 0 THEN NULL WHEN i % 41 = 0 THEN 'NaN'::numeric ELSE (i * 7 % 100000) / 100.0 - 300 END
FROM generate_series(1, 4000) i;
CREATE INDEX ios_idx ON ios USING sift_col (id, t, v, u, n) WITH (key_order = 'id', return_columns = 't, v, u');
VACUUM ios;
SELECT sift._test_f2_check_keys('ios_idx');
SELECT ios_same('SELECT id, t FROM ios WHERE id < 800') AS text,
       ios_same('SELECT id, v FROM ios WHERE id BETWEEN 100 AND 900') AS varchar,
       ios_same('SELECT id, u FROM ios WHERE id < 800') AS uuid,
       ios_same('SELECT id, n FROM ios WHERE id < 800') AS numeric,
       ios_same('SELECT id, t, v, u, n FROM ios WHERE id < 1500') AS all_columns,
       ios_same('SELECT id, t, n FROM ios WHERE n > 0 AND id < 2000') AS qual_on_numeric,
       ios_same('SELECT t FROM ios WHERE id = 19') AS long_value,
       ios_same('SELECT id, n FROM ios WHERE id % 41 = 0 AND id < 400') AS nan;
SELECT id, length(t), length(v), u, n FROM ios WHERE id IN (17, 19, 41, 55) ORDER BY id;
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT id, length(t), md5(t) = md5((SELECT t FROM ios h WHERE h.id = 19 OFFSET 0)) AS same_long FROM ios WHERE id = 19;
SELECT id, n FROM ios WHERE id IN (41, 82) ORDER BY id;
RESET enable_seqscan; RESET enable_bitmapscan;

-- after writes: the rows in the write buffer and the deleted ones, read from the heap where a page is not all visible; after VACUUM none is
INSERT INTO ios (id, t, v, u, n) SELECT 10000 + i, 'new ' || i, 'nv' || i, md5((10000 + i)::text)::uuid, i / 4.0 FROM generate_series(1, 300) i;
DELETE FROM ios WHERE id % 50 = 0;
UPDATE ios SET t = 'upd ' || id, n = n + 1 WHERE id BETWEEN 100 AND 130;
SELECT ios_same('SELECT id, t, v, u, n FROM ios WHERE id < 400 OR id > 10000') ~ '^same, index-only' AS after_writes;
VACUUM ios;
SELECT ios_same('SELECT id, t, v, u, n FROM ios WHERE id < 400 OR id > 10000') AS after_vacuum;

-- a column no option names, and a bpchar and a bytea, are not returned by the index
CREATE TABLE ios2 (id int, t text, c char(6), b bytea, n numeric);
INSERT INTO ios2 SELECT i, 't' || i, 'c' || i % 90, decode(md5(i::text), 'hex'), i / 3.0 FROM generate_series(1, 2000) i;
CREATE INDEX ios2_bad ON ios2 USING sift_col (id, c) WITH (return_columns = 'c');
CREATE INDEX ios2_bad2 ON ios2 USING sift_col (id, b) WITH (return_columns = 'b');
CREATE INDEX ios2_idx ON ios2 USING sift_col (id, t, c, b, n) WITH (key_order = 'id');
VACUUM ios2;
SELECT ios_same('SELECT id, t FROM ios2 WHERE id < 500') AS text_not_named,
       ios_same('SELECT id, c FROM ios2 WHERE id < 500') AS bpchar,
       ios_same('SELECT id, b FROM ios2 WHERE id < 500') AS bytea,
       ios_same('SELECT id, n FROM ios2 WHERE id < 500') AS unconstrained_numeric,
       ios_same('SELECT id FROM ios2 WHERE id < 500') AS no_column;

-- PostgreSQL 18 does not store a virtual generated column: no index can name it, and no column of the heap is read from it
SELECT current_setting('server_version_num')::int >= 180000 AS pg18 \gset
\if :pg18
CREATE TABLE vg (id int, doc jsonb, doubled int GENERATED ALWAYS AS (id * 2) VIRTUAL, stored int GENERATED ALWAYS AS (id * 3) STORED,
				 vdoc jsonb GENERATED ALWAYS AS (doc || '{}'::jsonb) VIRTUAL);
INSERT INTO vg (id, doc) SELECT i, jsonb_build_object('a', i, 'b', 'k' || i % 5) FROM generate_series(1, 500) i;
CREATE INDEX vg_col ON vg USING sift_col (id, doubled);
CREATE INDEX vg_expr ON vg USING sift_col (id, (doubled + 1));
CREATE INDEX vg_stored ON vg USING sift_col (id, stored) WITH (key_order = 'id');
SELECT sift_helpers.f2_same('SELECT id, stored FROM vg WHERE stored < 60', true, false) AS stored_column;
SELECT sift_helpers.f2_same('SELECT id, doubled FROM vg WHERE doubled < 60', true, false) AS virtual_column_by_heap;
SELECT sift.suggest_paths('vg', 'vdoc');
SELECT sift.suggest_paths('vg', 'doc') IS NOT NULL AS suggest_stored_doc;
DROP TABLE vg;
\endif

DROP TABLE w200, ios, ios2;
DROP FUNCTION ios_same(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
